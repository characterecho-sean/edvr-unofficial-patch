#!/usr/bin/env python3
"""Find the log a flight just wrote, and say whether it is the build you made.

    python tools/edvr_log.py --target steam --version
    python tools/edvr_log.py --target steam --expect-build HEAD
    python tools/edvr_log.py --target steam --grep "Stats\\[4[0-9]\\]"
    python tools/edvr_log.py --target steam --tail 80
    python tools/edvr_log.py --target frontier --tally vh
    python tools/edvr_log.py --target frontier --tally vh --frame 1
    python tools/edvr_log.py --target steam --tally pose
    python tools/edvr_log.py --target frontier --tally periodic --expect-build HEAD
    python tools/edvr_log.py --target frontier --tally periodic --window-ms 250
    python tools/edvr_log.py --target frontier --tally periodic --infer-runs
    python tools/edvr_log.py --target frontier --camera-census --expect-build HEAD
    python tools/edvr_log.py --target frontier --maps-sharp --expect-build HEAD
    python tools/edvr_log.py --target frontier --vscreen-fit --expect-build HEAD
    python tools/edvr_log.py --target epic --flat-upscale --expect-build HEAD
    python tools/edvr_log.py --target frontier --vr-supersampling --expect-build HEAD
    python tools/edvr_log.py --target frontier --ui-composites --expect-build HEAD
    python tools/edvr_log.py --target frontier --terrain-checkerboard --expect-build HEAD
    python tools/edvr_log.py --target frontier --route-curve --expect-build HEAD
    python tools/edvr_log.py --target steam --map-bounce --expect-build HEAD
    python tools/edvr_log.py --target steam --freezes --expect-build HEAD
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

--tally pose tables the pose-gap diagnostic: every `pose gap:` line in the RUNTIME log (it reads --tag openxr unless you name a tag or a file) by
caller (thread, return address): how far a head pose is located from the drawn frame's display time, how far it is turned from the drawn pose, and the
correlation of that angle with head speed across the windows (docs\terrain-culling.md). Exit 1 when the log has no such line.

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

--camera-census reads a VR flight flown with advanced.vr_camera_census = on
(design doc section 82). It prints the census's 5 s lines, the camera table, each
camera's role read from the KIND OF EACH of its logged calls (a camera object is
no identity: one was the left eye camera in the cockpit and is the world's kind-3
camera on foot; a second projection on the same object is its own role), each
logged on-foot frame's call sequence reduced to runs of (kind, caller, tone),
the eye composite draws' own constant-buffer rows, and then the OFFLINE JOIN: each
eye draw's rows matched to the rows the composer produced for every logged call
(equal within 1e-5). The cameras that match are the eye cameras; the report says
which of the six candidate signals (A call signature, B content join, C place
against the tone draw, D caller, E tangents against the advertised eye frusta,
F view) tells them from the world's camera. It ends with the STAGE 2 VERDICT, PASS
/ WARN / STOP lines on the injected flight (the world route's phase in the kind-3
cameras): (i) the eye rows must not move (|leak| below 1e-6 NDC while the frame's
phase is non-zero), (ii) the kind-3 calls' rows must carry the phase, (iii) only
kind 3 is injected, (iv) nothing ran off-thread or unreadable, (v) the roles, (vi)
nothing was injected on a frame whose window the route had shut. The verdict reads
the route's `vr world route 5s:` and `vr world route inject 5s:` lines and a few of
its own log lines too; a log that predates stage 2 gets n/a lines, never a crash.
With the census key on, the route also prints a refusal-census line a window
(`vr world route refusal 5s:`, the stage 2 experiment build; with the steady-detail key on, which
is its default, the line is printed with the census off as well, carrying only the depth check's
frame counts: such a window is `census-off`, never a census that failed): the report gets a section
with the share of the treated pixels whose history the resolver's prep refused, per
window and by cause (stale slot, masked record, ...), the state of the steady-detail
key and of the refusal view in each window, and totals by key state; (v) also checks
the weapon's role (inj-fp against the fold-in's mode counts and the struct's fov range).
A log with no census lines exits 1; the verdict never changes the exit code (read
its lines).

The census's EPISODES (design-world-camera-motion-2026-09-30.md section 6, Phase 0)
add three sections before the verdict: `== episodes ==` (per episode, one frame
sampled 30 frames after a trigger, aboard frames included: the trigger, the journal,
GuiFocus and the naming, the calls by kind and caller, the printed calls, the join of
each depth's first draw to the calls that composed its b1 rows, and the temporal
pass's chosen rows against the calls' view axes, then the facts for H1, H2 and H3),
`== on-foot naming runs ==` (the run-length histogram of the 5 s windows summed) and
`== the detour's CPU ==` (the observer halves' sampled cost). A log with none of those
lines reports exactly as before.

--maps-sharp reads a flight with experimental.on_foot_maps_sharp = on (design-world-
camera-motion-2026-09-30.md, Phase 1). It prints each map or menu the UI layer held as
a panel period (the TAKES line and the HANDS BACK line that closed it: when, how many
frames, how many eyes went through the layer-only door and how many kept the upscaler,
and why it handed back), the `on foot maps sharp 5s:` windows summed, whether the VR
world route let go with the gate and owned the world again after, and a PASS / WARN /
STOP verdict. Unlike the other readers its exit code carries the verdict: 0 for PASS
or WARN, 1 for STOP, 3 when the log has no line of the feature (the key was off, the
UI layer was not live, or the build predates it).

--vscreen-fit reads a flight with fix.vscreen_res_width = auto (design doc section 82,
the "vscreen auto-fit" entry). It prints the launch's rule line (`vScreen resolution:
auto = N wide: rule=fitted|legacy ...`: the rule that chose the width and why, from
which footprint, and which world-route condition failed when it is legacy), the
width the panel patch applied, the footprint instrument's arming line and every
`vscreen footprint 30s:` window (samples on foot and elsewhere, the screen's width in
eye pixels, its range and shape, the footprint at panel distance 1, the session's
head-on floor (its p10), what is stored for the next launch and the width it would
fit), then the verdict lines, PASS / WARN / STOP / n/a: RULE (the width follows the
rule's own tokens, to a step of 16, and is what was applied), INSTRUMENT (it ran, saw
the composite and read its sources), ON FOOT, STABLE (the windows' widths at distance
1 agree to 8%) and SHAPE (the median window's shape is 16:9 in the eye's own pixels,
from the log's game FOV when it carries one), which judge only the windows with 12 or
more on-foot samples, DISTANCE LAW (the footprint at distance 1 agrees across panel
distances), CALIBRATION (the session's stored p10 against the 5006 px head-on
footprint behind Sean's 3504 at distance 0.7 on a 4032 px eye, within 5%) and STORED
(a WARN when any window carries save-failed=N: a save of the footprint that did not
reach the file; the log's `vscreen footprint: SAVE FAILED (Win32 error N)` line, the
first three of a session, names the error). A log with none of these lines exits 1; the
verdict never changes the exit code (read its lines).

--flat-upscale reads a flat-profile flight (design doc section 83): the game's final copy admitted by
its structure, so DLSS, FSR and TAA resolve below the output (Elite's supersampling under 1.0) whatever bloom,
depth of field and the tone variant do. It prints the key line, each `flat route:` (R, E, D), the first
admission and every decline (`flat copy structure:`), each `flat copy structure 5s:` window (the game's copies
split into the whitelist's, the structure's admissions, its declines by cause, no scene, a render size that does
not fit, the HDR route's, and the key off's; the scene, output and source sizes; the longest chain of R-sized image
passes between the scene and the copy), the stand-down and F8 warning lines with the render size's words, and the
refusals summed, then PASS / WARN / STOP / n/a lines: KEY, ADMISSION (did the instrument run), TREATED, UPSCALE
(below the output, admitted or selected, treated), TONE REFUSALS, STAND-DOWN (no-3d-scene is a silent startup; a
render size is the user's resolution; a tone refusal with declines is a game anti-aliasing chain), F8 WARNING (the
false startup warning is a STOP), CHAIN (a game anti-aliasing filter the structure declines) and ADVICE (the old
supersampling paragraph must be gone). --vr-supersampling reads a VR flight: the `vr supersampling:` line (Elite's
Supersampling below 1, from the measured render size against the eye texture), vScreen's own adoption line it
follows and the menu's note that the headset notice was queued, with NOTICE / CONSISTENT / HEADSET / FLAT lines
(a flat log carrying the notice is a STOP). Neither verdict changes the exit code (read its lines).

--ui-composites reads one flight's census of the interface composites the UI layer left in the scene
(docs/ui-layer-2026-09-23.md, "2026-10-01: Disable GUI effects"). The layer names an interface draw by its
vertex shader, and a draw into the lit HDR eye that samples an interface surface under a vertex shader no
family names reached no decision and no refusal line: user 5's cockpit panels, with Elite's Disable GUI
effects on, stayed in the scene upscaled with it. Every 30 s the layer now prints one `composites left in
the scene` line, zeros included: how many composite draws of how many were left, a frame's worth, and each
pair left by vertex shader, pixel shader and family (`no family` = nothing named it), or NOT COUNTED when the
interface depth pass was off. The report lays the windows out and judges INSTRUMENT (the census ran: a log
with the layer's 30 s lines and none of these is a build before it or a census that never ran), DETECTOR (a
NOT COUNTED window), UNCLAIMED (a composite no family names, left in the scene: 0.5 draws a frame or more is a
STOP, less a WARN), NAMED (a family named it and the layer did not take it: a WARN, the layer's `left in the
game's frame` line says why), OVERFLOW (more different pairs than the table names) and TAKEN (every composite
went into the layer). Its exit code carries the verdict: 0 for PASS or WARN, 1 for STOP, 3 when the log has
no census line (main() answers 2 for a wrong build before this runs).

--terrain-checkerboard reads a VR flight's Elite terrain checkerboard rendering notice (design doc section 84): the
worker's start line, one line for the first read and each change (ON, OFF or unknown with its reason, read from the
game's active graphics preset on a worker thread), the log's bound line and the menu's note that the headset notice
was queued as a toast, once per raise, with READER / NOTICE / CHANGES / LIMIT / HEADSET / FLAT lines (a flat log
carrying them is a STOP). A log with none of the lines means the reader never started, which is not "read, off": an
OFF or an unknown read writes a line too. Its verdict does not change the exit code either.

--route-curve reads a flight with the curved VR world route (design doc section 82,
"The curved route": fix.panel_curvature above 0, experimental.temporal_aa_on_foot_world
= auto). It reads the route's `vr world route 5s:` lines by token (`curve=`: off,
pending, stood-down or curvature/columns/gain; `curve-reissues=`: the strips the layer
drew), the OWNS lines, the `panel curvature:` notes and the layer's `vr world route
layer:` refusals, prints the windows by ownership and curve, and judges CURVE (what
the owned windows said, and any change), RE-ISSUE (strips drawn equal eye takes; takes
with no strip under a curve are a flat screen under a curved game draw: STOP), READY
(pending past two windows), STOOD DOWN (with the note that names the cause), STALE
BUILD (a `curved-screen` refusal, which only the build before the curved route writes:
STOP), OWNS (its sentence names the curve the next window shows) and FAULT. Like
--maps-sharp its exit code carries the verdict: 0 for PASS or WARN, 1 for STOP, 3 when
the log has no route line. A log from before the curved route has no curve tokens:
that is a WARN which says so, never a PASS.

--map-bounce reads issue 65's `flat map bounce 5s:` windows and one-shot decision.
A missing line means the instrument never ran; pending windows warn, and fail-safe
trips or verification mismatches stop. It checks verification coverage and measured
bank-read speed when bounce is off. Exit 0 for PASS or WARN, 1 for STOP, 3 when the
instrument never ran.

--freezes reads one flight's freeze diagnostics (issue 63). A frame or runtime cycle of
250 ms or more is a freeze and always gets a line, so the report lays them out from both
logs (the runtime log is paired as --tally periodic pairs it; --runtime-file names one;
every time printed is local): FREEZES (each FREEZE line of the graphics log with its
LONG FRAME line, the runtime's native_long_cycle line of that sequence and its main
phase, the stall sampler's samples inside it with their age and owner, and the GPU
census's kept stall of that sequence), RUNTIME CYCLES of 250 ms or more (marked
`runtime only` when no FREEZE line goes with one), COUNTS (the latest counts line of each
half, by size bucket), WORST (the latest worst-few set of each half), STALL SAMPLER
(armed or off, samples by owner module, failed samples by reason, the longest suspension,
EDVR code on the stack) and GPU CLOCK; then PASS / WARN / STOP lines: INSTRUMENT (the
counts line is the proof the logging ran), UNWRITTEN FREEZES (over_250ms_unwritten above
0 is a STOP), FREEZE LINES (a runtime cycle of 250 ms or more with no FREEZE line, beside
a graphics counts line, is a STOP; a FREEZE line with no LONG FRAME line a WARN), SAMPLER,
SUSPENSION (over 5000 us a WARN, over 50000 a STOP), FREEZE TEST (a log written with
advanced.freeze_test_ms: its deliberate sleep must be a FREEZE with its LONG FRAME line, runtime
cycle and stall samples, EDVR's own code on the stack) and NO FREEZE. A value a log does not
carry prints `-`, never 0. It reads the headset-lock arc too (docs/headset-lock-vdxr-2026-
10-02.md): SLOW REGIME (a run of seconds with the frame rate under 40% of the display's for 5
s or more, the owner that held most of each frame, and the graphics memory then; read from
the build's own native_slow_regime lines, or, for a v0.18.0 log, put back together from its
LONG FRAME, native_long_cycle, FREEZE and vScreen totals lines and its native_submit_phases
windows, and printed as RECONSTRUCTED), END-FRAME EPISODES (the xrEndFrame calls of 3 display
periods or more), VENDOR EVENTS (the vendor's xrPollEvent events), VRAM (the graphics memory
against the OS's budget) and SLOW TEST (a log written with advanced.slow_test_ms: its
deliberate hold must be a regime held by xrEndFrame with its whole chain). A log that holds a
slow regime the vendor runtime, EDVR's copy or EDVR's own work holds gets a SLOW verdict that
names it and its owner, never PASS or WARN; a regime the game's own frame holds is INFO when
the runtime says its frames were a load and WARN when they were the game's scenes, and one with
no named owner is a WARN: none of those sets SLOW. Like --route-curve its exit code carries
the verdict: 0 for PASS or WARN, 1 for STOP, 2 for a wrong build (the runtime log is checked
too), 3 when the log holds none of the freeze lines and no slow regime (a build from before the
freeze logging), 4 for a slow regime the vendor runtime or EDVR holds (and no STOP).

Exit 0 when a log was read, 1 when none was found (or --camera-census found no
census line, or --vscreen-fit no auto-fit line, or --flat-upscale no flat line, or
--vr-supersampling no VR line, or --terrain-checkerboard no terrain checkerboard line), 2 when --expect-build did not match (--tally periodic
and --freezes check the runtime log against it too). --maps-sharp's, --route-curve's,
--ui-composites's and --freezes's codes for a log they read are their own (above): 0, 1 and
3 mean a verdict, not "no log".
"""

import argparse
import bisect
import calendar
import datetime
import math
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


def _mean_sd(values):
    n = len(values)
    if not n:
        return None, None
    mean = sum(values) / float(n)
    sd = statistics.stdev(values) if n > 1 else None
    return mean, sd


def _mean_sd_cell(ms, signed, digits):
    mean, sd = ms
    if mean is None:
        return "-"
    fmt = "%%%s.%df" % ("+" if signed else "", digits)
    return (fmt % mean) + (" +/- " + ("%.*f" % (digits, sd)) if sd is not None else "")


# --tally pose: the pose-gap diagnostic (src/openxr/pose_gap.h writes every line, and tools\openxr_pose_test holds tools\pose_gap_fixture.log, which this
# script's --self-test reads, to exactly what it writes). The lines are in the RUNTIME log, one per caller every 60 s:
#
#   pose gap: tid T calls n from exe+0xRVA prediction p ms target-minus-display mean a ms (min b max c) angle-to-drawn mean d max e deg
#       head f deg/s waitgetposes w failed k[ fallback f]
#   pose gap: more than 16 callers in a window; N calls not counted
#
# One line is one caller (thread, return RVA) over a window of WaitGetPoses; "target-minus-display" is the instant its pose was located at less the
# latest frame's predictedDisplayTime, "angle-to-drawn" the angle between the pose it got and the pose that frame was drawn with, "head" the render
# pose's angular speed. Any of the three reads n/a when none of the window's calls had it. Elite's own request for "now" is answered at the display
# time since the head-pose fix (docs\terrain-culling.md), so for it the gap reads 0; a caller that passes a real prediction, or is not in the game's
# image, is located at the wall clock and shows the true lag.

POSE_GAP_RE = re.compile(
    r"pose gap: tid (?P<tid>\d+) calls (?P<calls>\d+) from (?P<frm>exe\+0x[0-9A-Fa-f]+|outside|\?) "
    r"prediction (?P<pred>[-+0-9.]+) ms target-minus-display "
    r"(?:mean (?P<gm>[-+0-9.]+) ms \(min (?P<gmin>[-+0-9.]+) max (?P<gmax>[-+0-9.]+)\)|n/a) "
    r"angle-to-drawn (?:mean (?P<am>[-+0-9.]+) max (?P<amax>[-+0-9.]+) deg|n/a) "
    r"head (?:(?P<head>[-+0-9.]+) deg/s|n/a) waitgetposes (?P<waits>\d+) failed (?P<failed>\d+)(?: fallback (?P<fb>\d+))?")
POSE_GAP_MORE_RE = re.compile(r"pose gap: more than (?P<limit>\d+) callers in a window; (?P<dropped>\d+) calls not counted")


def parse_pose_gap(text):
    """Every `pose gap:` line, in order. Returns (windows, notes): a window is a dict of tid, calls, frm, pred (ms), gm/gmin/gmax (ms) and am/amax
    (deg) and head (deg/s) each None when the line said n/a, waits, failed, fb; a note is the text of an over-the-limit line."""
    windows = []
    notes = []
    for line in text.splitlines():
        m = POSE_GAP_RE.search(line)
        if m:
            def num(name):
                v = m.group(name)
                return float(v) if v is not None else None
            windows.append({
                "tid": int(m.group("tid")), "calls": int(m.group("calls")), "frm": m.group("frm"), "pred": float(m.group("pred")),
                "gm": num("gm"), "gmin": num("gmin"), "gmax": num("gmax"), "am": num("am"), "amax": num("amax"), "head": num("head"),
                "waits": int(m.group("waits")), "failed": int(m.group("failed")), "fb": int(m.group("fb") or 0)})
            continue
        if POSE_GAP_MORE_RE.search(line):
            notes.append(line.split("pose gap:", 1)[1].strip())
    return windows, notes


def _pearson(pairs):
    """Pearson's r over (x, y) pairs, None when there are fewer than three or either side does not vary."""
    n = len(pairs)
    if n < 3:
        return None
    mx = sum(p[0] for p in pairs) / float(n)
    my = sum(p[1] for p in pairs) / float(n)
    sxx = sum((p[0] - mx) ** 2 for p in pairs)
    syy = sum((p[1] - my) ** 2 for p in pairs)
    if sxx <= 1e-12 or syy <= 1e-12:
        return None
    return sum((p[0] - mx) * (p[1] - my) for p in pairs) / math.sqrt(sxx * syy)


def tally_pose(windows):
    """The --tally pose numbers: one row per (thread, return address), by thread, each over that caller's windows: n windows, calls, failed, fallbacks,
    and (mean, sd) pairs of the windows' mean gap (ms), mean angle (deg) and head speed (deg/s), the range of the angle means, and r, the correlation
    of a window's mean angle with its head speed (None when it cannot be told)."""
    groups = {}
    for w in windows:
        groups.setdefault((w["tid"], w["frm"]), []).append(w)
    rows = []
    for key in sorted(groups):
        ws = groups[key]
        angles = [w["am"] for w in ws if w["am"] is not None]
        rows.append({
            "tid": key[0], "frm": key[1], "n": len(ws), "calls": sum(w["calls"] for w in ws),
            "failed": sum(w["failed"] for w in ws), "fallbacks": sum(w["fb"] for w in ws),
            "gap": _mean_sd([w["gm"] for w in ws if w["gm"] is not None]),
            "angle": _mean_sd(angles), "angle_range": (min(angles), max(angles)) if angles else None,
            "head": _mean_sd([w["head"] for w in ws if w["head"] is not None]),
            "r": _pearson([(w["am"], w["head"]) for w in ws if w["am"] is not None and w["head"] is not None])})
    return rows


def print_pose_tally(text):
    """The --tally pose report. Returns the process exit code."""
    windows, notes = parse_pose_gap(text)
    for note in notes:
        print("[edvr] pose gap: %s" % note)
    if not windows:
        print("[edvr] no `pose gap:` lines in this log. The diagnostic is always on and writes one per caller every 60 s from the runtime's "
              "WaitGetPoses; they are in the runtime log (--tag openxr), not the graphics log, and need a build with the pose-gap diagnostic.")
        return 1
    rows = tally_pose(windows)
    print("[edvr] tally pose: %d window line(s) over %d caller row(s)" % (len(windows), len(rows)))
    print("[edvr] per caller (thread, return address), mean +/- sd over that caller's windows of each window's mean:")
    print("%7s %-14s %4s %7s %6s  %-20s %-22s %-11s %s"
          % ("tid", "from", "n", "calls", "failed", "gap ms (target-disp)", "angle deg (to drawn)", "head deg/s", "r(angle,head)"))
    for r in rows:
        print("%7d %-14s %4d %7d %6d  %-20s %-22s %-11s %s"
              % (r["tid"], r["frm"], r["n"], r["calls"], r["failed"], _mean_sd_cell(r["gap"], True, 2), _mean_sd_cell(r["angle"], False, 3),
                 ("%.1f" % r["head"][0]) if r["head"][0] is not None else "n/a", ("%+.2f" % r["r"]) if r["r"] is not None else "n/a"))
    for r in rows:
        if r["fallbacks"]:
            print("[edvr] tid %d: %d call(s) could not be located at the display time (no frame yet, or no positive display time) and were located at "
                  "now + prediction." % (r["tid"], r["fallbacks"]))
    for r in rows:
        if r["gap"][0] is None:
            continue
        spread = ""
        if r["angle_range"] is not None:
            spread = " (%.3f to %.3f across windows)" % r["angle_range"]
        print("[edvr] %s, tid %d: its pose is located %+.2f ms from the drawn frame's display time, and is turned %s deg from the drawn pose%s; "
              "angle against head speed r = %s over %d window(s)."
              % (r["frm"], r["tid"], r["gap"][0], ("%.3f" % r["angle"][0]) if r["angle"][0] is not None else "n/a", spread,
                 ("%+.2f" % r["r"]) if r["r"] is not None else "n/a", r["n"]))
    print("[edvr] Elite's own \"now\" request should read a gap of 0 ms and a small angle that does not grow with head speed; a caller with a real "
          "prediction, or outside the game's image, shows the true lag (before the fix Elite's was 41-44 ms and up to 4.1 degrees, r = +0.98).")
    return 0

# --camera-census: the VR camera census (advanced.vr_camera_census), one flight
# in the VR profile with the key on. The question it answers (design doc section
# 82): which signal tells an eye view's camera from the world's at the game's
# view-constant refresh, so the on-foot world route can put its sub-pixel phase into
# the world cameras (stage 2) and leave the eyes alone, and whether it did. The lines
# are written by src/d3d11/vr_camera_census_core.h (every line's text lives there;
# tools\vr_camera_census_test pins it and holds tools\camera_census_fixture.log,
# which this script's --self-test reads, to exactly what those formatters write):
#
#   vr camera census 5s: frames=.. calls=.. posts=.. off-thread=.. stale=.. inj-calls=.. kinds=.. callers=..
#       ... tone-frames=.. on-foot-frames=.. foot=yes|no|unknown|off ...
#   vr camera census: camera=0xPTR kind=K caller=+0xRVA thread=owner aspect=..
#       near=.. far=.. fov=.. bound=(..,..) offcentre=(..,..) viewport=(..,..)
#       tan=(l,r,b,t) view=0xPTR vctx=0xPTR first-call=N draw=D tone=before|after|none frame=F
#   vr camera census: changed: camera=0xPTR frame=F n=N <field>=<old>-><new> ...
#   vr camera census: sequence frame=F index=I/3 foot=.. phase=X,Y|- calls=N recorded=R truncated=T
#   vr camera census: call frame=F n=N camera=0xPTR kind=K caller=+0xRVA draw=D
#       tone=.. inj=0|1 role=scene|fp|aux|- fl=0xPRE>0xPOST view=0xPTR rows=[16 floats]   (rows 270..273 the composer wrote)
#   vr camera census: eye=E frame=F foot=.. phase=X,Y|- draw=D b1=0xPTR first=N bytes=N rows=[16] meas=(..,..)
#   vr camera census: eye-geometry eye=E frame=F seq=S frustum=[l,r,d,u] shift=(..,..)
#       expect=(..,..) expect-shifted=(..,..) leak=(..,..)
#   vr camera census: other-thread tid=T camera=0xPTR kind=K caller=+0xRVA calls=N
# the EPISODES (Phase 0; one frame sampled 30 frames after a trigger; every line is written by vrCensusPrintEpisode and the formatters below it):
#   vr camera census: episode frame=F n=N/10 trigger=key-on|foot:no>yes|naming:named>unnamed|gui:0>6 armed=F foot=.. gui=N|- named=0|1 phase=X,Y|-
#       calls=N recorded=R printed=P kinds=0:N,.. callers=+0xRVA:N,..,+more:N       (the call lines that follow are its calls)
#   vr camera census: call frame=F n=N ...                                         (the same call line, up to 120 an episode, the matched ones first)
#   vr camera census: pass-rows ep=N frame=F valid=1|0 bound=1|0|- rows=[12 floats]|- axes-match=n,n+K|- how=identity|transpose|- nearest=n|- diff=..
#   vr camera census: join ep=N sig=S depth=screen|eye eye=0|1|- size=WxH draw=D draws=N vs=0x.. ps=0x.. dw=yes|no|- b1=0xPTR|- first=N bytes=N
#       rows=read match=n,n+K|-  |  rows=- why=skip|no-b1|b1-too-small|map|staging
#   vr camera census: join-rows ep=N sig=S rows=[16 floats]                          (a signature that read its rows)
#   vr camera census: join-more ep=N signatures=N draws=N                            (signatures the table could not keep)
# and, with each 5 s line, three companions (the 5 s line itself is full at 400 characters):
#   vr camera census: episodes windows=W taken=N/10 triggers=N skipped=N state=idle|armed|live [trigger=.. armed=F sample=F]
#   vr camera census: runs windows=W frames=N named=1:n,2:n,3:n,4-8:n,9-30:n,31-89:n,90+:n unnamed=.. longest=named:N,unnamed:N open=named:N|unnamed:N|-
#   vr camera census: detour windows=W every=16 timed=observer-halves frames=N calls=N sampled=N est-ms-frame=X|-
#       obs-calls=N obs-sampled=N obs-pre-us=mean/max|- obs-post-us=.. inj-calls=N inj-sampled=N inj-pre-us=.. inj-post-us=..
# and, from the world route (src/d3d11/vr_world_route.cpp), two lines a 5 s window, back to back:
#   vr world route 5s: ... last=<verdict> jitter=on|off|idle|unnamed|no-hook|fault phase=X,Y rows=X,Y fp-mode=a/b/c
#       last-trigger=.. target=WxH hdr=WxH selection=..
#   vr world route inject 5s: inj-scene=N inj-fp=N inj-refused=N warming=N aux=N after=N unsupported=N other-kind=N
#       unreadable=N off-thread=N write-fail=N inj-kinds=none|3:N[,other:N] pair-checked=N pair-bad=N
#       inj-unnamed=N inj-shut=N
# (`unnamed`: the route is warming or owning but the frame before named no source for the screen, a map or a menu, so its camera
# window was shut; not a fault. `inj-shut` counts calls injected on a frame whose window was shut and must be 0; `inj-unnamed`
# counts frames whose window was open and that named nothing: one per world-to-map change is expected.) The route's own log
# lines the verdict quotes are listed in ROUTE_EVENT_MARKERS (`STOP at frame=`, `RELEASED the world at frame=`, ...).
# The inject line ends with `fov=<narrowest>..<widest>` (radians: the struct's field of view over the screen views of the window,
# two values when the first-person camera's tighter one is in it) or `fov=-` (no frustum was read), and, with the census key on or
# the steady-detail key on, a third line follows the two (src/d3d11/vr_world_route_math.h vrWorldFormatRefusalWindow):
#   vr world route refusal 5s: census=on|off every=N treated=N asked=N sampled=N read=N dropped=N size=WxH pixels=N refused=N
#       refused-pct=X stale-refused=N masked=N corrupt=N sentinel=N unreprojectable=N camera=N range=N depth=N weapon=N other=N
#       stale-kept=N depth-check=RAN/SKIPPED steady-detail=on|off view=on|off skinned-joined=N
# (`skinned-joined` is F2 on foot, the only ACCEPTED class the census counts: the skinned pixels the prep took an exact motion for from the on-foot
# source's target 7. It is not part of `refused`. 0 with a character in view means the route never read E; absent in a build before F2 on foot.
# `pixels` is what the read-back samples examined, `refused` the pixels whose history the prep refused, by cause. The stale pixels are
# two numbers: `stale-refused`, refused (with the steady-detail key off every stale pixel, with it on the ones last frame's depth did
# not confirm), and `stale-kept`, not refused (the camera term, confirmed by last frame's depth). `depth-check` is the resolves with
# the key on whose prep ran the depth check and those that could not (a reset frame is neither). The flight-3 build's `stale=` and
# `forgiven=` (its blanket form of the rule) still parse, as stale-refused and stale-kept, with no depth-check. "Ran, 0 refused" is
# pixels > 0 and refused=0, "never ran" is treated=0, asked=0 or read=0.) The route also logs a line when the steady-detail key or
# the refusal view changes (`steady-detail is ON|OFF from frame=`, `the refusal view is ON|OFF from frame=`), see ROUTE_EVENT_MARKERS.
#
# WHAT A CAMERA IS. The camera OBJECT is no identity: one object was the left eye camera in
# the cockpit (kind 5) and the world's kind-3 camera on foot (flight 1, 4.63 million calls),
# and the camera line names only the kind of its FIRST call. So the report works from the KIND
# OF EACH CALL in the logged call sequences: a camera is labelled by the kinds of its calls (it
# may have several), the world camera is the one with the most kind-3 calls before the tone, the
# eye cameras are the cameras whose calls joined an eye draw's rows, and a camera's kind-3 calls
# are grouped by the projection their composed rows carry, so a second projection on the same
# object (the first-person weapon camera: a tighter field of view and a larger near plane)
# shows as its own role.
#
# THE JOIN (B) is offline: each eye draw's b1 rows are matched to the rows of every
# logged call within CENSUS_JOIN_TOL; the cameras whose calls match ARE the eye
# cameras, and their field signature, caller, view and place against the tone draw
# are what a rule to exclude them can be built from. `foot=` is what Elite's journal
# said: the census samples a frame (a call sequence, an eye readback) only when the
# tone was drawn AND the journal, if it is read, says on foot (`off`: no journal,
# the tone alone decided); and, while the route is jittering, only when the frame's
# phase is non-zero (`phase=` is what vrWorldRouteWorldPhase said: `-` the route was
# not jittering, a pair the phase in render pixels, positive right/down).
#
# THE STAGE 2 VERDICT reads the injected flight: the eye rows must not move (|leak|
# below CENSUS_LEAK_PASS with a world phase of 1e-4 to 2e-4 NDC) while the kind-3 calls'
# rows carry the phase (their measured shift is flatProjectionJitter's: x = 2 px / W,
# y = -2 py / H, within CENSUS_PHASE_TOL), nothing but kind 3 is injected, and nothing ran
# off the render thread or unreadable. PASS, WARN or STOP a line; `n/a` for what a log
# cannot say (an older census, a route that never jittered).

CENSUS_LINE_RE = re.compile(
    r"^(?P<ts>\[[\d:.]+\])?\s*vr camera census(?P<five> 5s)?: (?P<rest>.*?)\s*$")
ROUTE_LINE_RE = re.compile(
    r"^(?P<ts>\[[\d:.]+\])?\s*vr world route (?P<inject>inject )?5s: (?P<rest>.*?)\s*$")
REFUSAL_LINE_RE = re.compile(
    r"^(?P<ts>\[[\d:.]+\])?\s*vr world route refusal 5s: (?P<rest>.*?)\s*$")
# The causes the refusal line counts, in the line's own order (src/d3d11/flat_mono_refusal.h), and what each one is. `other` is
# what the census cannot name: a refused pixel of a class the line has no token for.
REFUSAL_CAUSES = (
    ("stale-refused", "the engine slot's depth was not the pixel's (a later draw overdrew it) and the steady-detail depth check, if it is on, did not confirm the camera term"),
    ("masked", "a rig record with no usable history this frame (first seen, or after a gap)"),
    ("corrupt", "a slot code or a record number that did not survive intact"),
    ("sentinel", "no depth under the slot (the sky) or the out-of-range marker"),
    ("unreprojectable", "a moved record whose reprojection failed"),
    ("camera", "the camera term could not be formed"),
    ("range", "the reprojection left the screen or was not finite"),
    ("depth", "the pixel's own depth was not usable"),
    ("weapon", "an attached first-person pixel the weapon's map could not place"),
    ("other", "a refusal the census cannot name"),
)
CENSUS_JOIN_TOL = 1e-5
# Two shift-sign candidates whose residuals differ by less than this are a tie: rows are floats, so rounding alone moves a
# measure by about 1e-7, and with a shift of nothing (the jitter off) every candidate is the same number.
CENSUS_FIT_TIE = 2e-7
CENSUS_RUNS_SHOWN = 24    # a frame with more runs shows its first and last half of this, and says how many it left out
CENSUS_FIXTURE = "camera_census_fixture.log"
# What separates an eye call from a world-side call, read off each call's own rows (an object's first-sight line is
# the wrong place to read it: the world camera's was an eye's).
CENSUS_CALL_FIELDS = ("kind", "aspect", "fov", "near", "shift")
# The stage 2 verdict's thresholds (NDC): an eye row's leak below PASS is no leak, above STOP the world phase reached the
# eye camera (a phase of half a pixel at 5040 wide is 2e-4); a kind-3 call's measured shift must match the phase it was
# given to PHASE_TOL, and is wrong beyond PHASE_STOP.
CENSUS_LEAK_PASS = 1.0e-6
CENSUS_LEAK_STOP = 1.0e-5
CENSUS_PHASE_TOL = 1.0e-6
CENSUS_PHASE_STOP = 1.0e-5
CENSUS_PROJ_TOL = 1.0e-4   # relative: two calls whose scale and near agree to this carry one projection
CENSUS_PROJ_SHOWN = 6
# The kinds the design puts on the screen: the world is 3, the eyes 5 (design doc section 82, flight 1).
CENSUS_KIND5_PER_FRAME = 6.0       # two eyes x the three call sites +0x594E13, +0x594EAB, +0x594FE1
CENSUS_INJECTED_PER_FRAME = (54, 68)
# The role test's field-of-view ratio (kFlatCameraVrFirstPersonFovRatio in src/d3d11/flat_camera_vr.h): a screen view whose fov is at
# most this fraction of the frame's widest is the first-person camera. Flight 2's struct: 0.8203 against 0.9831 rad (ratio 0.834).
CENSUS_FP_FOV_RATIO = 0.92


def _cf(text):
    """A number as the DLL prints it. `nan` (the DLL's spelling of any
    non-finite value) and anything unreadable are NaN, so a comparison with one
    is false and it never joins."""
    try:
        return float(text)
    except (TypeError, ValueError):
        return float("nan")


def _chex(text):
    try:
        return int(text.lstrip("+"), 16)
    except (AttributeError, ValueError):
        return None


def _cint(text):
    """A non-negative integer token, else None."""
    return int(text) if text is not None and text.isdigit() else None


def _ctuple(text):
    """(a,b) or (a,b,c,d) -> floats; `-` or anything else -> None."""
    if not text or not text.startswith("(") or not text.endswith(")"):
        return None
    return tuple(_cf(p) for p in text[1:-1].split(","))


def _clist(text):
    if not text or not text.startswith("[") or not text.endswith("]"):
        return None
    return [_cf(p) for p in text[1:-1].split(",")]


def _cpair(text):
    """`x,y` (the route's and the census's phase) -> (x, y) floats, else None."""
    if not text or text.count(",") != 1:
        return None
    a, b = (_cf(p) for p in text.split(","))
    if a != a or b != b:
        return None
    return (a, b)


def _cphase(text):
    """A `phase=` token as (state, (x, y) or None): `absent` when the line has
    none (a census that predates stage 2), `off` for `-` (the route was not
    jittering that frame), `on` for a pair in render pixels (a pair of zeros is a
    jittering frame whose phase was zero: a warm-up frame)."""
    if text is None:
        return "absent", None
    if text == "-":
        return "off", None
    pair = _cpair(text)
    return ("on", pair) if pair else ("absent", None)


def _ckinds(text):
    """The route's `inj-kinds=` token: `none` -> {}, `3:54,other:2` -> {"3": 54, "other": 2}; anything else None."""
    if text is None:
        return None
    if text == "none":
        return {}
    out = {}
    for part in text.split(","):
        key, sep, value = part.partition(":")
        if not sep or not key or not value.isdigit():
            return None
        out[key] = out.get(key, 0) + int(value)
    return out


def _ckv(rest):
    kv = {}
    for token in rest.split():
        key, sep, value = token.partition("=")
        if sep and re.match(r"^[a-z][a-z0-9-]*$", key):
            kv[key] = value
    return kv


def _ckindmap(text):
    """An episode header's `kinds=` (`0:12,3:290,5:10,other:2,unreadable:1`, `-` for none) -> {0: 12, 3: 290, 5: 10, "other": 2, "unreadable": 1}; None when it is not one."""
    if text is None:
        return None
    if text == "-":
        return {}
    out = {}
    for part in text.split(","):
        key, sep, value = part.partition(":")
        if not sep or not value.isdigit():
            return None
        out[int(key) if key.isdigit() else key] = out.get(int(key) if key.isdigit() else key, 0) + int(value)
    return out


def _ccallers(text):
    """An episode header's `callers=` (`+0x58DE73:300,+0x594E13:130,+more:1`, `-` for none) -> ([(rva, n), ...], more); None when it is not one."""
    if text is None:
        return None
    if text == "-":
        return [], 0
    callers, more = [], 0
    for part in text.split(","):
        key, sep, value = part.rpartition(":")
        if not sep or not value.isdigit():
            return None
        if key == "+more":
            more += int(value)
            continue
        rva = _chex(key)
        if rva is None:
            return None
        callers.append((rva, int(value)))
    return callers, more


def _ctrigger(text):
    """A `trigger=` token: `key-on`, `foot:no>yes`, `naming:unnamed>named`, `gui:0>6` -> (kind, from, to) with from and to None for key-on."""
    if text == "key-on":
        return ("key-on", None, None)
    kind, sep, rest = (text or "").partition(":")
    if not sep or kind not in ("foot", "naming", "gui"):
        return None
    old, arrow, new = rest.partition(">")
    return (kind, old, new) if arrow and old and new else None


def _cmatch(text):
    """A `match=` / `axes-match=` token: `98,101`, `1,2,3,4,5,6+48`, `-` -> ([ordinals], how many more matched than are listed); None when it is not one."""
    if text is None:
        return None
    if text == "-":
        return [], 0
    listed, plus, extra = text.partition("+")
    if plus and not extra.isdigit():
        return None
    ordinals = []
    for part in listed.split(","):
        if not part.isdigit():
            return None
        ordinals.append(int(part))
    return ordinals, int(extra) if plus else 0


CENSUS_RUN_BINS = ("1", "2", "3", "4-8", "9-30", "31-89", "90+")


def _cbins(text):
    """A `named=` / `unnamed=` token of a runs line (`1:1,2:0,3:0,4-8:0,9-30:0,31-89:1,90+:1`) -> {bin name: runs}; None unless it has exactly the seven bins in order."""
    if text is None:
        return None
    out = {}
    for part in text.split(","):
        key, sep, value = part.rpartition(":")
        if not sep or not value.isdigit():
            return None
        out[key] = int(value)
    return out if tuple(out) == CENSUS_RUN_BINS else None


def _cpair_us(text):
    """A detour line's `mean/max` microseconds (`25/30`, `0.31/4.2`, `-`) -> (mean, max) floats, or None."""
    if not text or "/" not in text:
        return None
    a, _, b = text.partition("/")
    mean, peak = _cf(a), _cf(b)
    return (mean, peak) if mean == mean and peak == peak else None


def census_geometry(rows):
    """What a call's composed rows (rows 270..273, sixteen floats) say about the camera that made them, read back
    out of them and free of how the head is turned: the projection's x and y scale, the aspect and field of view
    they give, the near plane (rows[14]) and the off-centre shift flatCameraMeasureRowShift measures (NDC). The x
    column of the rows is p0 times one axis plus p8 times the view direction, so its length squared is p0^2 + p8^2
    and the scale is what is left after the shift is taken out. None when the rows are absent or not finite."""
    if not rows or len(rows) != 16 or any(not math.isfinite(v) for v in rows):
        return None
    f = (rows[3], rows[7], rows[11])
    ff = f[0] * f[0] + f[1] * f[1] + f[2] * f[2]
    if not ff > 1e-6:
        return None
    sx = (rows[0] * f[0] + rows[4] * f[1] + rows[8] * f[2]) / ff
    sy = (rows[1] * f[0] + rows[5] * f[1] + rows[9] * f[2]) / ff
    xs = math.sqrt(max(0.0, rows[0] ** 2 + rows[4] ** 2 + rows[8] ** 2 - sx * sx * ff))
    ys = math.sqrt(max(0.0, rows[1] ** 2 + rows[5] ** 2 + rows[9] ** 2 - sy * sy * ff))
    if not (xs > 0.0 and ys > 0.0):
        return None
    return {"shift": (sx, sy), "xs": xs, "ys": ys, "aspect": ys / xs, "fov": 2.0 * math.atan(1.0 / ys),
            "near": rows[14]}


def parse_camera_census(text):
    """A flight log's census lines, sorted into what each one is.

    Returns a dict: lines (how many census lines), windows [(stamp, text)],
    cameras {ptr: row} with order [ptr] in first-seen order, changes {ptr: [line]},
    sequences [{frame, index, calls, recorded, truncated, phase_state, phase, rows}],
    eyes [dict], geometry {(eye, frame): dict}, threads [dict], info [text]. A call
    row is {n, camera, kind, caller, draw, tone, inj, role, pre, post, view, rows,
    geo}; a value the DLL printed as `-` is None, and a token an older census never
    printed (inj, role, phase) is None or `absent`.

    The EPISODES (Phase 0): episodes [dict] (a header's fields, `rows` the call lines that followed it, `joins` the join
    signatures and their rows, `pass` the pass's rows line, `join_more`), episode_counters, runs and detour (the 5 s
    window's three companion lines; they are not windows: `windows` holds the 5 s lines only). An episode's call lines
    are NOT in `sequences`: the legacy analyses read the first three on-foot frames alone."""
    c = {"lines": 0, "unparsed": 0, "windows": [], "cameras": {}, "order": [], "changes": {},
         "sequences": [], "eyes": [], "geometry": {}, "threads": [], "info": [],
         "episodes": [], "episode_counters": [], "runs": [], "detour": []}
    current = None
    episode_by_n = {}
    for raw in text.splitlines():
        m = CENSUS_LINE_RE.match(raw)
        if not m:
            continue
        c["lines"] += 1
        rest = m.group("rest")
        if m.group("five"):
            c["windows"].append((m.group("ts") or "", rest))
            continue
        try:
            kv = _ckv(rest)
            if rest.startswith("camera="):
                ptr = _chex(kv.get("camera"))
                if ptr is None or ptr in c["cameras"]:
                    continue
                c["order"].append(ptr)
                c["cameras"][ptr] = {
                    "ptr": ptr, "kind": int(kv.get("kind", "-1")) if kv.get("kind", "-").isdigit() else None,
                    "caller": _chex(kv.get("caller")), "thread": kv.get("thread"),
                    "aspect": _cf(kv.get("aspect")), "near": _cf(kv.get("near")),
                    "far": _cf(kv.get("far")), "fov": _cf(kv.get("fov")),
                    "bound": _ctuple(kv.get("bound")), "offcentre": _ctuple(kv.get("offcentre")),
                    "viewport": _ctuple(kv.get("viewport")), "tan": _ctuple(kv.get("tan")),
                    "view": _chex(kv.get("view")), "vctx": _chex(kv.get("vctx")),
                    "first_call": kv.get("first-call"), "draw": kv.get("draw"),
                    "tone": kv.get("tone"), "frame": int(kv["frame"]) if kv.get("frame", "").isdigit() else None}
            elif rest.startswith("changed:"):
                ptr = _chex(kv.get("camera"))
                fields = {k: v for k, v in kv.items()
                          if k not in ("camera", "frame", "n") and "->" in v}
                c["changes"].setdefault(ptr, []).append(
                    {"frame": int(kv["frame"]) if kv.get("frame", "").isdigit() else None,
                     "n": kv.get("n"), "fields": fields})
            elif rest.startswith("sequence "):
                state, phase = _cphase(kv.get("phase"))
                current = {"frame": int(kv.get("frame", "-1")), "index": kv.get("index"), "foot": kv.get("foot"),
                           "phase_state": state, "phase": phase,
                           "calls": int(kv.get("calls", "0")), "recorded": int(kv.get("recorded", "0")),
                           "truncated": int(kv.get("truncated", "0")), "rows": []}
                c["sequences"].append(current)
            elif rest.startswith("call "):
                frame = int(kv.get("frame", "-1"))
                if _chex(kv.get("camera")) is None:   # nothing to join or digest, and no sequence to start for it
                    c["unparsed"] += 1
                    continue
                if current is None or current["frame"] != frame:
                    current = {"frame": frame, "index": "?", "foot": None, "phase_state": "absent", "phase": None,
                               "calls": 0, "recorded": 0, "truncated": 0, "rows": []}
                    c["sequences"].append(current)
                pre, _, post = kv.get("fl", "").partition(">")
                rows = _clist(kv.get("rows"))
                current["rows"].append({
                    "n": int(kv.get("n", "0")), "camera": _chex(kv.get("camera")),
                    "kind": int(kv["kind"]) if kv.get("kind", "-").isdigit() else None,
                    "caller": _chex(kv.get("caller")),
                    "draw": int(kv["draw"]) if kv.get("draw", "-").isdigit() else None,
                    "tone": kv.get("tone"), "pre": _chex(pre) if pre else None,
                    "post": _chex(post) if post and post != "-" else None,
                    "inj": int(kv["inj"]) if kv.get("inj") in ("0", "1") else None,
                    "role": kv.get("role"),
                    "view": _chex(kv.get("view")),
                    "rows": rows, "geo": census_geometry(rows), "frame": frame})
            elif rest.startswith("eye-geometry"):
                eye = int(kv.get("eye", "-1"))
                frame = int(kv.get("frame", "-1"))
                c["geometry"][(eye, frame)] = {
                    "known": kv.get("geometry") != "unavailable",
                    "seq": int(kv["seq"]) if kv.get("seq", "").isdigit() else None,
                    "frustum": _clist(kv.get("frustum")), "shift": _ctuple(kv.get("shift")),
                    "expect": _ctuple(kv.get("expect")),
                    "expect_shifted": _ctuple(kv.get("expect-shifted")),
                    "leak": _ctuple(kv.get("leak"))}
            elif rest.startswith("eye="):
                state, phase = _cphase(kv.get("phase"))
                c["eyes"].append({
                    "eye": int(kv.get("eye", "-1")), "frame": int(kv.get("frame", "-1")), "foot": kv.get("foot"),
                    "phase_state": state, "phase": phase,
                    "draw": int(kv["draw"]) if kv.get("draw", "-").isdigit() else None,
                    "b1": _chex(kv.get("b1")), "first": kv.get("first"), "bytes": kv.get("bytes"),
                    "rows": _clist(kv.get("rows")), "meas": _ctuple(kv.get("meas")),
                    "why": kv.get("why")})
            elif rest.startswith("other-thread"):
                c["threads"].append({"tid": kv.get("tid"), "camera": _chex(kv.get("camera")),
                                     "kind": kv.get("kind"), "caller": _chex(kv.get("caller")),
                                     "calls": kv.get("calls")})
            elif rest.startswith("episode "):
                # An episode's header; the call lines that follow it (the same frame) are its calls.
                n, _, of = kv.get("n", "0/0").partition("/")
                state, phase = _cphase(kv.get("phase"))
                trigger = _ctrigger(kv.get("trigger"))
                callers = _ccallers(kv.get("callers"))
                kinds = _ckindmap(kv.get("kinds"))
                if not n.isdigit() or trigger is None or kinds is None or callers is None:
                    c["unparsed"] += 1
                    continue
                ep = {"n": int(n), "of": int(of) if of.isdigit() else None, "frame": int(kv.get("frame", "-1")),
                      "armed": _cint(kv.get("armed")), "trigger": trigger, "foot": kv.get("foot"),
                      "gui": _cint(kv.get("gui")), "named": kv.get("named") == "1",
                      "phase_state": state, "phase": phase,
                      "calls": int(kv.get("calls", "0")), "recorded": int(kv.get("recorded", "0")),
                      "printed": int(kv.get("printed", "0")), "kinds": kinds, "callers": callers[0],
                      "callers_more": callers[1], "rows": [], "joins": [], "pass": None, "join_more": None, "join_draws": None}
                c["episodes"].append(ep)
                episode_by_n[ep["n"]] = ep
                current = ep
            elif rest.startswith("join-rows "):
                ep = episode_by_n.get(_cint(kv.get("ep")))
                sig = _cint(kv.get("sig"))
                rows = _clist(kv.get("rows"))
                row = next((j for j in ep["joins"] if j["sig"] == sig), None) if ep else None
                if row is None or rows is None or len(rows) != 16:
                    c["unparsed"] += 1
                    continue
                row["rows"] = rows
            elif rest.startswith("join-draws "):
                ep = episode_by_n.get(_cint(kv.get("ep")))
                counts = [_cint(kv.get(k)) for k in ("seen", "relevant", "views", "signatures")]
                if not ep or any(v is None for v in counts):
                    c["unparsed"] += 1
                    continue
                ep["join_draws"] = dict(zip(("seen", "relevant", "views", "signatures"), counts))
            elif rest.startswith("join-more "):
                ep = episode_by_n.get(_cint(kv.get("ep")))
                if not ep:
                    c["unparsed"] += 1
                    continue
                ep["join_more"] = {"signatures": _cint(kv.get("signatures")), "draws": _cint(kv.get("draws"))}
            elif rest.startswith("join "):
                ep = episode_by_n.get(_cint(kv.get("ep")))
                size = re.match(r"^(\d+)x(\d+)$", kv.get("size", ""))
                match = _cmatch(kv.get("match")) if "match" in kv else None
                if not ep or _cint(kv.get("sig")) is None or not size or kv.get("depth") not in ("screen", "eye") or \
                        ("match" in kv and match is None):
                    c["unparsed"] += 1
                    continue
                ep["joins"].append({
                    "sig": int(kv["sig"]), "depth": kv["depth"], "eye": _cint(kv.get("eye")),
                    "w": int(size.group(1)), "h": int(size.group(2)),
                    "draw": _cint(kv.get("draw")), "draws": _cint(kv.get("draws")),
                    "vs": _chex(kv.get("vs")), "ps": _chex(kv.get("ps")),
                    "dw": {"yes": True, "no": False}.get(kv.get("dw")),
                    "b1": _chex(kv.get("b1")), "first": _cint(kv.get("first")), "bytes": _cint(kv.get("bytes")),
                    "read": kv.get("rows") == "read", "why": kv.get("why"),
                    "match": match[0] if match else None, "match_more": match[1] if match else 0,
                    "rows": None})
            elif rest.startswith("pass-rows "):
                ep = episode_by_n.get(_cint(kv.get("ep")))
                valid = kv.get("valid") == "1"
                rows = _clist(kv.get("rows")) if valid else None
                match = _cmatch(kv.get("axes-match")) if valid else ([], 0)
                if not ep or kv.get("valid") not in ("0", "1") or (valid and (rows is None or len(rows) != 12 or match is None)):
                    c["unparsed"] += 1
                    continue
                ep["pass"] = {"valid": valid, "bound": {"1": True, "0": False}.get(kv.get("bound")), "rows": rows,
                              "match": match[0], "match_more": match[1],
                              "how": kv.get("how") if kv.get("how") in ("identity", "transpose") else None,
                              "nearest": _cint(kv.get("nearest")), "diff": _cf(kv.get("diff"))}
            elif rest.startswith("episodes "):
                taken, _, of = kv.get("taken", "").partition("/")
                if not taken.isdigit() or _cint(kv.get("triggers")) is None or _cint(kv.get("skipped")) is None:
                    c["unparsed"] += 1
                    continue
                c["episode_counters"].append({
                    "ts": m.group("ts") or "", "windows": _cint(kv.get("windows")), "taken": int(taken),
                    "of": int(of) if of.isdigit() else None, "triggers": int(kv["triggers"]), "skipped": int(kv["skipped"]),
                    "state": kv.get("state"), "trigger": _ctrigger(kv.get("trigger")),
                    "armed": _cint(kv.get("armed")), "sample": _cint(kv.get("sample"))})
            elif rest.startswith("runs "):
                named, unnamed = _cbins(kv.get("named")), _cbins(kv.get("unnamed"))
                longest = re.match(r"^named:(\d+),unnamed:(\d+)$", kv.get("longest", ""))
                opened = re.match(r"^(named|unnamed):(\d+)$", kv.get("open", ""))
                if named is None or unnamed is None or not longest or (kv.get("open") != "-" and not opened) or \
                        _cint(kv.get("frames")) is None:
                    c["unparsed"] += 1
                    continue
                c["runs"].append({
                    "ts": m.group("ts") or "", "windows": _cint(kv.get("windows")), "frames": int(kv["frames"]),
                    "named": named, "unnamed": unnamed,
                    "longest_named": int(longest.group(1)), "longest_unnamed": int(longest.group(2)),
                    "open": (opened.group(1), int(opened.group(2))) if opened else None})
            elif rest.startswith("detour "):
                c["detour"].append({
                    "ts": m.group("ts") or "", "windows": _cint(kv.get("windows")), "every": _cint(kv.get("every")),
                    "timed": kv.get("timed"), "frames": _cint(kv.get("frames")), "calls": _cint(kv.get("calls")),
                    "sampled": _cint(kv.get("sampled")),
                    "est_ms": _cf(kv.get("est-ms-frame")) if kv.get("est-ms-frame", "-") != "-" else None,
                    "modes": {name: {"calls": _cint(kv.get(name + "-calls")), "sampled": _cint(kv.get(name + "-sampled")),
                                     "pre": _cpair_us(kv.get(name + "-pre-us")), "post": _cpair_us(kv.get(name + "-post-us"))}
                              for name in ("obs", "inj")}})
            else:
                c["info"].append(rest)
        except (ValueError, TypeError, KeyError, IndexError):
            c["unparsed"] += 1   # a line cut short or garbled: counted, never fatal to the report
    return c


def parse_world_route(text):
    """The world route's 5 s lines. The route prints two a window, back to back: `vr world route 5s:` and
    `vr world route inject 5s:`; the inject line joins the route line before it BY ORDER (their timestamps are the
    boundary's and need not be read), and a window that has only one of the two is kept with the other None. Returns a
    list of {ts, route, inject} with each side a key=value dict; `tok(w, key)` reads a token from either."""
    windows = []
    for raw in text.splitlines():
        m = ROUTE_LINE_RE.match(raw)
        if not m:
            continue
        kv = _ckv(m.group("rest"))
        if m.group("inject"):
            if windows and windows[-1]["inject"] is None and windows[-1]["route"] is not None:
                windows[-1]["inject"] = kv
            else:
                windows.append({"ts": m.group("ts") or "", "route": None, "inject": kv})
        else:
            windows.append({"ts": m.group("ts") or "", "route": kv, "inject": None})
    return windows


# Lines of the route's own log the verdict quotes (their wording is the route's: src/d3d11/vr_world_route*.cpp). Each key is what the
# line means to the verdict; the needle is a phrase of it.
ROUTE_EVENT_MARKERS = (
    ("stop", "STOP at frame="),
    ("shut", "on a frame whose window the route had shut"),
    ("kind-other", "camera calls of a kind other than 3 were INJECTED"),
    ("rows-disagree", "camera rows disagree with the phase"),
    ("no-scene-call", "jitter is wanted but no scene camera call was injected"),
    ("jittered", "the world is JITTERED from frame="),
    ("window-open", "camera window was open"),
    ("released", "vr world route: RELEASED the world at frame="),
    ("excluded", "camera call EXCLUDED, not a screen view"),
    ("steady-detail", "vr world route: steady-detail is "),
    ("refusal-view", "vr world route: the refusal view is "),
)


def route_events(text):
    """{key: [line, ...]} for every line of the log that carries one of ROUTE_EVENT_MARKERS' phrases, and under "vscreen-applied" the
    panel patch's `vScreen resolution: WxH -> WxH at N site(s)` line (the on-foot screen's size, the size the route resolves at)."""
    events = {}
    for raw in text.splitlines():
        for key, needle in ROUTE_EVENT_MARKERS:
            if needle in raw:
                events.setdefault(key, []).append(raw.strip())
        if "vScreen resolution: " in raw and " -> " in raw and re.search(r"vScreen resolution: \d+x\d+ -> \d+x\d+ at \d+ site", raw):
            events.setdefault("vscreen-applied", []).append(raw.strip())
    return events


def route_tok(window, key):
    """A token of one route window, from the route line or the inject line (None when neither has it)."""
    for side in ("route", "inject"):
        kv = window[side]
        if kv is not None and key in kv:
            return kv[key]
    return None


def route_hdr(windows, events=None):
    """The size the world phase is in (render pixels): the route line's `hdr=WxH`, the commonest that is not 0x0
    (it reads 0x0 while the route has no frame); failing that the size the route's own `the world is JITTERED from
    frame=` line names (`phase (x,y) px in WxH`). (W, H, where from), or None when the log has neither."""
    sizes = {}
    for w in windows:
        m = re.match(r"^(\d+)x(\d+)$", route_tok(w, "hdr") or "")
        if m and int(m.group(1)) and int(m.group(2)):
            key = (int(m.group(1)), int(m.group(2)))
            sizes[key] = sizes.get(key, 0) + 1
    if sizes:
        w, h = sorted(sizes.items(), key=lambda kv: (-kv[1], kv[0]))[0][0]
        return w, h, "a `vr world route 5s:` line's hdr="
    for line in (events or {}).get("jittered", []):
        m = re.search(r" px in (\d+)x(\d+),", line)
        if m and int(m.group(1)) and int(m.group(2)):
            return int(m.group(1)), int(m.group(2)), "the route's `the world is JITTERED` line"
    # The route resolves the world at the on-foot screen's own size (R = D), which the panel patch's line names: the logged size, for a
    # log whose route lines are missing or read 0x0.
    for line in (events or {}).get("vscreen-applied", []):
        m = re.search(r" -> (\d+)x(\d+) at \d+ site", line)
        if m and int(m.group(1)) and int(m.group(2)):
            return int(m.group(1)), int(m.group(2)), "the panel patch's `vScreen resolution:` line"
    return None


def _cmodes(text):
    """The route line's `fp-mode=a/b/c` (frames whose weapon fold-in ran in mode 0, 1, 2) as three ints, else None."""
    m = re.match(r"^(\d+)/(\d+)/(\d+)$", text or "")
    return (int(m.group(1)), int(m.group(2)), int(m.group(3))) if m else None


def _cfov(text):
    """The inject line's `fov=` token: `lo..hi` -> (lo, hi) floats; `-` (no frustum was read) or anything else -> None."""
    m = re.match(r"^(\d+(?:\.\d+)?)\.\.(\d+(?:\.\d+)?)$", text or "")
    return (float(m.group(1)), float(m.group(2))) if m else None


def route_fov(routes):
    """(narrowest, widest, windows): the struct's field of view (radians) over the inject lines' `fov=` tokens, and how many windows
    had a range; (None, None, 0) when none did (an older build, or no frustum was read)."""
    lo = hi = None
    n = 0
    for w in routes:
        r = _cfov(route_tok(w, "fov"))
        if r:
            lo = r[0] if lo is None or r[0] < lo else lo
            hi = r[1] if hi is None or r[1] > hi else hi
            n += 1
    return lo, hi, n


def parse_refusal_windows(text):
    """The refusal census's 5 s lines (`vr world route refusal 5s:`: one a window while advanced.vr_camera_census is on and the route is
    engaged, while the steady-detail key is on and the route is engaged (its default; census=off then), and while samples of a window
    that has just ended are still draining), each joined to the route's own line and inject line
    of the same window (the route prints route, inject, refusal, in that order; a missing one is None). A list of dicts: ts, kv (every
    token), census (census=on), steady and view (the tokens' text), w and h (size=), pct (refused-pct), route and inject (the other
    two lines' tokens), the counters as ints (None when a token is absent or not a number: treated, asked, sampled, read, dropped,
    every, pixels, refused), kept (`stale-kept=`, or the flight-3 build's `forgiven=`), check_ran and check_skipped (`depth-check=RAN/SKIPPED`:
    None for a build without the depth check) and causes {name: int or None} for every name of REFUSAL_CAUSES (`stale-refused`
    reads the flight-3 build's `stale=` when the new token is absent)."""
    out = []
    route = inject = None
    for raw in text.splitlines():
        m = ROUTE_LINE_RE.match(raw)
        if m:
            if m.group("inject"):
                inject = _ckv(m.group("rest"))
            else:
                route, inject = _ckv(m.group("rest")), None
            continue
        m = REFUSAL_LINE_RE.match(raw)
        if not m:
            continue
        kv = _ckv(m.group("rest"))
        size = re.match(r"^(\d+)x(\d+)$", kv.get("size", ""))
        w = {"ts": m.group("ts") or "", "kv": kv, "census": kv.get("census") == "on", "steady": kv.get("steady-detail"),
             "view": kv.get("view"), "w": int(size.group(1)) if size else None, "h": int(size.group(2)) if size else None,
             "pct": _cf(kv.get("refused-pct")), "route": route, "inject": inject}
        for key in ("every", "treated", "asked", "sampled", "read", "dropped", "pixels", "refused"):
            w[key] = _cint(kv.get(key))
        w["kept"] = _cint(kv.get("stale-kept", kv.get("forgiven")))
        w["skinned"] = _cint(kv.get("skinned-joined"))   # None in a build before F2 on foot
        check = re.match(r"^(\d+)/(\d+)$", kv.get("depth-check", ""))
        w["check_ran"], w["check_skipped"] = (int(check.group(1)), int(check.group(2))) if check else (None, None)
        w["causes"] = {name: _cint(kv.get(name)) for name, _ in REFUSAL_CAUSES}
        if w["causes"]["stale-refused"] is None:
            w["causes"]["stale-refused"] = _cint(kv.get("stale"))
        out.append(w)
        route = inject = None
    return out


def refusal_state(w):
    """What one refusal line says happened in its window. `measured`: samples were read back and the shares are real (including a window
    that measured and found nothing refused: pixels > 0, refused=0). `census-off`: the census key was off and nothing asked for it; the
    line is there for the steady-detail key (on by default) and carries its depth-check frame counts only, so there is nothing to
    measure and nothing wrong. The rest are windows that measured nothing, each for its own reason: `idle` (the route treated no frame),
    `no-ask` (the census was on, the route treated frames and none asked for it), `no-sample` (fewer asks than one sample takes),
    `no-read` (samples were dispatched and none came back), `unreadable` (a counter did not parse)."""
    if any(w[k] is None for k in ("treated", "asked", "sampled", "read", "pixels", "refused")):
        return "unreadable"
    if w["read"] and w["pixels"]:
        return "measured"
    if not w["census"] and not (w["asked"] or w["sampled"]):
        return "census-off"
    if not w["treated"]:
        return "idle"
    if not w["asked"]:
        return "no-ask"
    if not w["sampled"]:
        return "no-sample"
    return "no-read"


def _pct(n, pixels):
    return "%.3f%%" % (100.0 * n / pixels) if pixels else "-"


def _stale_tail(steady, kept, stale_refused, ran, skipped):
    """The tail of a measured line's stale part: with the key on, how much of the stale pixels the depth check kept, and, when the line
    carries it (and the key is on, or the check counted frames), the depth check's own frames."""
    out = ""
    stale = (kept or 0) + (stale_refused or 0)
    if steady == "on" and stale:
        out += " (%.1f%% of the stale pixels)" % (100.0 * (kept or 0) / stale)
    if ran is not None and (steady == "on" or ran or skipped):
        out += "; depth-check %d ran, %d skipped" % (ran, skipped or 0)
    return out


def _key_findings(w, stamp, measured):
    """What the steady-detail key's state and the resolver's depth-check frames say to each other in one window. The frame counts are
    the resolver's own host counters, so a window needs no samples for them (a `census-off` window has none); the stale pixels are
    known only to a window that measured."""
    out = []
    causes = w["causes"]
    ran, skipped = w["check_ran"], w["check_skipped"]
    if measured and w["steady"] == "on" and ran is None and (causes["stale-refused"] or 0) > 0:
        # A build without the depth check (the flight-3 build): its key was the blanket form, which refused no stale pixel.
        out.append(("WARN", "%s: steady-detail=on and %d stale pixel(s) were still refused: the key's rule did not reach the resolver "
                    "(a build without the depth check: with it on, a stale slot takes the camera term and is counted as kept)"
                    % (stamp, causes["stale-refused"])))
    if w["steady"] == "on" and ran is not None:
        if not ran and not skipped:
            if w["treated"]:   # a window that treated nothing had nothing to check
                out.append(("WARN", "%s: steady-detail=on and the resolver counted no depth-check frame in a window that treated %d frame(s): "
                            "the key reached the route and not the resolver" % (stamp, w["treated"])))
        elif not ran:
            out.append(("WARN", "%s: steady-detail=on and the depth check never ran (0 ran, %d skipped): the resolver could not make "
                        "last frame's depth, so every stale pixel was refused as with the key off" % (stamp, skipped)))
        elif measured and not (w["kept"] or 0) and (causes["stale-refused"] or 0) > 0:
            out.append(("note", "%s: steady-detail=on, the depth check ran in %d frame(s) and kept no stale pixel (%d refused): "
                        "everything stale moved, or last frame's depth never matched" % (stamp, ran, causes["stale-refused"])))
    if w["steady"] == "off":
        if (w["kept"] or 0) > 0:
            out.append(("WARN", "%s: %d stale pixel(s) were kept while steady-detail=off: the line's key state and the resolver's disagree"
                        % (stamp, w["kept"])))
        if (ran or 0) or (skipped or 0):
            out.append(("WARN", "%s: the depth check counted frames (%d ran, %d skipped) while steady-detail=off: the line's key state and "
                        "the resolver's disagree" % (stamp, ran or 0, skipped or 0)))
    return out


def refusal_findings(windows):
    """[(level, text)] about what the refusal lines do not support believing (WARN: a number that contradicts another, a key that did not
    take effect, a census that never measured) and what is worth knowing (note). A `census-off` window is not a census that failed: only
    the steady-detail key's own checks apply to it."""
    out = []
    states = [refusal_state(w) for w in windows]
    for w, s in zip(windows, states):
        stamp = w["ts"] or "(no stamp)"
        if s == "unreadable":
            out.append(("WARN", "%s: a refusal line has a counter that is missing or not a number (an older or garbled line): its window is not counted"
                        % stamp))
        elif s == "no-ask":
            out.append(("WARN", "%s: the route treated %d frame(s) and none asked the resolver for the census: the key reached the route "
                        "(census=on) and not the resolver" % (stamp, w["treated"])))
        elif s == "no-read":
            out.append(("WARN", "%s: %d sample(s) were dispatched and none was read back: the read-back is stalled (or this is the window "
                        "the key went on in)" % (stamp, w["sampled"])))
        elif s == "census-off":
            out.extend(_key_findings(w, stamp, False))
        elif s == "measured":
            causes = w["causes"]
            if w["refused"] > w["pixels"]:
                out.append(("WARN", "%s: refused %d exceeds the pixels examined, %d: the census counts a pixel once, so one of the two is wrong"
                            % (stamp, w["refused"], w["pixels"])))
            out.extend(_key_findings(w, stamp, True))
            if (w["dropped"] or 0) > 0:
                out.append(("note", "%s: %d sample(s) were skipped because the read-back ring was full (the shares are unaffected, the sample "
                            "count is lower)" % (stamp, w["dropped"])))
            if (causes["other"] or 0) > 0:
                out.append(("note", "%s: %d refused pixel(s) are of a class the census cannot name (`other`)" % (stamp, causes["other"])))
        if w["view"] == "on":
            out.append(("note", "%s: the refusal view was painting: the headset showed the prep's classification, not the world" % stamp))
    measured = [w for w, s in zip(windows, states) if s == "measured"]
    counted = [w for w, s in zip(windows, states) if s != "census-off"]
    treated = sum(w["treated"] or 0 for w in counted)
    if counted and not measured:
        if treated:
            out.append(("WARN", "the census never measured: the route treated %d frame(s) over %d window(s) and no sample was read back"
                        % (treated, len(counted))))
        else:
            out.append(("note", "the census was on for %d window(s) and the route treated no frame in any of them: nothing was measured "
                        "(not owning the world: a ship, a menu, or the route key off)" % len(counted)))
    return out


def print_refusal_census(windows, events=None):
    """The refusal-census section of --camera-census: the share of the treated pixels whose history the resolver's prep refused, per 5 s
    window and by cause, with the steady-detail key's state and the refusal view's in each window, then totals by key state and the
    findings. Returns the findings."""
    events = events or {}
    every = next((w["every"] for w in windows if w["every"]), None)
    print("\n== refusal census (advanced.vr_camera_census: the route's own resolve, the prep's per-pixel classification, one sample in %s "
          "of the resolves that ask; shares are of the pixels the samples examined) ==" % (every or "?"))
    if not windows:
        print("none: no `vr world route refusal 5s:` line in this log (the route never engaged, or this build predates the census; with the "
              "route on the line is printed while advanced.vr_camera_census is on, and in every window where the steady-detail depth check "
              "counted frames, which a current build always does: its steady detail is always on, and only a log from a build that had the "
              "setting may say off)")
        return []
    states = [refusal_state(w) for w in windows]
    off_windows = [w for w, s in zip(windows, states) if s == "census-off"]
    for w, s in zip(windows, states):
        if s == "census-off":
            continue   # summarised below, one line a key state: a long flight has hundreds of them
        route = w["route"] or {}
        inject = w["inject"] or {}
        context = "route state=%s jitter=%s fp-mode %s, inj-fp %s, struct fov %s" % (
            route.get("state", "?"), route.get("jitter", "?"), route.get("fp-mode", "?"), inject.get("inj-fp", "?"), inject.get("fov", "?"))
        head = "%s steady-detail=%s view=%s census=%s" % (w["ts"] or "(no stamp)", w["steady"], w["view"], "on" if w["census"] else "off")
        if s == "measured":
            named = [(name, w["causes"][name]) for name, _ in REFUSAL_CAUSES if w["causes"][name]]
            mix = ", ".join("%s %s" % (name, _pct(n, w["pixels"])) for name, n in named) if named else "none refused"
            # (printed only when a skinned pixel took its motion from target 7: a zero depends on whether a character was in view)
            skinned = "" if not w.get("skinned") else "; skinned-joined %d (F2 on foot: pixels that took their exact motion from target 7)" % w["skinned"]
            print("%s: MEASURED %d sample(s) of %dx%d (%d asked, %d dispatched, %d dropped), treated %d; pixels %d; refused %s (%d): %s; "
                  "stale-kept %s%s%s | %s"
                  % (head, w["read"], w["w"] or 0, w["h"] or 0, w["asked"], w["sampled"], w["dropped"] or 0, w["treated"], w["pixels"],
                     _pct(w["refused"], w["pixels"]), w["refused"], mix, _pct(w["kept"] or 0, w["pixels"]),
                     _stale_tail(w["steady"], w["kept"], w["causes"]["stale-refused"], w["check_ran"], w["check_skipped"]), skinned, context))
        else:
            reason = {
                "idle": "the route treated no frame in this window (nothing was measured)",
                "no-ask": "the route treated %s frame(s) and none asked for the census" % w["treated"],
                "no-sample": "%s ask(s), fewer than one sample's worth (every %s)" % (w["asked"], w["every"]),
                "no-read": "%s sample(s) dispatched, none read back" % w["sampled"],
                "unreadable": "a counter did not parse",
            }[s]
            print("%s: NOT MEASURED: %s | %s" % (head, reason, context))
    off_keys = []
    for w in off_windows:
        if w["steady"] not in off_keys:
            off_keys.append(w["steady"])
    for key in off_keys:
        group = [w for w in off_windows if w["steady"] == key]
        counted = [w for w in group if w["check_ran"] is not None]
        print("census off in %d window(s), steady-detail=%s (the line is printed for the key): the route treated %d frame(s)%s"
              % (len(group), key, sum(w["treated"] or 0 for w in group),
                 "; depth-check %d ran, %d skipped" % (sum(w["check_ran"] for w in counted), sum(w["check_skipped"] for w in counted))
                 if counted else ""))
    measured = [(w, s) for w, s in zip(windows, states) if s == "measured"]
    keys = []
    for w, _ in measured:
        if w["steady"] not in keys:
            keys.append(w["steady"])
    for key in keys:
        group = [w for w, _ in measured if w["steady"] == key]
        pixels = sum(w["pixels"] for w in group)
        refused = sum(w["refused"] for w in group)
        kept = sum(w["kept"] or 0 for w in group)
        with_check = [w for w in group if w["check_ran"] is not None]
        ran = sum(w["check_ran"] for w in with_check) if with_check else None
        skipped = sum(w["check_skipped"] for w in with_check) if with_check else None
        totals = {name: sum(w["causes"][name] or 0 for w in group) for name, _ in REFUSAL_CAUSES}
        mix = ", ".join("%s %s" % (name, _pct(n, pixels)) for name, n in totals.items() if n)
        of_refused = ", ".join("%s %.1f%%" % (name, 100.0 * n / refused) for name, n in totals.items() if n) if refused else ""
        print("totals, steady-detail=%s: %d measured window(s), %d sample(s), pixels %d, refused %s (%d)%s; stale-kept %s%s%s"
              % (key, len(group), sum(w["read"] for w in group), pixels, _pct(refused, pixels), refused,
                 ": %s" % mix if mix else " (none refused)", _pct(kept, pixels),
                 _stale_tail(key, kept, totals["stale-refused"], ran, skipped),
                 "; of the refused: %s" % of_refused if of_refused else ""))
        if len(group) > 1:
            shares = [100.0 * w["refused"] / w["pixels"] for w in group]
            print("    per-window refused share: min %.3f%%, max %.3f%% over %d window(s)" % (min(shares), max(shares), len(group)))
    seen = [(name, text) for name, text in REFUSAL_CAUSES if any((w["causes"][name] or 0) for w, _ in measured)]
    if seen:
        print("causes seen: %s" % "; ".join("%s = %s" % (name, text) for name, text in seen))
    for key in ("steady-detail", "refusal-view"):
        for line in events.get(key, [])[:6]:
            print("route log: %s" % line[:240])
    findings = refusal_findings(windows)
    for level, text in findings:
        print("%s %s" % ("!!" if level == "WARN" else "refusal note:", text))
    warns = sum(1 for level, _ in findings if level == "WARN")
    asked = len(windows) - len(off_windows)
    if warns:
        print("refusal census: WARN (%d finding(s) above)" % warns)
    elif measured:
        print("refusal census: consistent (%d of %d window(s) measured)%s" % (
            len(measured), asked, "; %d other window(s) had the census off" % len(off_windows) if off_windows else ""))
    elif off_windows and not asked:
        print("refusal census: the census key was off in all %d window(s): nothing to measure (the lines carry the steady-detail key's "
              "depth-check frames)" % len(off_windows))
    else:
        print("refusal census: nothing measured (%d window(s))" % asked)
    return findings


def census_join(c, tol=CENSUS_JOIN_TOL):
    """(B): every eye draw's rows against every logged call's rows. Returns one
    dict per eye draw: {eye, matches: [call rows], scope}, scope being `frame` (a
    call of the same frame matched), `other-frame` (only a call of another frame
    did: same camera, same pose, unusual), `none` (no call matched although the
    frame's sequence was logged), `no-sequence` (that frame's calls were not
    logged) or `no-rows` (the readback failed)."""
    by_frame = {}
    every = []
    for seq in c["sequences"]:
        for row in seq["rows"]:
            by_frame.setdefault(seq["frame"], []).append(row)
            every.append(row)

    def matching(rows, eye_rows):
        return [r for r in rows if r["rows"] and len(r["rows"]) == len(eye_rows) and
                all(abs(a - b) <= tol for a, b in zip(r["rows"], eye_rows))]

    out = []
    for eye in c["eyes"]:
        if not eye["rows"]:
            out.append({"eye": eye, "matches": [], "scope": "no-rows"})
            continue
        same = matching(by_frame.get(eye["frame"], []), eye["rows"])
        if same:
            out.append({"eye": eye, "matches": same, "scope": "frame"})
            continue
        other = matching(every, eye["rows"])
        if other:
            out.append({"eye": eye, "matches": other, "scope": "other-frame"})
        else:
            out.append({"eye": eye, "matches": [],
                        "scope": "none" if eye["frame"] in by_frame else "no-sequence"})
    return out


def census_runs(rows):
    """A frame's calls as runs of consecutive calls that share (kind, caller,
    tone): [(kind, caller, tone, count, first n, last n)]."""
    runs = []
    for r in rows:
        key = (r["kind"], r["caller"], r["tone"])
        if runs and runs[-1][:3] == key:
            runs[-1] = key + (runs[-1][3] + 1, runs[-1][4], r["n"])
        else:
            runs.append(key + (1, r["n"], r["n"]))
    return runs


def _cequal(a, b):
    """Two signature values the same, to the precision a log line holds."""
    if a is None or b is None:
        return a is b
    if isinstance(a, tuple):
        return isinstance(b, tuple) and len(a) == len(b) and \
            all(_cequal(x, y) for x, y in zip(a, b))
    if isinstance(a, int) and isinstance(b, int):
        return a == b
    if a != a or b != b:
        return False
    return abs(a - b) <= 1e-4 * max(1.0, abs(a), abs(b))


def _cg(value):
    """A value for the report: floats to six digits, tuples as (a, b)."""
    if value is None:
        return "-"
    if isinstance(value, tuple):
        return "(" + ", ".join(_cg(v) for v in value) + ")"
    if isinstance(value, float):
        return "%.6g" % value
    if isinstance(value, int):
        return "0x%X" % value if value > 0xFFFF else str(value)
    return str(value)


def _ckinds_text(kinds):
    """{3: 54, 5: 3} -> `k3 x54, k5 x3`, in kind order."""
    return ", ".join("k%s x%d" % (k, kinds[k]) for k in sorted(kinds, key=lambda k: (k is None, k or 0))) or "none"


def census_all_rows(c):
    return [r for seq in c["sequences"] for r in seq["rows"]]


def census_camera_calls(c):
    """Per camera, from the logged calls alone: {ptr: {kinds {kind: n}, frames {frame: {kind: n}}, before, after, none
    (kind-3 calls by the tone), calls, callers, views}}. The kind is the CALL's own; the camera line's kind is that of
    the first call the census ever heard and says nothing about the logged frames."""
    stats = {}
    for seq in c["sequences"]:
        for r in seq["rows"]:
            s = stats.setdefault(r["camera"], {"kinds": {}, "frames": {}, "before": 0, "after": 0, "none": 0,
                                               "calls": 0, "callers": set(), "views": set()})
            s["calls"] += 1
            s["kinds"][r["kind"]] = s["kinds"].get(r["kind"], 0) + 1
            per = s["frames"].setdefault(seq["frame"], {})
            per[r["kind"]] = per.get(r["kind"], 0) + 1
            if r["kind"] == 3 and r["tone"] in ("before", "after", "none"):
                s[r["tone"]] += 1
            s["callers"].add(r["caller"])
            s["views"].add(r["view"])
    return stats


def census_roles(c, join):
    """Who is who, from the calls. The eye cameras are the ones an eye draw's rows
    joined to. The world camera is the camera with the most kind-3 calls before the
    tone in the logged sequences (else the most kind-3 calls; the first seen on a
    tie), whatever its first-seen kind was. Every other camera with a kind-3 call is
    world-side (a pass's, a probe's). Returns {eye, world, world_side, other, stats}:
    sets of camera pointers, `world` one pointer or None, `other` the cameras with
    no kind-3 call or no logged call at all."""
    eye = set()
    for j in join:
        for m in j["matches"]:
            eye.add(m["camera"])
    stats = census_camera_calls(c)
    position = {p: i for i, p in enumerate(c["order"])}
    kind3 = {p for p, s in stats.items() if s["kinds"].get(3, 0) and p not in eye}
    world = None
    if kind3:
        world = sorted(kind3, key=lambda p: (-(stats[p]["before"] + stats[p]["none"]), -stats[p]["kinds"].get(3, 0),
                                             position.get(p, 1 << 30), p))[0]
    known = set(c["order"]) | set(stats)
    return {"eye": eye, "world": world, "world_side": kind3,
            "other": {p for p in known if p not in eye and p not in kind3}, "stats": stats}


def census_projections(rows):
    """One camera's kind-3 calls grouped by the projection their composed rows carry (scale and near): a second
    projection on the same object is a second role (the first-person weapon camera shares the scene camera's object
    with a tighter field of view and a larger near plane). Returns groups, biggest first: {xs, ys, near, shift,
    count, frames {frame: n}, roles {role: n}, injected, callers, views}. Calls with no rows are not grouped."""
    groups = []
    for r in rows:
        g = r.get("geo")
        if r["kind"] != 3 or g is None:
            continue
        for grp in groups:
            if abs(g["xs"] - grp["xs"]) <= CENSUS_PROJ_TOL * grp["xs"] and \
                    abs(g["ys"] - grp["ys"]) <= CENSUS_PROJ_TOL * grp["ys"] and \
                    abs(g["near"] - grp["near"]) <= CENSUS_PROJ_TOL * max(abs(grp["near"]), 1e-9):
                break
        else:
            grp = {"xs": g["xs"], "ys": g["ys"], "near": g["near"], "shift": g["shift"], "count": 0, "frames": {},
                   "roles": {}, "injected": 0, "callers": set(), "views": set()}
            groups.append(grp)
        grp["count"] += 1
        grp["frames"][r["frame"]] = grp["frames"].get(r["frame"], 0) + 1
        if r.get("role") is not None:
            grp["roles"][r["role"]] = grp["roles"].get(r["role"], 0) + 1
        if r.get("inj") == 1:
            grp["injected"] += 1
        grp["callers"].add(r["caller"])
        grp["views"].add(r["view"])
    return sorted(groups, key=lambda grp: (-grp["count"], grp["near"]))


def census_call_value(row, name):
    if name == "kind":
        return row["kind"]
    g = row.get("geo")
    return None if g is None else g[name]


def census_separation(c, roles):
    """(A): which fields of a call's own signature (its kind and what its composed rows say: aspect, field of view, near
    plane, off-centre shift) tell the eye cameras' calls from the world-side kind-3 calls. A field separates when no
    eye call has a value any world-side call has. Returns a list of (field, eye values, world-side values), distinct
    values only."""
    rows = census_all_rows(c)
    eye_calls = [r for r in rows if r["camera"] in roles["eye"]]
    world_calls = [r for r in rows if r["kind"] == 3 and r["camera"] not in roles["eye"]]
    out = []
    if not eye_calls or not world_calls:
        return out

    def distinct(calls, name):
        seen = []
        for r in calls:
            v = census_call_value(r, name)
            if v is not None and not any(_cequal(v, s) for s in seen):
                seen.append(v)
        return seen

    for field in CENSUS_CALL_FIELDS:
        ev, wv = distinct(eye_calls, field), distinct(world_calls, field)
        if ev and wv and all(not _cequal(e, w) for e in ev for w in wv):
            out.append((field, ev, wv))
    return out


def census_order(c, roles):
    """(C): per logged sequence, whether every kind-3 call of a world-side camera comes before every call of an eye camera,
    by position, by the tone flag and by the draw ordinal. Returns a list of dicts."""
    out = []
    for seq in c["sequences"]:
        world = [r for r in seq["rows"] if r["kind"] == 3 and r["camera"] in roles["world_side"]]
        eye = [r for r in seq["rows"] if r["camera"] in roles["eye"]]
        entry = {"frame": seq["frame"], "world": len(world), "eye": len(eye),
                 "by_position": None, "world_tone": {}, "eye_tone": {},
                 "world_last_draw": None, "eye_first_draw": None}
        if world and eye:
            entry["by_position"] = max(r["n"] for r in world) < min(r["n"] for r in eye)
        for r in world:
            entry["world_tone"][r["tone"]] = entry["world_tone"].get(r["tone"], 0) + 1
        for r in eye:
            entry["eye_tone"][r["tone"]] = entry["eye_tone"].get(r["tone"], 0) + 1
        wd = [r["draw"] for r in world if r["draw"] is not None]
        ed = [r["draw"] for r in eye if r["draw"] is not None]
        entry["world_last_draw"] = max(wd) if wd else None
        entry["eye_first_draw"] = min(ed) if ed else None
        out.append(entry)
    return out


def _cfmt_tone(counts):
    return ", ".join("%s x%d" % (k, counts[k]) for k in sorted(counts)) or "none"


def census_expected_measure(frustum, dx, dy):
    """What flatCameraMeasureRowShift reads off the rows of a projection built from
    the window {left, right, down, up} moved by (dx, dy): (-(R+L)/(R-L), -(U+D)/(U-D)),
    the model's off-centre terms p8 and p9. None when the window has no width."""
    left, right, down, up = frustum[0] + dx, frustum[1] + dx, frustum[2] + dy, frustum[3] + dy
    if right == left or up == down:
        return None
    return (-(right + left) / (right - left), -(up + down) / (up - down))


def census_shift_fit(meas, frustum, shift):
    """Which way the eye's rows carry the shift EDVR advertised. The DLL's `leak=`
    column assumes the game builds its eye camera from the frustum moved by +shift;
    that sign is not proven, so the reader tries every sign of each axis and the
    unshifted frustum and names the best fit. Returns (label, residual, runner-up
    label, its residual), residuals being the largest |measured - expected| of the
    two axes, in NDC."""
    fits = []
    for name, sx, sy in (("the advertised frustum moved by (+shift.x, +shift.y)", 1, 1),
                         ("the frustum moved by (+shift.x, -shift.y)", 1, -1),
                         ("the frustum moved by (-shift.x, +shift.y)", -1, 1),
                         ("the frustum moved by (-shift.x, -shift.y)", -1, -1),
                         ("the unshifted frustum (the shift is not in the rows)", 0, 0)):
        expected = census_expected_measure(frustum, sx * shift[0], sy * shift[1])
        if expected is None or meas is None or len(meas) != 2:
            continue
        fits.append((max(abs(meas[0] - expected[0]), abs(meas[1] - expected[1])), name))
    if not fits:
        return None
    fits.sort()
    runner = fits[1] if len(fits) > 1 else (None, None)
    return fits[0][1], fits[0][0], runner[1], runner[0]


def census_phase_ndc(phase, width, height):
    """The shift in NDC a phase in render pixels (positive right/down) is, flatProjectionJitter's rule: x = 2 px / W,
    y = -2 py / H (NDC y is up)."""
    return (2.0 * phase[0] / width, -2.0 * phase[1] / height)


def _phase_nonzero(phase):
    return phase is not None and abs(phase[0]) + abs(phase[1]) > 0.0


def census_verdict(c, routes, roles=None, events=None):
    """The stage 2 verdict (design doc section 82, stage 2): did the world phase reach the kind-3 cameras and stay out
    of the eyes? Returns [(tag, status, text)] for (i) LEAK, (ii) KIND-3 ROWS CARRY THE PHASE, (iii) INJECTED KINDS,
    (iv) OFF-THREAD / UNREADABLE, (v) the ROLES and (vi) the INJECTION WINDOW, then the notes; status is PASS, WARN,
    STOP or n/a (what this log cannot say: an older census, a route that never jittered). `events` is route_events(text)."""
    events = events or {}
    out = []

    def add(tag, status, text):
        out.append((tag, status, text))

    def total(windows, key):
        values = [_cint(route_tok(w, key)) for w in windows]
        return sum(v for v in values if v is not None), sum(1 for v in values if v is not None)

    eyes, seqs = c["eyes"], c["sequences"]
    jitter_on = [w for w in routes if route_tok(w, "jitter") == "on"]
    tokens_seen = any(e["phase_state"] != "absent" for e in eyes) or any(s["phase_state"] != "absent" for s in seqs)
    jittering_seen = bool(jitter_on) or any(e["phase_state"] == "on" for e in eyes) or \
        any(s["phase_state"] == "on" for s in seqs)
    route_tokens = any(route_tok(w, "jitter") is not None for w in routes)
    older = "this log predates stage 2: no phase= token on the census lines%s" % (
        "" if not route_tokens else ", though the route line has jitter=")

    # The render size the world phase is in, from the log itself (the route's hdr=, the JITTERED line, or the panel patch's size): never
    # assumed (5040x2835 is what the legacy width gives a 4032 px eye, not what every rig has).
    size = route_hdr(routes, events)

    # ---- (i) the leak into the eye cameras ----
    nonzero = [e for e in eyes if e["phase_state"] == "on" and _phase_nonzero(e["phase"])]
    geometry = c["geometry"]
    if not tokens_seen and not route_tokens:
        add("i", "n/a", "LEAK: not judged: %s" % older)
    elif not nonzero:
        states = {}
        for e in eyes:
            states[e["phase_state"]] = states.get(e["phase_state"], 0) + 1
        reason = ("%d eye draw(s) were read back (%d with a zero phase, %d with the route not jittering)"
                  % (len(eyes), states.get("on", 0), states.get("off", 0))) if eyes else "no eye draw was read back"
        if jittering_seen:
            early = [e for e in eyes if e["phase_state"] == "off"]
            add("i", "STOP", "LEAK: no eye draw was sampled with a non-zero phase although the route was jittering%s "
                "(%s).%s The census samples a frame only when its phase is non-zero while the route jitters, so the "
                "eye rows were never read in a frame that could leak"
                % (" (jitter=on in %d route window(s))" % len(jitter_on) if jitter_on else "", reason,
                   " %d eye draw(s) were read earlier with phase=- (the route not jittering yet): the eye budget was "
                   "spent before it started; turn the census on after the route owns the world." % len(early)
                   if early else ""))
        else:
            add("i", "n/a", "LEAK: not judged: the route never jittered in this log (%s)" % reason)
    else:
        worst, measured, unmeasured, sign_fit, shifted_eyes = 0.0, 0, 0, 0, 0
        worst_at = None
        for e in nonzero:
            g = geometry.get((e["eye"], e["frame"]))
            leak = None
            if g and g["known"] and g["leak"] and all(v == v for v in g["leak"]):
                leak = max(abs(v) for v in g["leak"])
                # An eye shift that is on (the design has it off while the world is owned) makes `leak=` depend on the
                # sign the game carries it with, which is not proven: take the best fit over the signs.
                if g["shift"] and any(abs(v) > 1e-12 for v in g["shift"]) and g["frustum"] and e["meas"]:
                    shifted_eyes += 1
                    fit = census_shift_fit(e["meas"], g["frustum"], g["shift"])
                    if fit:
                        leak = min(leak, fit[1])
                        sign_fit += 1
            if leak is None:
                unmeasured += 1
                continue
            measured += 1
            if leak > worst:
                worst, worst_at = leak, (e["eye"], e["frame"])
        px = max(max(abs(e["phase"][0]), abs(e["phase"][1])) for e in nonzero)
        if size is not None:
            ndc = max(max(abs(census_phase_ndc(e["phase"], size[0], size[1])[0]), abs(census_phase_ndc(e["phase"], size[0], size[1])[1]))
                      for e in nonzero)
            base = ("%d of %d eye draw(s) had a non-zero world phase (up to %.4f px, about %.1e NDC at %dx%d)"
                    % (len(nonzero), len(eyes), px, ndc, size[0], size[1]))
        else:
            base = ("%d of %d eye draw(s) had a non-zero world phase (up to %.4f px; the render size is not in this log, so no NDC figure)"
                    % (len(nonzero), len(eyes), px))
        # the same frame's two phases (the sequence header's and the eye line's) are one number
        disagree = []
        for e in nonzero:
            for s in seqs:
                if s["frame"] == e["frame"] and s["phase_state"] == "on" and s["phase"] and \
                        (abs(s["phase"][0] - e["phase"][0]) > 1e-3 or abs(s["phase"][1] - e["phase"][1]) > 1e-3):
                    disagree.append(e["frame"])
        if not measured:
            add("i", "STOP", "LEAK: %s, but none could be measured (the eye-geometry line was missing, unavailable or not "
                "finite for %d of them)" % (base, unmeasured))
        elif worst > CENSUS_LEAK_STOP:
            add("i", "STOP", "LEAK: %s; worst |leak| %.2e NDC at eye %d frame %d, over %.0e: the world phase reached the "
                "eye camera (the baseline with no phase is 1e-8)" % (base, worst, worst_at[0], worst_at[1], CENSUS_LEAK_STOP))
        elif worst >= CENSUS_LEAK_PASS or unmeasured or disagree or shifted_eyes:
            why = []
            if worst >= CENSUS_LEAK_PASS:
                why.append("worst |leak| %.2e NDC is between %.0e and %.0e" % (worst, CENSUS_LEAK_PASS, CENSUS_LEAK_STOP))
            if unmeasured:
                why.append("%d eye draw(s) could not be measured" % unmeasured)
            if disagree:
                why.append("the sequence and eye lines of frame(s) %s name different phases"
                           % ", ".join(str(f) for f in sorted(set(disagree))))
            if shifted_eyes:
                why.append("%d eye draw(s) had the eye shift on while the world phase was non-zero (the design has it off "
                           "while owned; their leak is the best fit over the shift's signs)" % shifted_eyes)
            add("i", "WARN", "LEAK: %s; %s; worst |leak| %.2e NDC over %d measured" % (base, "; ".join(why), worst, measured))
        else:
            add("i", "PASS", "LEAK: %s; worst |leak| %.2e NDC over %d measured, below %.0e (the baseline with no phase is "
                "1e-8; a phase leaking into an eye would read 1e-4)" % (base, worst, measured, CENSUS_LEAK_PASS))

    # ---- (ii) the kind-3 calls carry the phase ----
    sampled =[s for s in seqs if s["phase_state"] == "on" and _phase_nonzero(s["phase"])]
    calls_have_inj = any(r["inj"] is not None for r in census_all_rows(c))
    if not seqs or not (tokens_seen or route_tokens):
        add("ii", "n/a", "KIND-3 ROWS CARRY THE PHASE: not judged: %s" % (older if seqs else "no call sequence was logged"))
    elif not sampled:
        if jittering_seen:
            add("ii", "STOP", "KIND-3 ROWS CARRY THE PHASE: no call sequence was logged with a non-zero phase although the "
                "route was jittering (%d sequence(s) logged, %d with phase=-, %d with a zero phase)"
                % (len(seqs), sum(1 for s in seqs if s["phase_state"] == "off"),
                   sum(1 for s in seqs if s["phase_state"] == "on")))
        else:
            add("ii", "n/a", "KIND-3 ROWS CARRY THE PHASE: not judged: the route never jittered in this log")
    elif not calls_have_inj:
        add("ii", "n/a", "KIND-3 ROWS CARRY THE PHASE: not judged: the call lines carry no inj= token (an older census)")
    elif size is None:
        add("ii", "WARN", "KIND-3 ROWS CARRY THE PHASE: the render size is unknown (no `vr world route 5s:` line has a "
            "non-zero hdr=WxH), so the phase in pixels cannot be turned into NDC")
    else:
        width, height = size[0], size[1]
        by_role = {}
        notinj, afterinj = 0, 0
        signs = {"y flipped (y = +2 py / H)": 0, "x flipped (x = -2 px / W)": 0, "both axes flipped": 0}
        bad = 0
        for s in sampled:
            exp = census_phase_ndc(s["phase"], width, height)
            for r in s["rows"]:
                if r["kind"] != 3 or r["geo"] is None or r["role"] not in ("scene", "fp"):
                    continue
                if r["inj"] != 1:
                    if r["tone"] != "after":
                        notinj += 1
                    continue
                if r["tone"] == "after":
                    afterinj += 1
                sx, sy = r["geo"]["shift"]
                err = max(abs(sx - exp[0]), abs(sy - exp[1]))
                b = by_role.setdefault(r["role"], {"n": 0, "worst": 0.0})
                b["n"] += 1
                b["worst"] = max(b["worst"], err)
                if err > CENSUS_PHASE_TOL:
                    # A call that misses its phase: which sign convention, if any, does it fit? (all the misses must fit one)
                    bad += 1
                    for label, kx, ky in (("y flipped (y = +2 py / H)", 1, -1), ("x flipped (x = -2 px / W)", -1, 1),
                                          ("both axes flipped", -1, -1)):
                        if max(abs(sx - kx * exp[0]), abs(sy - ky * exp[1])) <= CENSUS_PHASE_TOL:
                            signs[label] += 1
        checked = sum(b["n"] for b in by_role.values())
        worst = max([b["worst"] for b in by_role.values()] or [0.0])
        desc = ", ".join("%s %d call(s) worst %.2e" % ({"scene": "scene", "fp": "first-person"}[k], by_role[k]["n"],
                                                       by_role[k]["worst"]) for k in ("scene", "fp") if k in by_role)
        where = "expected: x = 2 px / W, y = -2 py / H at %dx%d (from %s)" % (width, height, size[2])
        if not checked:
            add("ii", "STOP", "KIND-3 ROWS CARRY THE PHASE: %d sequence(s) with a non-zero phase hold no injected (inj=1) "
                "scene or first-person call with rows: the phase never reached a kind-3 call (%d scene/first-person call(s) "
                "were not injected)" % (len(sampled), notinj))
        elif worst > CENSUS_PHASE_STOP:
            fit = [k for k, v in sorted(signs.items()) if bad and v == bad]
            add("ii", "STOP", "KIND-3 ROWS CARRY THE PHASE: %d injected call(s) in %d sequence(s) do not measure the phase "
                "they were given (%s; worst |measured - expected| %.2e NDC, over %.0e; %d of them miss)%s; %s"
                % (checked, len(sampled), desc, worst, CENSUS_PHASE_STOP, bad,
                   ("; the rows that miss DO fit the phase with %s" % fit[0]) if fit else "", where))
        elif worst > CENSUS_PHASE_TOL or notinj or afterinj:
            why = []
            if worst > CENSUS_PHASE_TOL:
                why.append("worst |measured - expected| %.2e NDC is between %.0e and %.0e" % (worst, CENSUS_PHASE_TOL, CENSUS_PHASE_STOP))
            if notinj:
                why.append("%d scene/first-person call(s) before the tone were not injected (inj=0)" % notinj)
            if afterinj:
                why.append("%d injected call(s) came after the tone (the window should have closed at the trigger)" % afterinj)
            add("ii", "WARN", "KIND-3 ROWS CARRY THE PHASE: %d injected call(s) in %d sequence(s) (%s); %s; %s"
                % (checked, len(sampled), desc, "; ".join(why), where))
        else:
            add("ii", "PASS", "KIND-3 ROWS CARRY THE PHASE: %d injected kind-3 call(s) in %d sequence(s) with a non-zero "
                "phase measure the phase they were given (%s), within %.0e NDC; %s"
                % (checked, len(sampled), desc, CENSUS_PHASE_TOL, where))

    # ---- (iii) only kind 3 is injected ----
    kinds_windows = [w for w in routes if _ckinds(route_tok(w, "inj-kinds")) is not None]
    injected = {}
    for w in kinds_windows:
        for k, v in _ckinds(route_tok(w, "inj-kinds")).items():
            injected[k] = injected.get(k, 0) + v
    other_injected = {k: v for k, v in injected.items() if k != "3" and v}
    census_bad = [r for r in census_all_rows(c) if r["inj"] == 1 and r["kind"] != 3]
    census_inj = sum(1 for r in census_all_rows(c) if r["inj"] == 1)
    if other_injected or census_bad or events.get("kind-other"):
        parts = []
        if other_injected:
            parts.append("the route's inj-kinds= counts %s" % ", ".join("kind %s x%d" % kv for kv in sorted(other_injected.items())))
        if census_bad:
            parts.append("%d logged call(s) with inj=1 are not kind 3 (%s)"
                         % (len(census_bad), ", ".join(sorted({"k%s" % r["kind"] for r in census_bad}))))
        if events.get("kind-other"):
            parts.append("the route logged: %s" % events["kind-other"][0][:200])
        add("iii", "STOP", "INJECTED KINDS: a kind other than 3 was injected: %s" % "; ".join(parts))
    elif not kinds_windows and not calls_have_inj:
        add("iii", "n/a", "INJECTED KINDS: not judged: no route line has inj-kinds= and no call line has inj= (%s)"
            % older.replace("this log predates stage 2: ", ""))
    elif not injected.get("3") and not census_inj:
        add("iii", "WARN", "INJECTED KINDS: nothing was injected (inj-kinds=none in %d route window(s), no call with inj=1)"
            % len(kinds_windows))
    else:
        add("iii", "PASS", "INJECTED KINDS: only kind 3 was injected (route inj-kinds=3:%d over %d window(s); %d logged call(s) "
            "with inj=1, all kind 3)" % (injected.get("3", 0), len(kinds_windows), census_inj))

    # ---- (iv) nothing off the render thread, nothing unreadable ----
    window_kv = [_ckv(line) for _, line in c["windows"]]
    census_off = sum(_cint(k.get("off-thread")) or 0 for k in window_kv)
    census_unreadable = 0
    for k in window_kv:
        m = re.search(r"unreadable:(\d+)", k.get("kinds", ""))
        if m:
            census_unreadable += int(m.group(1))
    route_off, off_n = total(routes, "off-thread")
    route_unreadable, unr_n = total(routes, "unreadable")
    parts = []
    if census_off:
        parts.append("the census counted %d call(s) off the render thread" % census_off)
    if route_off:
        parts.append("the route counted %d off-thread call(s)" % route_off)
    if route_unreadable:
        parts.append("the route counted %d call(s) whose kind could not be read" % route_unreadable)
    if census_unreadable:
        parts.append("the census counted %d call(s) whose kind could not be read" % census_unreadable)
    if parts:
        add("iv", "STOP", "OFF-THREAD / UNREADABLE: %s" % "; ".join(parts))
    elif not window_kv and not off_n and not unr_n:
        add("iv", "n/a", "OFF-THREAD / UNREADABLE: not judged: no 5 s line to read")
    elif off_n or unr_n:
        add("iv", "PASS", "OFF-THREAD / UNREADABLE: none: census off-thread 0 and no unreadable kind over %d window line(s), "
            "route off-thread 0 and unreadable 0 over %d route window(s)" % (len(window_kv), max(off_n, unr_n)))
    else:
        add("iv", "PASS", "OFF-THREAD / UNREADABLE: none on the census side (off-thread 0 and no unreadable kind over %d window "
            "line(s)); the route's off-thread= and unreadable= tokens are absent (an older route line, or none)" % len(window_kv))

    # ---- (v) the roles ----
    names = ("inj-scene", "inj-fp", "inj-refused", "warming", "aux", "after", "unsupported", "other-kind")
    sums = {n: total(routes, n) for n in names}
    role_calls = {}
    for s in sampled if sampled else []:
        for r in s["rows"]:
            if r["role"] in ("scene", "fp", "aux"):
                t = role_calls.setdefault(r["role"], [0, 0])
                t[0] += 1
                t[1] += 1 if r["inj"] == 1 else 0
    aux_injected = sum(1 for r in census_all_rows(c) if r["role"] == "aux" and r["inj"] == 1)
    if not any(v[1] for v in sums.values()) and not role_calls and not calls_have_inj:
        add("v", "n/a", "ROLES: not judged: no route token and no role= on a call line (%s)"
            % older.replace("this log predates stage 2: ", ""))
    else:
        route_text = ", ".join("%s %d" % (n, sums[n][0]) for n in names if sums[n][1]) or "no route token"
        call_text = ", ".join("%s %d (%d injected)" % ({"scene": "scene", "fp": "first-person", "aux": "auxiliary"}[k],
                                                      role_calls[k][0], role_calls[k][1])
                              for k in ("scene", "fp", "aux") if k in role_calls) or "none in a non-zero-phase sequence"
        body = "ROLES: the route counted %s; logged calls in non-zero-phase sequences: %s" % (route_text, call_text)
        # The weapon (the stage 2 experiment build): the fold-in's mode counts say how many frames drew a weapon (mode 1 or 2), inj-fp how
        # many of its calls the role test credited, and the inject line's fov= range whether the struct carries a second, tighter field of
        # view for the test to find. Flight 2: inj-fp 0 with the fold-in in mode 2 on every weapon frame, and no fov= token.
        modes = [_cmodes(route_tok(w, "fp-mode")) for w in routes]
        m1 = sum(m[1] for m in modes if m)
        m2 = sum(m[2] for m in modes if m)
        folded = m1 + m2
        fov_lo, fov_hi, fov_n = route_fov(routes)
        fp_calls, scene_calls = sums["inj-fp"][0], sums["inj-scene"][0]
        if fov_n:
            body += "; struct field of view %.4f..%.4f rad over %d window(s)" % (fov_lo, fov_hi, fov_n)
        if sums["inj-fp"][1] and fp_calls and folded:
            body += "; the weapon's fold-in ran in %d frame(s) (mode 1: %d, mode 2: %d), about %.1f first-person call(s) credited a frame" % (
                folded, m1, m2, fp_calls / float(folded))
        refused = sums["inj-refused"][0]
        write_fail, _ = total(routes, "write-fail")
        if aux_injected:
            add("v", "STOP", "%s: %d auxiliary call(s) were injected (an excluded role must never get a phase)" % (body, aux_injected))
        elif refused or write_fail:
            add("v", "WARN", "%s: %d call(s) were refused for want of a write and %d write(s) failed (a flush or a restore that failed "
                "can leave a phase in a camera)" % (body, refused, write_fail))
        elif sums["inj-scene"][1] and not sums["inj-scene"][0]:
            add("v", "WARN", "%s: no scene call was injected" % body)
        elif events.get("no-scene-call"):
            add("v", "WARN", "%s; the route logged: %s" % (body, events["no-scene-call"][0][:200]))
        elif sums["inj-fp"][1] and not fp_calls and folded:
            if fov_n and fov_lo < CENSUS_FP_FOV_RATIO * fov_hi:
                why = ("the struct carries two fields of view (%.4f and %.4f rad), so the role test should have told the weapon's calls "
                       "from the scene's: a fault in the field-of-view test" % (fov_lo, fov_hi))
            elif fov_n:
                why = ("the struct carries one field of view (%.4f rad) in every window, so the weapon's calls cannot be told from the "
                       "scene's by it" % fov_hi)
            else:
                why = "no inject line carries a fov= range (a build that predates the field-of-view test: flight 2's picture)"
            add("v", "WARN", "%s: no first-person call was credited (inj-fp 0) although the weapon's fold-in ran in %d frame(s) (mode 1: %d, "
                "mode 2: %d), so the weapon's pixels refuse their history; %s" % (body, folded, m1, m2, why))
        elif fp_calls and m2 and not m1:
            add("v", "WARN", "%s: first-person calls were credited (inj-fp %d) and yet the fold-in ran only in mode 2 (%d frame(s), mode 1 in "
                "none): the frames that credited the weapon are not the frames its map was made in" % (body, fp_calls, m2))
        elif fp_calls and scene_calls and fp_calls >= scene_calls:
            add("v", "WARN", "%s: as many first-person calls as scene calls (%d against %d; flight 2's weapon was 15 of the world's 78): the "
                "role test's scene anchor is probably a wider screen-aspect camera, read the struct field of view range" % (
                    body, fp_calls, scene_calls))
        else:
            add("v", "PASS", body)

    # ---- (vi) the injection window: nothing is injected on a frame whose window the route had shut ----
    shut, shut_n = total(routes, "inj-shut")
    unnamed, unnamed_n = total(routes, "inj-unnamed")
    states = [route_tok(w, "jitter") for w in routes]
    faults = sum(1 for s in states if s == "fault")
    nohook = sum(1 for s in states if s == "no-hook")
    # Episodes the log itself shows in which the route let go or found no source: a run of jitter=unnamed windows, a drop from jitter=on to
    # anything else, and the route's own RELEASED lines. The first map or menu frame after a world frame cannot be told from a world frame
    # before its cameras refresh (they come before any draw), so ONE injected-on-an-unnamed-frame per such change is expected.
    episodes, previous = 0, None
    for s in states:
        if s == "unnamed" and previous != "unnamed":
            episodes += 1
        elif previous == "on" and s not in ("on", "unnamed", None):
            episodes += 1
        previous = s
    episodes += len(events.get("released", []))
    parts = []
    if shut:
        parts.append("inj-shut=%d: camera calls were INJECTED on a frame whose window the route had shut (it must always be 0)" % shut)
    if events.get("shut"):
        parts.append("the route logged: %s" % events["shut"][0][:200])
    # The route's STOP lines other than the two named here: a kind-other STOP is (iii)'s, and a shut-window STOP is quoted once, above.
    stops = [l for l in events.get("stop", []) if "kind other than 3" not in l and l not in events.get("shut", [])]
    if stops:
        parts.append("the route's own STOP line: %s" % stops[0][:200])
    if faults:
        parts.append("jitter=fault in %d route window(s)" % faults)
    if parts:
        add("vi", "STOP", "INJECTION WINDOW: %s" % "; ".join(parts))
    elif not shut_n and not unnamed_n and not states.count("unnamed") and not nohook:
        add("vi", "n/a", "INJECTION WINDOW: not judged: no route line has inj-shut= or inj-unnamed= (an older route line, or none)")
    elif nohook:
        add("vi", "WARN", "INJECTION WINDOW: jitter=no-hook in %d route window(s): the route wanted to jitter and the refresh hook was not live, so "
            "nothing was injected in them; inj-shut=%d" % (nohook, shut))
    elif unnamed > episodes:
        add("vi", "WARN", "INJECTION WINDOW: inj-shut=0, but inj-unnamed=%d exceeds the %d map/menu/release episode(s) this log shows (jitter=unnamed runs, "
            "drops out of jitter=on, RELEASED lines): calls were injected on frames that named no source more often than the scene changed" % (unnamed, episodes))
    else:
        add("vi", "PASS", "INJECTION WINDOW: inj-shut=0 over %d route window(s); inj-unnamed=%d (one per world-to-map change is expected: its first frame "
            "cannot be told from a world frame before the cameras refresh; this log shows %d such episode(s)); jitter=unnamed in %d window(s)"
            % (shut_n, unnamed, episodes, states.count("unnamed")))

    # ---- the pair check and the call rates, as notes ----
    checked, n1 = total(routes, "pair-checked")
    bad, n2 = total(routes, "pair-bad")
    if n1 or n2:
        add("note", "WARN" if bad else "note",
            "the route's own pair check of the rows it believes: %d checked, %d inconsistent%s"
            % (checked, bad, " (the rows do not carry the phase the route claims)" if bad else ""))
    if events.get("excluded"):
        sigs = []
        for line in events["excluded"]:
            m = re.search(r"kind 3 (aspect=\S+ fov=\S+ near=\S+ far=\S+ caller=\S+).*?: (\d+) call\(s\) so far", line)
            sigs.append("%s (%s call(s) so far)" % (m.group(1), m.group(2)) if m else line[:120])
        add("note", "note", "the route excluded %d kind-3 call signature(s) by role and never injected them (they need a role before they get a phase): %s"
            % (len(sigs), "; ".join(sigs[:4]) + ("; ..." if len(sigs) > 4 else "")))
    full = [k for k in window_kv if _cint(k.get("frames")) and k.get("on-foot-frames") == k.get("frames")]
    frames = sum(_cint(k["frames"]) for k in full)
    if frames:
        k5 = 0
        inj = 0
        inj_seen = False
        for k in full:
            m = re.search(r"(?:^|,)5:(\d+)", k.get("kinds", ""))
            k5 += int(m.group(1)) if m else 0
            if _cint(k.get("inj-calls")) is not None:
                inj += _cint(k["inj-calls"])
                inj_seen = True
        text = "a fully-on-foot census window holds %.1f kind-5 calls a frame (design: %.1f, two eyes x three call sites)" % (
            k5 / frames, CENSUS_KIND5_PER_FRAME)
        if inj_seen:
            text += "; %.1f injected calls a frame (design: %d-%d)" % (inj / frames, CENSUS_INJECTED_PER_FRAME[0],
                                                                      CENSUS_INJECTED_PER_FRAME[1])
        add("note", "note", text + " (over %d frame(s) in %d window(s))" % (frames, len(full)))
    return out


def print_stage2_verdict(c, routes, roles=None, events=None, refusals=None):
    """Prints the refusal-census section (when `refusals`, the parsed refusal windows, is given: an empty list says there were none) and
    then the verdict section, and returns the verdict's overall word: STOP, WARN, PASS or n/a. The refusal census has its own closing
    line and never changes the verdict's word."""
    if refusals is not None:
        print_refusal_census(refusals, events)
    print("\n== stage 2 verdict (PASS leak < %.0e NDC, STOP leak > %.0e; kind-3 rows within %.0e NDC of the phase) =="
          % (CENSUS_LEAK_PASS, CENSUS_LEAK_STOP, CENSUS_PHASE_TOL))
    verdict = census_verdict(c, routes, roles, events)
    events = events or {}
    if events.get("jittered") or events.get("released"):
        print("route log: the world was JITTERED from %d episode(s) (`the world is JITTERED from frame=`), RELEASED %d time(s)%s"
              % (len(events.get("jittered", [])), len(events.get("released", [])),
                 "; `camera rows disagree with the phase` %d time(s)" % len(events["rows-disagree"]) if events.get("rows-disagree") else ""))
    if routes:
        print("route lines: %d window(s), %d with jitter=on" % (len(routes), sum(1 for w in routes if route_tok(w, "jitter") == "on")))
    else:
        print("route lines: none (no `vr world route 5s:` line in this log: the route key was off, or this build predates it)")
    for tag, status, text in verdict:
        if status == "note":
            print("      note: %s" % text)
        else:
            print("%-5s (%s) %s" % (status, tag, text))
    statuses = [s for _, s, _ in verdict if s != "note"]
    core = [s for t, s, _ in verdict if t in ("i", "ii", "iii")]
    # A PASS needs the three lines that establish the injection (the eyes did not move, the kind-3 rows carry the phase, only kind 3
    # was injected) to have been judged and passed: a log that predates stage 2 passes (iv) for having nothing off-thread and says n/a.
    overall = "STOP" if "STOP" in statuses else "WARN" if "WARN" in statuses else \
        "PASS" if core and all(s == "PASS" for s in core) else "n/a"
    print("stage 2 verdict: %s (%d PASS, %d WARN, %d STOP, %d n/a)%s"
          % (overall, statuses.count("PASS"), statuses.count("WARN"), statuses.count("STOP"), statuses.count("n/a"),
             " -- not judged: this log does not show the injection (see the n/a lines)" if overall == "n/a" else ""))
    return overall


def _phase_text(state, phase):
    if state == "on":
        return "phase (%.4f, %.4f) px" % (phase[0], phase[1])
    if state == "off":
        return "the route was not jittering (phase=-)"
    return "no phase= token (a census that predates stage 2)"


# ---- the episodes (design-world-camera-motion-2026-09-30.md section 6, Phase 0) ----------------------------------------
# One frame sampled 30 frames after a trigger, whatever the frame is (src/d3d11/vr_camera_census_core.h: the lines are written by one function,
# vrCensusPrintEpisode, which tools\vr_camera_census_test also builds tools\camera_census_fixture.log with). Per episode: the header (the trigger, the journal,
# GuiFocus, the naming, the calls by kind and caller over ALL the frame's calls), the calls that printed (up to 120, those that matched something first), the pass's
# chosen rows with the calls whose view axes equal them, and the join: the first draw of each (depth, vertex shader, pixel shader) into the 2D screen's depth or an eye's, whether
# it writes depth, its b1 size and, for the first of each depth, the b1 rows 270..273 read back and the calls that composed them. The DLL matches over every call the frame
# recorded; this reader matches again over the calls that printed (the tolerance of the eye-draw join), so a call the cap kept out is named by its ordinal and said not to be printed.
CENSUS_EPISODE_RUNS_SHOWN = 12


def census_episode_call_label(r):
    """One call as the episode report names it: its camera, kind, caller, view and place against the tone."""
    return "camera 0x%X kind %s caller %s view %s tone %s draw %s" % (
        r["camera"], "-" if r["kind"] is None else r["kind"], "+0x%X" % r["caller"] if r["caller"] is not None else "-",
        "0x%X" % r["view"] if r["view"] else "-", r["tone"], "-" if r["draw"] is None else r["draw"])


def census_episode_join(ep, tol=CENSUS_JOIN_TOL):
    """Each join signature against the episode's printed calls. Returns one dict per signature: {join, printed [call rows whose composed rows are the
    rows the draw read, within tol], listed [the ordinals the DLL listed], unprinted [listed ordinals that are not among the printed calls], more [matches
    the DLL counted past its list], disagree [printed calls the two rules judge differently, when the DLL's list is whole]}. A signature whose rows were not
    read has none of these (`read` False)."""
    by_n = {r["n"]: r for r in ep["rows"]}
    out = []
    for j in ep["joins"]:
        entry = {"join": j, "read": j["rows"] is not None, "printed": [], "listed": list(j["match"] or []), "more": j["match_more"],
                 "unprinted": [], "disagree": []}
        if entry["read"]:
            entry["printed"] = [r for r in ep["rows"] if r["rows"] and len(r["rows"]) == len(j["rows"]) and
                                all(abs(a - b) <= tol for a, b in zip(r["rows"], j["rows"]))]
            entry["unprinted"] = [n for n in entry["listed"] if n not in by_n]
            if j["match"] is not None and not entry["more"]:
                printed_n = {r["n"] for r in entry["printed"]}
                entry["disagree"] = sorted(printed_n.symmetric_difference(n for n in entry["listed"] if n in by_n))
        out.append(entry)
    return out


def census_episode_pass(ep):
    """The pass's chosen rows against the printed calls: {pass, matched [call rows the DLL listed as equal, that printed], unprinted [listed ordinals not
    printed], nearest [the nearest call's row, or None]}, or None when the pass chose nothing in the frame."""
    p = ep["pass"]
    if not p or not p["valid"]:
        return None
    by_n = {r["n"]: r for r in ep["rows"]}
    return {"pass": p, "matched": [by_n[n] for n in p["match"] if n in by_n], "unprinted": [n for n in p["match"] if n not in by_n],
            "nearest": by_n.get(p["nearest"])}


def _kinds_phrase(kinds):
    """{3: 290, 5: 10, "other": 2} -> `k3 x290, k5 x10, kother x2`, kinds in order."""
    order = sorted(kinds, key=lambda k: (not isinstance(k, int), k if isinstance(k, int) else str(k)))
    return ", ".join("k%s x%d" % (k, kinds[k]) for k in order) or "none"


def _calls_phrase(rows):
    """Printed calls grouped by camera: `camera 0x.. kind 5 caller +0x.., +0x.. (n=9/10/11) view 0x.. tone after`, one per camera."""
    by_camera = {}
    for r in rows:
        by_camera.setdefault((r["camera"], r["kind"]), []).append(r)
    parts = []
    for (ptr, kind), ms in sorted(by_camera.items(), key=lambda kv: (kv[1][0]["n"], kv[0][0])):
        parts.append("camera 0x%X (kind %s) caller %s, %d call(s) n=%s, view %s, tone %s"
                     % (ptr, "-" if kind is None else kind, ", ".join(sorted({"+0x%X" % m["caller"] if m["caller"] is not None else "-" for m in ms})),
                        len(ms), "/".join(str(m["n"]) for m in ms),
                        ", ".join(sorted({"0x%X" % m["view"] if m["view"] else "-" for m in ms})), "/".join(sorted({str(m["tone"]) for m in ms}))))
    return "; ".join(parts)


def _depth_label(j):
    return "screen" if j["depth"] == "screen" else ("eye %d" % j["eye"] if j["eye"] is not None else "eye ?")


def _trigger_text(trigger):
    """(kind, from, to) -> `key-on`, `foot no>yes`, `naming unnamed>named`, `gui 0>6`."""
    if not trigger:
        return "?"
    kind, old, new = trigger
    return kind if old is None else "%s %s>%s" % (kind, old, new)


def print_census_episodes(c):
    """The episodes section of --camera-census, then the naming runs (H2) and the detour's CPU (D). Prints nothing for a log that has none of their lines (an
    older census), so a legacy report is byte-for-byte what it was."""
    eps, counters = c["episodes"], c["episode_counters"]
    if not (eps or counters or c["runs"] or c["detour"]):
        return
    last = counters[-1] if counters else None
    head = "%d printed" % len(eps)
    if last:
        head += "; the last `episodes` line: %d taken of %s, %d trigger(s), %d skipped" % (last["taken"], last["of"] if last["of"] is not None else 10,
                                                                                        last["triggers"], last["skipped"])
    print("\n== episodes (%s) ==" % head)
    if not counters:
        print("no `vr camera census: episodes` line (the window's counters): a census that predates the episodes, or a log that ends before its first 5 s window")
    elif last["taken"] > len(eps):
        print("%d episode(s) were armed and not printed: the log ended, or the key went off, before the frame they were to sample%s"
              % (last["taken"] - len(eps), "; one is armed now (trigger %s, armed at frame %s, samples frame %s)" % (
                  _trigger_text(last["trigger"]), last["armed"], last["sample"]) if last["state"] != "idle" and last["trigger"] else ""))
    if last and last["skipped"]:
        print("%d trigger(s) arrived while an episode was armed (or after the session's ten): counted, never sampled" % last["skipped"])
    if not eps:
        print("none: no episode was sampled (the census was on for under 31 frames, or no frame of the session reached an armed trigger's sampled frame)")
    for ep in eps:
        print("episode %d/%s: frame %d, trigger %s (armed at frame %s); journal foot=%s, GuiFocus %s, naming %s, %s"
              % (ep["n"], ep["of"] if ep["of"] is not None else "?", ep["frame"], _trigger_text(ep["trigger"]), ep["armed"], ep["foot"],
                 ep["gui"] if ep["gui"] is not None else "unknown", "NAMED (a draw named the screen's source)" if ep["named"] else "unnamed",
                 _phase_text(ep["phase_state"], ep["phase"])))
        print("    %d call(s), %d recorded%s, %d printed; by kind: %s; by caller: %s%s"
              % (ep["calls"], ep["recorded"], " (%d past the buffer: counted, not recorded)" % (ep["calls"] - ep["recorded"])
                 if ep["calls"] > ep["recorded"] else "", ep["printed"], _kinds_phrase(ep["kinds"]),
                 ", ".join("+0x%X x%d" % (rva, n) for rva, n in sorted(ep["callers"], key=lambda t: (-t[1], t[0]))) or "none",
                 " and %d more caller(s)" % ep["callers_more"] if ep["callers_more"] else ""))
        if ep["printed"] < ep["recorded"]:
            print("    the call lines are capped at 120 an episode (%d of %d recorded calls printed): the calls that matched a join or the pass's rows first, then every kind-5 call, "
                  "then the first call of each run, then the rest in order; the counts above are of all %d calls" % (ep["printed"], ep["recorded"], ep["calls"]))
        if len(ep["rows"]) != ep["printed"]:
            print("    !! %d call line(s) of the %d the header says printed are in this log (the log's own line budget, or lines cut short)"
                  % (len(ep["rows"]), ep["printed"]))
        runs = census_runs(ep["rows"])
        half = CENSUS_EPISODE_RUNS_SHOWN // 2
        for i, (rkind, caller, tone, count, first, last_n) in enumerate(runs):
            if len(runs) > CENSUS_EPISODE_RUNS_SHOWN and half <= i < len(runs) - half:
                if i == half:
                    print("    ... %d more run(s) of the printed calls ..." % (len(runs) - 2 * half))
                continue
            cams = sorted({r["camera"] for r in ep["rows"] if first <= r["n"] <= last_n and (r["kind"], r["caller"], r["tone"]) == (rkind, caller, tone)})
            print("    k%s %s %-6s x%-3d (calls %d..%d) camera %s"
                  % ("-" if rkind is None else rkind, "+0x%X" % caller if caller is not None else "-", tone, count, first, last_n,
                     ", ".join("0x%X" % p for p in cams)))
        joined = census_episode_join(ep)
        jd = ep["join_draws"]
        print("    join: %d signature(s)%s%s (a signature is a depth, a vertex shader and a pixel shader; the first of each depth has its rows read back)"
              % (len(joined), "; %d more did not fit the table (%s draw(s))" % (ep["join_more"]["signatures"], ep["join_more"]["draws"]) if ep["join_more"] else "",
                 "; the per-draw hook was handed %d draw(s), %d of them into a screen- or eye-sized depth, %d depth view(s) resolved" % (jd["seen"], jd["relevant"], jd["views"])
                 if jd else "; no `join-draws` line: the hook's draw counts are not in this log"))
        if jd and not jd["seen"]:
            print("        !! the join's per-draw hook was handed NO draw in the sampled frame: the route's per-draw path never reached it (the census was on, but no draw "
                  "watch ran), so nothing could be joined; this is not 'no draw into a screen- or eye-sized depth'")
        elif not joined:
            print("        none: %s" % ("the hook saw %d draw(s) and none went into a depth of the 2D screen's size or an eye's size" % jd["seen"] if jd
                                        else "no draw of the sampled frame went into a depth of the 2D screen's size or an eye's size (no draw counts to say how many were seen)"))
        for entry in joined:
            j = entry["join"]
            print("        %s %dx%d: first draw %s (%s draw(s)), vs 0x%X ps 0x%X, depth write %s, b1 %s%s"
                  % (_depth_label(j), j["w"], j["h"], "-" if j["draw"] is None else j["draw"], "-" if j["draws"] is None else j["draws"],
                     j["vs"] or 0, j["ps"] or 0, {True: "yes", False: "no", None: "unknown"}[j["dw"]],
                     "0x%X (%s bytes, first constant %s)" % (j["b1"], j["bytes"], j["first"]) if j["b1"] else "not bound",
                     "" if entry["read"] else "; the rows were read but their `join-rows` line is not in this log" if j["read"]
                     else "; rows not read (%s)" % (j["why"] or "?")))
            if not entry["read"]:
                continue
            if entry["printed"]:
                print("            rows 270..273 equal the composed rows of: %s" % _calls_phrase(entry["printed"]))
            elif entry["listed"] or entry["more"]:
                print("            rows 270..273 equal the composed rows of call(s) n=%s, none of which is among the printed calls" % ",".join(str(n) for n in entry["listed"]))
            else:
                print("            rows 270..273 equal the composed rows of NO call of this frame: whatever composed them is not at the refresh (or came from another frame)")
            if entry["unprinted"]:
                print("            the DLL also lists call(s) n=%s%s, not among the printed lines" % (",".join(str(n) for n in entry["unprinted"]),
                                                                                                       " and %d more" % entry["more"] if entry["more"] else ""))
            if entry["disagree"]:
                print("            !! the DLL's match and this reader's disagree on call(s) n=%s" % ",".join(str(n) for n in entry["disagree"]))
        analysis = census_episode_pass(ep)
        if ep["pass"] is None:
            print("    the pass's chosen rows: no `pass-rows` line for this episode")
        elif analysis is None:
            print("    the pass's chosen rows: none (valid=0: the pass chose no camera rows this frame: it is off, or nothing was treated)")
        else:
            p = analysis["pass"]
            relation = "as they are" if p["how"] == "identity" else "transposed" if p["how"] == "transpose" else "-"
            if p["match"] or p["match_more"]:
                print("    the pass's chosen rows (bound block %s) equal the view axes (%s) of: %s%s"
                      % ({True: "yes", False: "no", None: "?"}[p["bound"]], relation, _calls_phrase(analysis["matched"]) or "call(s) n=%s, not printed" % ",".join(str(n) for n in p["match"]),
                         " (+%d more call(s))" % p["match_more"] if p["match_more"] else ""))
                kinds = {r["kind"] for r in analysis["matched"]}
                if kinds and kinds != {5} and 5 in ep["kinds"]:
                    print("    -> the chooser STRAYS: the rows equal a kind %s camera's axes, not an eye camera's (this frame has %d kind-5 call(s))"
                          % ("/".join(sorted(str(k) for k in kinds)), ep["kinds"][5]))
                elif kinds == {5}:
                    print("    -> the rows are an eye camera's (kind 5)")
            else:
                print("    the pass's chosen rows (bound block %s) equal the view axes of NO recorded call; the nearest is call n=%s at a distance of %.3e%s%s"
                      % ({True: "yes", False: "no", None: "?"}[p["bound"]], p["nearest"] if p["nearest"] is not None else "-", p["diff"],
                         " (%s, read %s)" % (census_episode_call_label(analysis["nearest"]), relation) if analysis["nearest"] else "",
                         ": the rows are not a refresh call's axes (or not in the convention compared)" if p["nearest"] is not None else ""))
    if eps:
        print("\n== reading the episodes (facts for H1, H2 and H3, no verdict) ==")
        foot_yes = [e for e in eps if e["foot"] == "yes"]
        unnamed = [e for e in foot_yes if not e["named"]]
        print("H1 (on-foot maps and menus never name a source): %d on-foot episode(s) (journal foot=yes), %d of them unnamed (no draw named the 2D screen's source)%s"
              % (len(foot_yes), len(unnamed), ": episode(s) %s" % ", ".join(str(e["n"]) for e in unnamed) if unnamed else ""))
        for e in unnamed:
            screens = [en for en in census_episode_join(e) if en["join"]["depth"] == "screen"]
            read = [en for en in screens if en["read"]]
            if not screens:
                what = "no draw went into a screen-sized depth"
            elif not read:
                what = "%d signature(s) drew into a screen-sized depth, their rows not read" % len(screens)
            else:
                kinds = sorted({str(r["kind"]) for en in read for r in en["printed"]})
                what = "%d signature(s) drew into a screen-sized depth; the first draw's rows %s" % (
                    len(screens), "equal the composed rows of kind %s call(s)" % "/".join(kinds) if kinds else "equal no printed call's composed rows")
            print("    episode %d: %s" % (e["n"], what))
        aboard = [e for e in eps if e["foot"] in ("no", "off")]
        print("H3 (the cockpit's maps are driven by kind-5 eye cameras' rows): %d aboard episode(s) (journal foot=no or off)" % len(aboard))
        for e in aboard:
            parts = []
            for en in census_episode_join(e):
                if en["join"]["depth"] == "eye" and en["read"]:
                    kinds = sorted({str(r["kind"]) for r in en["printed"]})
                    parts.append("%s depth rows equal %s" % (_depth_label(en["join"]), "kind %s call(s)" % "/".join(kinds) if kinds else "no printed call's rows"))
            analysis = census_episode_pass(e)
            if analysis:
                kinds = sorted({str(r["kind"]) for r in analysis["matched"]})
                parts.append("the pass's rows equal %s" % ("kind %s call(s)' axes" % "/".join(kinds) if kinds else "no recorded call's axes (nearest n=%s)" % analysis["pass"]["nearest"]))
            print("    episode %d (%s, GuiFocus %s): %s" % (e["n"], _trigger_text(e["trigger"]), e["gui"] if e["gui"] is not None else "unknown",
                                                            "; ".join(parts) or "no eye-depth rows read and no pass rows"))
    print_census_runs(c)
    print_census_detour(c)


def census_runs_total(c):
    """The runs lines summed: {named {bin: n}, unnamed {bin: n}, frames, longest_named, longest_unnamed, windows, lines}."""
    total = {"named": dict.fromkeys(CENSUS_RUN_BINS, 0), "unnamed": dict.fromkeys(CENSUS_RUN_BINS, 0), "frames": 0, "longest_named": 0,
             "longest_unnamed": 0, "windows": 0, "lines": len(c["runs"])}
    for r in c["runs"]:
        for way in ("named", "unnamed"):
            for b in CENSUS_RUN_BINS:
                total[way][b] += r[way][b]
        total["frames"] += r["frames"]
        total["longest_named"] = max(total["longest_named"], r["longest_named"])
        total["longest_unnamed"] = max(total["longest_unnamed"], r["longest_unnamed"])
        total["windows"] += r["windows"] or 0
    return total


def print_census_runs(c):
    if not c["runs"]:
        return
    t = census_runs_total(c)
    print("\n== on-foot naming runs (the `runs` lines summed over %d 5 s window(s); frames the journal says on foot: %d) ==" % (t["windows"], t["frames"]))
    print("a run is consecutive on-foot frames that all named the 2D screen's source (named) or none did (unnamed), counted when it ends")
    print("%-9s %s" % ("run of", "  ".join("%6s" % b for b in CENSUS_RUN_BINS)))
    for way in ("named", "unnamed"):
        print("%-9s %s" % (way, "  ".join("%6d" % t[way][b] for b in CENSUS_RUN_BINS)))
    long_unnamed = sum(t["unnamed"][b] for b in CENSUS_RUN_BINS[2:])
    print("longest named run %d frame(s), longest unnamed run %d frame(s)" % (t["longest_named"], t["longest_unnamed"]))
    print("H2 (the longest unnamed run in an on-foot world stays under 3 frames): %d unnamed run(s) of 3 frames or more, and %d of 1 or 2; a map or a menu opened on foot is "
          "one of the long ones, so this reads the world only over a stretch with none open" % (long_unnamed, t["unnamed"]["1"] + t["unnamed"]["2"]))
    last = c["runs"][-1]
    if last["open"]:
        print("the run open at the last window: %s for %d frame(s) so far" % last["open"])


def print_census_detour(c):
    if not c["detour"]:
        return
    lines = c["detour"]
    frames = sum(d["frames"] or 0 for d in lines)
    calls = sum(d["calls"] or 0 for d in lines)
    sampled = sum(d["sampled"] or 0 for d in lines)
    every = lines[0]["every"]
    print("\n== the detour's CPU (the observer's two halves, %s call in %s timed; %d `detour` line(s) over %d 5 s window(s)) =="
          % ("1" if every else "?", every if every else "?", len(lines), sum(d["windows"] or 0 for d in lines)))
    print("what is timed: %s -- the census's work inside the detour (the clock is read at the start and end of each half). The detour's own prologue, the injection's "
          "writes and the game's body are not in it." % (lines[0]["timed"] or "?"))
    print("%d frame(s), %d refresh call(s), %d timed" % (frames, calls, sampled))
    est = [(d["est_ms"], d["frames"] or 0) for d in lines if d["est_ms"] is not None]
    if est:
        weight = sum(f for _, f in est)
        mean = sum(e * f for e, f in est) / float(weight) if weight else sum(e for e, _ in est) / float(len(est))
        print("estimated ms a frame: mean %.3f over %d window line(s) (lowest %.3f, highest %.3f); calls a frame: %.1f"
              % (mean, len(est), min(e for e, _ in est), max(e for e, _ in est), calls / float(frames) if frames else 0.0))
    else:
        print("estimated ms a frame: none (no window had both calls and timed calls)")
    for name, label in (("obs", "observed calls (the detour did not inject for them)"), ("inj", "injected calls (the route's phase was written for them)")):
        n_calls = sum(d["modes"][name]["calls"] or 0 for d in lines)
        n_sampled = sum(d["modes"][name]["sampled"] or 0 for d in lines)
        parts = []
        for half in ("pre", "post"):
            weighted = [(d["modes"][name][half], d["modes"][name]["sampled"] or 0) for d in lines if d["modes"][name][half]]
            weight = sum(w for _, w in weighted)
            if weight:
                parts.append("%s half: mean %.3g us, longest %.3g us" % (half, sum(p[0] * w for p, w in weighted) / float(weight), max(p[1] for p, _ in weighted)))
        print("%s: %d call(s), %d timed%s" % (label, n_calls, n_sampled, "; " + "; ".join(parts) if parts else "; nothing timed"))


def _camera_role_label(ptr, roles):
    return "EYE" if ptr in roles["eye"] else "WORLD" if ptr == roles["world"] \
        else "world-side" if ptr in roles["world_side"] else "other"


def print_camera_census(text):
    """The --camera-census report. Returns the process exit code: 0 when the log
    has census lines, 1 when it has none."""
    c = parse_camera_census(text)
    if not c["lines"]:
        print("[edvr] camera census: no `vr camera census` line in this log. The key "
              "advanced.vr_camera_census was off, this is not a VR-profile log, or the "
              "build predates the census.")
        return 1
    routes = parse_world_route(text)
    events = route_events(text)
    refusals = parse_refusal_windows(text)
    print("[edvr] camera census: %d census line(s): %d 5 s line(s), %d camera(s), "
          "%d call sequence(s), %d eye draw(s), %d other-thread entr%s%s"
          % (c["lines"], len(c["windows"]), len(c["order"]), len(c["sequences"]),
             len(c["eyes"]), len(c["threads"]), "y" if len(c["threads"]) == 1 else "ies",
             ", %d episode(s)" % len(c["episodes"]) if c["episodes"] else ""))
    if c["unparsed"]:
        print("[edvr]   %d census line(s) could not be parsed (cut short or garbled) and were skipped" % c["unparsed"])
    for info in c["info"]:
        print("[edvr]   note: %s" % info)

    print("\n== 5 s lines ==")
    if not c["windows"]:
        print("none: the census never printed a window (a log that ends within five "
              "seconds of the key going on, or a census that did not run)")
    for stamp, line in c["windows"]:
        print("%s vr camera census 5s: %s" % (stamp, line))
    window_kv = [_ckv(line) for _, line in c["windows"]]
    off_thread = sum(int(k.get("off-thread", "0")) for k in window_kv if k.get("off-thread", "0").isdigit())
    if off_thread:
        print("!! %d refresh call(s) ran on a thread other than the render thread "
              "(off-thread): the owner-thread assumption failed; the other-thread "
              "entries below name them" % off_thread)
    elif window_kv:
        print("off-thread calls: 0 in every window -- the refresh runs on the render thread")
    if window_kv and not any(k.get("progress") == "yes" for k in window_kv):
        print("!! progress=no in every window: the world route never reported its draw progress, so every "
              "call prints draw=- tone=none and the place against the tone draw (C) cannot be read; frames "
              "were sampled by the journal alone (foot=yes), so a call sequence and eye rows exist only if "
              "the journal said on foot")
    tone_frames = sum(int(k.get("tone-frames", "0")) for k in window_kv if k.get("tone-frames", "0").isdigit())
    sampled_frames = sum(int(k.get("on-foot-frames", "0")) for k in window_kv if k.get("on-foot-frames", "0").isdigit())
    if window_kv:
        feet = sorted({k.get("foot", "?") for k in window_kv})
        print("frames with the tone drawn: %d; sampled (tone while the journal says on foot, or no journal; while the "
              "world route jitters, with a non-zero phase): %d; the journal said: %s"
              % (tone_frames, sampled_frames, ", ".join(feet)))
    if routes:
        states = {}
        for w in routes:
            states[route_tok(w, "jitter") or "(no jitter= token)"] = states.get(route_tok(w, "jitter") or "(no jitter= token)", 0) + 1
        print("world route: %d 5 s window(s); jitter: %s" % (len(routes), ", ".join("%s x%d" % (k, states[k]) for k in sorted(states))))
    if tone_frames and not sampled_frames:
        if any(route_tok(w, "jitter") == "on" for w in routes):
            print("!! the tone was drawn in %d frame(s) but no frame was sampled: the route was jittering, and the census "
                  "samples a frame then only with a non-zero phase (or the journal never said on foot: foot=no is a ship, "
                  "foot=unknown a menu). Nothing below can be joined." % tone_frames)
        else:
            print("!! the tone was drawn in %d frame(s) but no frame was sampled: the journal never said on foot "
                  "(foot=no is a ship, foot=unknown a menu). Nothing below can be joined; disembark and fly again."
                  % tone_frames)
    for t in c["threads"]:
        print("other-thread: tid %s camera %s kind %s caller %s calls %s"
              % (t["tid"], _cg(t["camera"]), t["kind"], _cg(t["caller"]), t["calls"]))

    join = census_join(c)
    roles = census_roles(c, join)
    stats = roles["stats"]
    print("\n== cameras (%d, in first-seen order; `1st` is the kind of the camera's FIRST call ever, `calls` the kinds of its "
          "logged calls) ==" % len(c["order"]))
    if c["order"]:
        print("%-14s %-4s %-9s %-10s %-8s %-7s %-7s %-24s %-14s %-14s %-6s %-5s %-16s %s"
              % ("camera", "1st", "caller", "aspect", "near", "fov", "far", "bound",
                 "viewport", "view", "tone", "frame", "calls", "role / changes"))
    for ptr in c["order"]:
        cam = c["cameras"][ptr]
        st = stats.get(ptr)
        changes = len(c["changes"].get(ptr, []))
        label = _camera_role_label(ptr, roles) if st else "not in a logged sequence"
        print("0x%-12X %-4s %-9s %-10s %-8s %-7s %-7s %-24s %-14s %-14s %-6s %-5s %-16s %s%s"
              % (ptr, cam["kind"], "+0x%X" % cam["caller"] if cam["caller"] is not None else "-",
                 _cg(cam["aspect"]), _cg(cam["near"]), _cg(cam["fov"]), _cg(cam["far"]),
                 _cg(cam["bound"]), _cg(cam["viewport"]),
                 "0x%X" % cam["view"] if cam["view"] else "-", cam["tone"], cam["frame"],
                 "/".join("k%s x%d" % (k, st["kinds"][k]) for k in sorted(st["kinds"], key=lambda k: (k is None, k or 0))) if st else "-",
                 label, "; %d 'changed:' line(s)" % changes if changes else ""))
    for ptr in c["order"]:
        for ch in c["changes"].get(ptr, [])[:2]:
            print("    0x%X frame %s moved: %s" % (ptr, ch["frame"], ", ".join(
                "%s %s" % (k, v) for k, v in sorted(ch["fields"].items()))))

    # ---- what each camera is, from the kind of each of its calls ----
    by_camera = {}
    for seq in c["sequences"]:
        for r in seq["rows"]:
            by_camera.setdefault(r["camera"], []).append(r)
    print("\n== roles from the calls (the KIND of each logged call decides; the camera object's first-seen kind does not) ==")
    if not stats:
        print("none: no call sequence was logged, so no camera can be labelled by its calls")
    shown = [p for p in c["order"] if p in stats] + sorted(p for p in stats if p not in c["cameras"])
    for ptr in shown:
        st = stats[ptr]
        nframes = len(st["frames"])
        first = c["cameras"].get(ptr, {}).get("kind")
        kinds = st["kinds"]
        line = "camera 0x%X %s: %s over %d logged frame(s) (%.1f a frame)" % (
            ptr, _camera_role_label(ptr, roles), _ckinds_text(kinds), nframes, st["calls"] / float(max(nframes, 1)))
        k3 = kinds.get(3, 0)
        if k3:
            line += "; kind-3 calls before the tone %d, after %d%s" % (
                st["before"], st["after"], ", no tone info %d" % st["none"] if st["none"] else "")
        if first is not None and first not in kinds:
            line += "; first seen as kind %s" % first
        if len([k for k in kinds if k is not None]) > 1:
            per = ", ".join("frame %d: %s" % (f, _ckinds_text(st["frames"][f])) for f in sorted(st["frames"]))
            line += "; SEVERAL KINDS (%s)" % per
        print(line)
        if k3:
            groups = census_projections(by_camera[ptr])
            for i, g in enumerate(groups[:CENSUS_PROJ_SHOWN]):
                nf = float(max(len(g["frames"]), 1))
                ratio = ""
                if i and groups[0]["ys"] > 0:
                    tighter = g["ys"] / groups[0]["ys"]
                    ratio = ", x%.3f %s than projection 1" % (tighter if tighter >= 1 else 1 / tighter,
                                                              "tighter" if tighter >= 1 else "wider")
                roles_seen = ", ".join("%s x%d" % (k, g["roles"][k]) for k in sorted(g["roles"]))
                note = ""
                if i and g["near"] > groups[0]["near"] and g["ys"] > groups[0]["ys"]:
                    note = " -- the first-person weapon camera's signature: the same object, a tighter field of view and a larger near plane"
                print("    projection %d: %.1f call(s) a frame, scale %.4f x %.4f (aspect %.4f), near %.6g%s, off-centre (%.2e, %.2e)%s%s%s"
                      % (i + 1, g["count"] / nf, g["xs"], g["ys"], g["ys"] / g["xs"], g["near"], ratio, g["shift"][0], g["shift"][1],
                         "; role= %s" % roles_seen if roles_seen else "",
                         "; injected %d of %d" % (g["injected"], g["count"]) if any(r["inj"] is not None for r in by_camera[ptr]) else "",
                         note))
            if len(groups) > CENSUS_PROJ_SHOWN:
                print("    ... %d more projection(s) ..." % (len(groups) - CENSUS_PROJ_SHOWN))

    print("\n== call sequences (%d frame(s), calls reduced to runs of kind/caller/tone) ==" % len(c["sequences"]))
    if not c["sequences"]:
        print("none: no on-foot frame (the tone was never seen) while a sequence was still wanted")
    for seq in c["sequences"]:
        print("frame %d (sequence %s, journal foot=%s, %s): %d call(s), %d recorded, %d truncated"
              % (seq["frame"], seq["index"], seq["foot"] or "?", _phase_text(seq["phase_state"], seq["phase"]),
                 seq["calls"], seq["recorded"], seq["truncated"]))
        # Per camera first: how many calls, from which callers, on which side of the tone, over which draws.
        digest = {}
        for r in seq["rows"]:
            d = digest.setdefault(r["camera"], {"kinds": {}, "n": 0, "callers": {}, "tone": {}, "draws": [], "views": set(),
                                                "inj": 0, "roles": {}})
            d["n"] += 1
            d["kinds"][r["kind"]] = d["kinds"].get(r["kind"], 0) + 1
            d["views"].add(r["view"])
            d["callers"][r["caller"]] = d["callers"].get(r["caller"], 0) + 1
            d["tone"][r["tone"]] = d["tone"].get(r["tone"], 0) + 1
            if r["inj"] == 1:
                d["inj"] += 1
            if r["role"] in ("scene", "fp", "aux"):
                d["roles"][r["role"]] = d["roles"].get(r["role"], 0) + 1
            if r["draw"] is not None:
                d["draws"].append(r["draw"])
        for ptr, d in sorted(digest.items(), key=lambda kv: min(r["n"] for r in seq["rows"] if r["camera"] == kv[0])):
            print("    camera 0x%X %s: %d call(s), callers %s, view %s, tone %s, draws %s%s"
                  % (ptr, "+".join("k%s" % ("-" if k is None else k) for k in sorted(d["kinds"], key=lambda k: (k is None, k or 0))),
                     d["n"],
                     ", ".join("+0x%X x%d" % (k, v) if k is not None else "- x%d" % v
                               for k, v in sorted(d["callers"].items(), key=lambda kv: (kv[0] is None, kv[0] or 0))),
                     ", ".join("0x%X" % v if v else "-" for v in sorted(d["views"], key=lambda v: v or 0)),
                     _cfmt_tone(d["tone"]),
                     "%d..%d" % (min(d["draws"]), max(d["draws"])) if d["draws"] else "-",
                     ("; injected %d%s" % (d["inj"], " (%s)" % ", ".join("%s %d" % (k, d["roles"][k]) for k in sorted(d["roles"]))
                                          if d["roles"] else "")) if d["inj"] or d["roles"] else ""))
        runs = census_runs(seq["rows"])
        half = CENSUS_RUNS_SHOWN // 2
        for i, (kind, caller, tone, count, first, last) in enumerate(runs):
            if len(runs) > CENSUS_RUNS_SHOWN and half <= i < len(runs) - half:
                if i == half:
                    print("    ... %d more run(s) ..." % (len(runs) - 2 * half))
                continue
            cams = sorted({r["camera"] for r in seq["rows"]
                           if first <= r["n"] <= last and (r["kind"], r["caller"], r["tone"]) == (kind, caller, tone)})
            print("    k%s %s %-6s x%-3d (calls %d..%d) camera %s"
                  % ("-" if kind is None else kind, "+0x%X" % caller if caller is not None else "-",
                     tone, count, first, last, ", ".join("0x%X" % p for p in cams)))

    print("\n== eye draws (b1 rows 270..273 read back from the GPU) ==")
    if not c["eyes"]:
        print("none: no eye composite draw in an on-foot frame reached the readback")
    for j in join:
        e = j["eye"]
        print("eye %d frame %d draw %s (journal foot=%s, %s) b1 0x%X first=%s bytes=%s" %
              (e["eye"], e["frame"], e["draw"] if e["draw"] is not None else "-", e["foot"] or "?",
               _phase_text(e["phase_state"], e["phase"]), e["b1"] or 0, e["first"], e["bytes"]))
        if e["rows"]:
            print("    rows   %s" % ", ".join("%.7g" % v for v in e["rows"]))
            print("    meas   %s (flatCameraMeasureRowShift: the off-centre terms of the rows)" % _cg(e["meas"]))
        else:
            print("    rows   unavailable (%s)" % (e["why"] or "?"))
        g = c["geometry"].get((e["eye"], e["frame"]))
        if g and g["known"]:
            print("    EDVR advertised seq %s frustum %s shift %s; expected measure %s, shifted %s; leak %s"
                  % (g["seq"], _cg(tuple(g["frustum"]) if g["frustum"] else None), _cg(g["shift"]),
                     _cg(g["expect"]), _cg(g["expect_shifted"]), _cg(g["leak"])))
            fit = census_shift_fit(e["meas"], g["frustum"], g["shift"]) if g["frustum"] and g["shift"] else None
            if fit and fit[3] is not None and fit[3] - fit[1] < CENSUS_FIT_TIE:
                # No winner to name: sorting a tie would pick a label by its spelling.
                print("    the rows cannot tell which way the shift is carried: every candidate leaves about %.2e NDC "
                      "(the advertised shift, %s, is too small to separate them from rounding)"
                      % (fit[1], _cg(g["shift"])))
            elif fit:
                print("    the rows measure as %s (residual %.2e NDC; next best: %s, %.2e)"
                      % (fit[0], fit[1], fit[2], fit[3]))
        else:
            print("    EDVR advertised geometry: unavailable")
    leaks = [max(abs(v) for v in g["leak"]) for g in c["geometry"].values()
             if g["known"] and g["leak"] and all(v == v for v in g["leak"])]
    if leaks:
        print("leak measure over %d eye draw(s): largest |leak| = %.3e NDC (with no world phase injected this is the "
              "baseline a leak detector must clear; with one, the stage 2 verdict below judges it for each eye draw whose "
              "frame has a non-zero phase; a phase of half a pixel at 5040 wide is 2e-4; `leak` assumes the game builds "
              "its eye camera from the frustum moved by +shift, which the fit line above checks: `the rows measure as`, "
              "or `cannot tell` when the shift is too small)"
              % (len(leaks), max(leaks)))

    print("\n== the offline join (B): eye rows against the rows of every logged call, tolerance %g ==" % CENSUS_JOIN_TOL)
    if not join:
        print("nothing to join: no eye draw was read back")
    for j in join:
        e = j["eye"]
        label = "eye %d frame %d draw %s" % (e["eye"], e["frame"], e["draw"] if e["draw"] is not None else "-")
        if j["scope"] == "no-rows":
            print("%s: no rows were read (%s)" % (label, e["why"] or "?"))
        elif j["scope"] == "no-sequence":
            print("%s: frame %d's call sequence was not logged (only the first %d on-foot frames are), so its rows "
                  "cannot be joined" % (label, e["frame"], 3))
        elif j["scope"] == "none":
            print("%s: NO logged call of frame %d produced these rows. The eye's camera does not reach the refresh "
                  "with them: either it is composed by another of the composer's callers, or by a path the detour "
                  "does not see. (B) does not identify it." % (label, e["frame"]))
        else:
            cams = {}
            for m in j["matches"]:
                cams.setdefault(m["camera"], []).append(m)
            for ptr, ms in sorted(cams.items()):
                print("%s: camera 0x%X (kind %s) caller %s, %d call(s) n=%s, draw %s, tone %s%s"
                      % (label, ptr, "/".join(sorted({str(m["kind"]) for m in ms})),
                         ", ".join(sorted({"+0x%X" % m["caller"] for m in ms})),
                         len(ms), "/".join(str(m["n"]) for m in ms),
                         "/".join(str(m["draw"]) for m in ms if m["draw"] is not None) or "-",
                         "/".join(sorted({m["tone"] for m in ms})),
                         "" if j["scope"] == "frame" else " (another frame's call: the same pose)"))
            if len(cams) > 1:
                print("    %d different cameras composed identical rows for this draw" % len(cams))
    # The field signature the eye rows belong to: each joined camera's first-sight line, once.
    for ptr in [p for p in c["order"] if p in roles["eye"]]:
        cam = c["cameras"][ptr]
        print("eye camera 0x%X signature: kind %s, caller %s, aspect %s, near %s, far %s, fov %s, bound %s, offcentre %s, "
              "viewport %s, tangents %s, view %s"
              % (ptr, cam["kind"], "+0x%X" % cam["caller"] if cam["caller"] is not None else "-", _cg(cam["aspect"]),
                 _cg(cam["near"]), _cg(cam["far"]), _cg(cam["fov"]), _cg(cam["bound"]), _cg(cam["offcentre"]),
                 _cg(cam["viewport"]), _cg(cam["tan"]), "0x%X" % cam["view"] if cam["view"] else "-"))

    print("\n== which signal separates the eye cameras (A)-(F) ==")
    if not roles["eye"]:
        candidates = sorted(p for p, s in stats.items() if s["kinds"].get(5))
        print("no camera was joined to an eye draw, so no eye camera is known and nothing can be separated. "
              "%s%s" % ("(The join found no matching call in a logged frame: see above.)" if join else "(No eye draw was read back.)",
                        " Cameras with kind-5 calls in the logged sequences (candidates, not joined): %s"
                        % ", ".join("0x%X" % p for p in candidates) if candidates else ""))
        print_census_episodes(c)
        print_stage2_verdict(c, routes, roles, events, refusals)
        return 0
    print("eye camera(s): %s; world camera: %s; other world-side kind-3 camera(s): %s"
          % (", ".join("0x%X" % p for p in c["order"] if p in roles["eye"]),
             "0x%X" % roles["world"] if roles["world"] else "none found",
             ", ".join("0x%X" % p for p in c["order"] if p in roles["world_side"] and p != roles["world"]) or "none"))
    if roles["world"]:
        st = stats[roles["world"]]
        nframes = max(len(st["frames"]), 1)
        print("world camera 0x%X: %d kind-3 call(s) over %d logged frame(s) = %.1f a frame, %d of them before the tone "
              "(%d projection(s); its first-seen kind was %s)"
              % (roles["world"], st["kinds"].get(3, 0), nframes, st["kinds"].get(3, 0) / float(nframes), st["before"],
                 len(census_projections(by_camera[roles["world"]])),
                 c["cameras"].get(roles["world"], {}).get("kind", "unknown")))
    separating = census_separation(c, roles)
    if separating:
        print("(A) call signature (kind, and what a call's composed rows say: aspect, fov, near, off-centre): %s separate every "
              "eye camera from EVERY world-side kind-3 camera:" % ", ".join(f for f, _, _ in separating))
        for field, ev, ov in separating:
            print("      %-9s eye %s  vs  world-side %s" % (field, ", ".join(_cg(v) for v in ev[:4]), ", ".join(_cg(v) for v in ov[:4])))
    else:
        print("(A) call signature: no single field (kind, aspect, fov, near, off-centre) separates the eye cameras' calls from the "
              "world-side kind-3 calls")
    print("(B) content join: %d of %d eye draw(s) joined to a camera; eye camera(s) %s"
          % (sum(1 for j in join if j["matches"]), len(join),
             ", ".join("0x%X" % p for p in c["order"] if p in roles["eye"])))
    order = census_order(c, roles)
    decided = [o for o in order if o["by_position"] is not None]
    ok = [o for o in decided if o["by_position"]]
    if decided:
        print("(C) place in the frame: every refresh of a world-side camera precedes every refresh of an eye camera "
              "in %d of %d logged sequence(s) (the world side being the kind-3 calls)" % (len(ok), len(decided)))
        for o in decided:
            print("      frame %d: world-side %d call(s) [tone %s; last draw %s], eye %d call(s) [tone %s; first draw %s]%s"
                  % (o["frame"], o["world"], _cfmt_tone(o["world_tone"]),
                     o["world_last_draw"] if o["world_last_draw"] is not None else "-", o["eye"],
                     _cfmt_tone(o["eye_tone"]), o["eye_first_draw"] if o["eye_first_draw"] is not None else "-",
                     "" if o["by_position"] else "  <- INTERLEAVED"))
        unknown = any("none" in o["world_tone"] or "none" in o["eye_tone"] for o in decided)
        tone_clean = all(set(o["world_tone"]) <= {"before"} and set(o["eye_tone"]) <= {"after"} for o in decided)
        print("      the tone flag %s" % (
            "is unavailable (tone=none: the route reported no draw progress for these calls)" if unknown
            else "separates the two sets" if tone_clean else "does NOT separate the two sets"))
    else:
        print("(C) place in the frame: no logged sequence holds both a world-side and an eye camera's call")
    eye_callers, world_callers = set(), set()
    for seq in c["sequences"]:
        for r in seq["rows"]:
            if r["camera"] in roles["eye"]:
                eye_callers.add(r["caller"])
            elif r["camera"] == roles["world"] and r["kind"] == 3:
                world_callers.add(r["caller"])
    fmt = lambda s: ", ".join("+0x%X" % x for x in sorted(v for v in s if v is not None)) or "none"
    shared = eye_callers & world_callers
    print("(D) caller: eye camera(s) call from %s; the world camera from %s; %s"
          % (fmt(eye_callers), fmt(world_callers),
             ("no caller is shared: the caller separates them" if eye_callers and world_callers and not shared
              else "shared: %s -- the caller alone does not separate them" % fmt(shared) if shared
              else "not enough calls logged to say")))
    eye_views, world_views = set(), set()
    for seq in c["sequences"]:
        for r in seq["rows"]:
            if r["camera"] in roles["eye"]:
                eye_views.add(r["view"])
            elif r["camera"] == roles["world"] and r["kind"] == 3:
                world_views.add(r["view"])
    vfmt = lambda s: ", ".join("0x%X" % v for v in sorted(x for x in s if x)) or "none"
    shared_views = (eye_views & world_views) - {None, 0}
    print("(F) view (the refresh's second argument, the pass object): eye camera(s) are refreshed with %s; the world "
          "camera with %s; %s"
          % (vfmt(eye_views), vfmt(world_views),
             ("no view is shared: the view separates them" if eye_views - {None, 0} and world_views - {None, 0} and not shared_views
              else "shared: %s -- the view alone does not separate them" % vfmt(shared_views) if shared_views
              else "not enough calls logged to say")))
    worst, compared = 0.0, 0
    for j in join:
        e = j["eye"]
        g = c["geometry"].get((e["eye"], e["frame"]))
        if not g or not g["known"] or not g["frustum"] or not j["matches"]:
            continue
        cam = c["cameras"].get(j["matches"][0]["camera"])
        if not cam or not cam["tan"] or len(cam["tan"]) != 4 or cam["frame"] != e["frame"]:
            continue
        diff = max(abs(a - b) for a, b in zip(cam["tan"], g["frustum"]))
        diff_flip = max(abs(a - b) for a, b in zip(cam["tan"], (g["frustum"][0], g["frustum"][1], -g["frustum"][3], -g["frustum"][2])))
        worst = max(worst, min(diff, diff_flip))
        compared += 1
    if compared:
        print("(E) tangents: the eye camera's tangents match the frustum EDVR advertised for its eye to within %.2e "
              "(compared on %d draw(s) in the camera's first frame)" % (worst, compared))
    elif all(not (c["cameras"].get(p) or {}).get("tan") for p in roles["eye"]):
        print("(E) tangents: not compared: the eye cameras' lines carry no tangents (tan=-: a kind-5 camera is the game's custom "
              "matrix, not a frustum); the eye-geometry leak= above is the comparison with what EDVR advertised")
    else:
        print("(E) tangents: no eye draw fell in the frame an eye camera's line was printed, so the camera's tangents "
              "and the advertised frustum were not compared (their difference is the eye shift, about 1e-4, per frame)")
    print_census_episodes(c)
    print_stage2_verdict(c, routes, roles, events, refusals)
    return 0


# --maps-sharp: the on-foot maps gate (experimental.on_foot_maps_sharp, docs/design-world-camera-motion-2026-09-30.md, Phase 1).
# The lines it reads are written by src/d3d11/ui_maps_math.h's formatters (their wording is the anchors below); the rig
# tools/on_foot_maps_test compares tools/maps_sharp_fixture.log to those formatters byte for byte, and this reader's self-test
# parses the same file, so a formatter that drifts fails in the build rather than in the ten minutes after a flight.
MAPS_STAMP_RE = re.compile(r"^\[(?P<ts>\d\d:\d\d:\d\d\.\d{3})\] (?P<msg>.*)$")
# The journal's reading is text the DLL substitutes into "(the journal: %s)", and one of its readings has parentheses of its own ("no
# Flags2 in Status.json (a menu, or no file yet)": every arrival, before the game writes Status.json), so the groups below take
# everything up to the fixed words after them, never up to the first ")".
MAPS_ON_RE = re.compile(r"^on foot maps sharp: ON at frame=(?P<frame>\d+) .*gate starts as .*: (?P<start>the world|not the world) "
                        r"\(the journal: (?P<journal>.*)\)\.$")
# The OFF line's reason is "(why)", and three of the DLL's reasons have parentheses of their own ("no temporal mode is on (fix.temporal_aa is
# off)", "the eye jitter is not as shipped (...)", "screen motion is not live (...)"), so it takes one level of nesting: a pattern that stopped
# at the first ")" lost the OFF line of a real flight (edvr_gfx_20261001_085519.log, fix.temporal_aa off) as a line it did not know.
MAPS_OFF_RE = re.compile(r"^on foot maps sharp: OFF at frame=(?P<frame>\d+) \((?P<why>(?:[^()]|\([^()]*\))*)\): ")
MAPS_TAKE_RE = re.compile(r"^on foot maps sharp: the layer TAKES the 2D screen at frame=(?P<frame>\d+): no world camera named its source "
                          r"for (?P<run>\d+) frames in a row \(after (?P<world>\d+) world frames, (?P<secs>[0-9.]+) s; the journal: "
                          r"(?P<journal>.*)\)\. A map or a menu is sharp from the layer")
MAPS_BACK_RE = re.compile(r"^on foot maps sharp: the layer HANDS BACK the 2D screen at frame=(?P<frame>\d+) after (?P<frames>\d+) panel "
                          r"frames \((?P<secs>[0-9.]+) s; (?P<only>\d+) eyes through the layer-only door, (?P<kept>\d+) kept the "
                          r"upscaler because the game drew something else into them\): (?P<why>.*)\.$")
MAPS_NOTEMPTY_RE = re.compile(r"^on foot maps sharp: the layer took the 2D screen for eye (?P<eye>\d) \(sequence (?P<seq>\d+)\) but the "
                              r"game drew (?P<draws>\d+) draw\(s\) into eye-sized targets this frame and the layer took (?P<taken>\d+): ")
MAPS_NOTLIVE_RE = re.compile(r"^on foot maps sharp: the maps gate is on but .*: (?P<why>[^:]*)\.$")
MAPS_WINDOW_RE = re.compile(r"^on foot maps sharp 5s: key=on (?P<secs>[0-9.]+) s mode=(?P<mode>\w+) gate=(?P<gate>world|panel) "
                            r"frames=(?P<frames>\d+) named=(?P<named>\d+) unnamed=(?P<unnamed>\d+) world-frames=(?P<world>\d+) "
                            r"panel-frames=(?P<panel>\d+) holds=(?P<holds>\d+) releases=(?P<releases>\d+) "
                            r"screen-takes=(?P<takes>\d+) recognised=(?P<recognised>\d+) door-layer-only=(?P<only>\d+) "
                            r"door-not-empty=(?P<notempty>\d+) not-live-frames=(?P<notlive>\d+)(?: screen-draws=(?P<draws>\d+))?")
# `draws` is the 2D screen composites the layer's decision SAW in the window (taken or not); a log from a build before the Phase 1 fix
# (design doc 8.10) has no such token and reads None: the reader then judges by takes alone and says it cannot tell "nothing drawn"
# from "drawn and refused".
MAPS_ROUTE_RELEASED_RE = re.compile(r"vr world route: RELEASED the world at frame=(?P<frame>\d+) \((?P<why>[^)]*)\)")
MAPS_ROUTE_OWNS_RE = re.compile(r"vr world route: OWNS the world from frame=(?P<frame>\d+) ")
# A black eye is the sharpen door's own failure line ("... got NO composite from the UI layer -- the eye is BLACK", printed only on the
# failure path). The luma probe's "first black stage is X" lines are NOT black-eye evidence: the probe prints one whenever the first
# black stage CHANGES, including "is none" the moment a black arrival gives way to a world, and "is game" for as long as the layer
# holds the screen (the game's own eye image is empty by construction). Four real flights each carry 8 to 20 of them; reading them as
# black eyes made every real flight STOP. The probe's sample lines are kept per panel period instead (MAPS_LUMA_RE), as a note.
MAPS_BLACK_NEEDLES = ("native sharpen: LAYER-ONLY eye",)
MAPS_LUMA_RE = re.compile(r"^luma probe: eye=(?P<eye>\d) game=(?P<game>\S+) dlss_out=(?P<dlss>\S+) final=(?P<final>\S+)$")
MAPS_LUMA_STAGE_RE = re.compile(r"^(?P<mean>[0-9.]+)/(?P<max>[0-9.]+)/(?P<black>\d+)%$")
MAPS_SHORT_PANEL_FRAMES = 10     # a panel period shorter than this (0.11 s at 90 Hz) is a flap in the world, not a map or a menu a person opened
MAPS_DOOR_SHARE = 0.9            # eyes through the layer-only door, of the 2 x panel-frames a panel period should have had
MAPS_SAME_BOUNDARY_S = 0.05      # the route lets go on the gate's boundary: its RELEASED line is within this of the TAKES line


def _clock_s(ts):
    h, m, rest = ts.split(":")
    return int(h) * 3600 + int(m) * 60 + float(rest)


def parse_maps_sharp(text):
    """The feature's lines, in log order: {events: [{kind, ts, t, ...}], windows: [{ts, t, ...ints}], route: [{kind, ts, t, ...}],
    black: [{ts, t, line}], declined: [...], luma: [{ts, t, eye, final_black, final_mean}]}. A line that starts "on foot maps sharp"
    but is none of the kinds goes in `unparsed`. `luma` holds the probe's sample lines (the final stage's black share), which
    maps_sharp_episodes lays against the panel periods."""
    out = {"events": [], "windows": [], "route": [], "black": [], "declined": [], "unparsed": [], "luma": []}
    for raw in text.splitlines():
        m = MAPS_STAMP_RE.match(raw.rstrip("\r"))
        if not m:
            continue
        ts, msg = m.group("ts"), m.group("msg")
        t = _clock_s(ts)
        if msg.startswith("on foot maps sharp 5s:"):
            w = MAPS_WINDOW_RE.match(msg)
            if not w:
                out["unparsed"].append(raw)
                continue
            d = {k: int(v) for k, v in w.groupdict().items() if k not in ("secs", "mode", "gate", "draws")}
            d.update(ts=ts, t=t, secs=float(w.group("secs")), mode=w.group("mode"), gate=w.group("gate"),
                     draws=int(w.group("draws")) if w.group("draws") is not None else None)
            out["windows"].append(d)
        elif msg.startswith("on foot maps sharp:"):
            for kind, rx in (("on", MAPS_ON_RE), ("off", MAPS_OFF_RE), ("take", MAPS_TAKE_RE), ("back", MAPS_BACK_RE),
                             ("notempty", MAPS_NOTEMPTY_RE), ("notlive", MAPS_NOTLIVE_RE)):
                e = rx.match(msg)
                if e:
                    ev = {"kind": kind, "ts": ts, "t": t}
                    for k, v in e.groupdict().items():
                        ev[k] = float(v) if k == "secs" else int(v) if v.isdigit() else v
                    out["events"].append(ev)
                    break
            else:
                out["unparsed"].append(raw)
        elif "vr world route: RELEASED the world" in msg:
            e = MAPS_ROUTE_RELEASED_RE.search(msg)
            if e:
                out["route"].append({"kind": "released", "ts": ts, "t": t, "frame": int(e.group("frame")), "why": e.group("why")})
        elif "vr world route: OWNS the world" in msg:
            e = MAPS_ROUTE_OWNS_RE.search(msg)
            if e:
                out["route"].append({"kind": "owns", "ts": ts, "t": t, "frame": int(e.group("frame"))})
        elif msg.startswith("luma probe: eye=") and " game=" in msg:
            lm = MAPS_LUMA_RE.match(msg)
            if lm:
                fm = MAPS_LUMA_STAGE_RE.match(lm.group("final"))
                out["luma"].append({"ts": ts, "t": t, "eye": int(lm.group("eye")),
                                    "final_black": int(fm.group("black")) if fm else None,
                                    "final_mean": float(fm.group("mean")) if fm else None})
        elif any(n in msg for n in MAPS_BLACK_NEEDLES):
            out["black"].append({"ts": ts, "t": t, "line": msg[:160]})
        elif "native temporal: layer-only declined" in msg:
            out["declined"].append({"ts": ts, "t": t, "line": msg[:200]})
    return out


def maps_sharp_episodes(p):
    """Panel periods: each TAKES line paired with the next HANDS BACK or OFF line. {take, end, frames, secs, only, kept, why, route_released,
    route_owns, luma_samples, luma_black, open}. `open` is a TAKES still unanswered at the end of the log.

    A period also starts at an ON line whose gate starts as a panel (the key on, or the layer live, while the screen already shows
    nothing the world camera names: the main menu, a load, the arrival after it). Its `take` is that ON line, marked from_on, with
    no world frames behind it. Without it the arrival with the key on from launch had no TAKES line, and its HANDS BACK closed
    nothing the report could show."""
    eps = []
    cur = None
    for ev in p["events"]:
        if ev["kind"] == "on" and ev.get("start") == "not the world":
            if cur is not None:
                cur["open"] = True
                eps.append(cur)
            cur = {"take": {"kind": "take", "ts": ev["ts"], "t": ev["t"], "frame": ev["frame"], "run": 0, "world": 0, "secs": 0.0,
                            "journal": ev["journal"], "from_on": True}, "end": None, "open": False}
        elif ev["kind"] == "take":
            if cur is not None:   # a second take with no hand-back between (a switch in and out): the earlier one ended unseen
                cur["open"] = True
                eps.append(cur)
            cur = {"take": ev, "end": None, "open": False}
        elif ev["kind"] in ("back", "off") and cur is not None:
            cur["end"] = ev
            if ev["kind"] == "back":
                cur.update(frames=ev["frames"], secs=ev["secs"], only=ev["only"], kept=ev["kept"], why=ev["why"])
            else:
                cur.update(frames=None, secs=ev["t"] - cur["take"]["t"], only=None, kept=None, why="the gate stopped: " + ev["why"])
            eps.append(cur)
            cur = None
    if cur is not None:
        cur["open"] = True
        eps.append(cur)
    for e in eps:
        t0 = e["take"]["t"]
        # A period that began at the ON line had no world to let go of: no route release belongs to it.
        rel = [] if e["take"].get("from_on") else [r for r in p["route"] if r["kind"] == "released" and abs(r["t"] - t0) <= 1.0]
        e["route_released"] = min(rel, key=lambda r: abs(r["t"] - t0)) if rel else None
        # The luma probe's samples inside the period: the final stage is the texture handed to the VR half, the layer's composite
        # included. Black by content after a load (nothing is drawn); black under a map is a defect. The judge tells them apart by
        # what the journal said when the period began.
        end_t = e["end"]["t"] if e["end"] is not None else float("inf")
        samples = [l for l in p["luma"] if t0 <= l["t"] <= end_t and l["final_black"] is not None]
        e["luma_samples"] = len(samples)
        e["luma_black"] = sum(1 for l in samples if l["final_black"] >= 99)
        # What the 2D screen did in the period, from the 5 s windows that overlap it (a window is stamped when it closes). Composites are
        # only ever TAKEN in panel frames, so the taken count needs no trimming. A window with world frames in it cannot say how many of
        # the composites it saw were drawn in its panel frames, so the drawn count uses only the whole-panel windows (gate=panel at its
        # close and no world frame in it): there, 0 drawn means nothing was on the 2D screen (the cockpit after boarding, a loading
        # black) and a drawn count above the taken count means the layer left composites in the game's frame.
        wins = [w for w in p["windows"] if w["t"] > t0 and w["t"] - w["secs"] < end_t]
        e["takes"] = sum(w["takes"] for w in wins)
        pure = [w for w in wins if w["gate"] == "panel" and w["world"] == 0 and w["panel"] > 0]
        e["pure_windows"] = len(pure)
        e["pure_frames"] = sum(w["panel"] for w in pure)
        e["pure_takes"] = sum(w["takes"] for w in pure)
        e["pure_draws"] = sum(w["draws"] for w in pure) if pure and all(w["draws"] is not None for w in pure) else None
        if e["end"] is not None and e["end"]["kind"] == "back":
            owns = [r for r in p["route"] if r["kind"] == "owns" and 0 <= r["t"] - e["end"]["t"] <= 2.0]
            e["route_owns"] = min(owns, key=lambda r: r["t"]) if owns else None
        else:
            e["route_owns"] = None
    return eps


def maps_sharp_judge(p, eps):
    """(stops, warns, notes): lists of sentences. STOP is what would ruin a flight (the design's STOP list: a release in the world, flapping, a
    black eye); WARN is what says the feature did not do all it should (an eye kept the upscaler, a door that ran for too few eyes, the route
    and the gate letting go apart); notes are facts."""
    stops, warns, notes = [], [], []
    ws = p["windows"]
    for w in ws:
        if w["takes"] and not w["recognised"]:
            stops.append("%s: %d taken 2D screen composites and none recognised -- screen motion's recognition never ran for a taken screen, so "
                         "the panel could never come back to the eye route (the design's trap)" % (w["ts"], w["takes"]))
        elif w["takes"] and w["recognised"] * 10 < w["takes"] * 9:
            warns.append("%s: only %d of %d taken composites were recognised" % (w["ts"], w["recognised"], w["takes"]))
        if w["notlive"]:
            warns.append("%s: the key is on but the gate was not decided by naming for %d frame(s)" % (w["ts"], w["notlive"]))
        if w["notempty"]:
            warns.append("%s: %d eye(s) had the screen taken but the game drew something else into an eye-sized target, so the upscaler ran for "
                         "them (door-not-empty)" % (w["ts"], w["notempty"]))
        # The door should have run for every composite the layer TOOK (an eye whose 2D screen was taken with nothing else drawn into it): the
        # expectation is the taken composites, not the panel frames. A panel frame with nothing on the 2D screen (the cockpit after boarding,
        # a loading black) has nothing taken and nothing for the door to skip; the first flight read as a failed door because this rule
        # counted panel frames (design doc 8.10).
        if w["takes"] and w["only"] < MAPS_DOOR_SHARE * w["takes"] and not w["notempty"]:
            warns.append("%s: %d taken composites but only %d eyes through the layer-only door (expected about %d): the upscaler ran for the rest "
                         "without a counted reason" % (w["ts"], w["takes"], w["only"], w["takes"]))
        # A window that was panel from start to end, in which the decision saw more 2D screen composites than the layer took: composites
        # the gate gave the layer and the layer left in the game's frame. (Only the new 5 s line counts what the decision saw.)
        if w["draws"] is not None and w["gate"] == "panel" and w["world"] == 0 and w["panel"] and w["draws"] > w["takes"]:
            warns.append("%s: the gate gave the layer the panel for the whole window and the decision saw %d 2D screen composites, but only %d were "
                         "taken: the layer left %d in the game's frame" % (w["ts"], w["draws"], w["takes"], w["draws"] - w["takes"]))
    for e in eps:
        t0 = e["take"]["ts"]
        if e["takes"] == 0 and e["pure_windows"]:
            if e["pure_draws"] == 0:
                notes.append("%s: no 2D screen composite was drawn in this panel period's %d whole-panel window(s) (%d frames): the cockpit after "
                             "boarding, a load, anything that draws no screen. Nothing for the layer to take and nothing for the door to skip"
                             % (t0, e["pure_windows"], e["pure_frames"]))
            elif e["pure_draws"] is None:
                notes.append("%s: the layer took no 2D screen composite in this panel period's %d whole-panel window(s) (%d frames). This log's 5 s "
                             "line has no screen-draws, so \"nothing was drawn\" cannot be told from \"drawn and not taken\" here; the journal, the "
                             "on-foot source lines and the layer's own 30 s `2D screen draws asked` can" % (t0, e["pure_windows"], e["pure_frames"]))
        if e["luma_black"]:
            if e["take"].get("journal") == "on foot" and e["takes"] > 0:
                warns.append("%s: the luma probe read the final stage black in %d of %d sample(s) of a panel period the journal calls on foot: a "
                             "map or a menu should be on screen, so look at the headset's picture for this stretch"
                             % (t0, e["luma_black"], e["luma_samples"]))
            elif e["takes"] == 0:
                notes.append("%s: the luma probe read the final stage black in %d of %d sample(s) of this panel period, in which the layer held no "
                             "2D screen: not the layer's picture. A load's black, a fade, the game closing (the first flight's four samples were "
                             "its exit fade, seconds before the shutdown totals)" % (t0, e["luma_black"], e["luma_samples"]))
            else:
                notes.append("%s: the luma probe read the final stage black in %d of %d sample(s) of this panel period (the journal: %s): black "
                             "by content when nothing is drawn after a load, so this is the arrival and not a black eye"
                             % (t0, e["luma_black"], e["luma_samples"], e["take"].get("journal", "?")))
        if e["end"] is None or e["open"]:
            notes.append("%s: the panel period starting here was still open at the end of the log" % t0)
            continue
        if e["frames"] is not None and e["frames"] < MAPS_SHORT_PANEL_FRAMES:
            stops.append("%s: a panel period of %d frames (%.2f s) -- shorter than a person opens a map or a menu: the gate released in the world "
                         "and held again (a flap)" % (t0, e["frames"], e["secs"]))
        if e["kept"]:
            warns.append("%s: %d eye(s) of this panel period kept the upscaler because something else was drawn into them" % (t0, e["kept"]))
        if e["route_released"] is not None and abs(e["route_released"]["t"] - e["take"]["t"]) > MAPS_SAME_BOUNDARY_S:
            warns.append("%s: the VR world route let go %.0f ms from the gate (not the same boundary)"
                         % (t0, 1000 * abs(e["route_released"]["t"] - e["take"]["t"])))
    for b in p["black"]:
        stops.append("%s: a black eye was reported (%s)" % (b["ts"], b["line"]))
    for d in p["declined"]:
        warns.append("%s: the layer-only door declined an eye (%s)" % (d["ts"], d["line"]))
    if not ws:
        warns.append("no 5 s window line: the feature printed nothing while the key was on, so it never ran")
    if p["unparsed"]:
        warns.append("%d 'on foot maps sharp' line(s) the reader does not know (the formatters changed?); the first: %s"
                     % (len(p["unparsed"]), p["unparsed"][0].strip()[:200]))
    if not any(e["end"] is not None and not e["open"] and e["end"]["kind"] == "back" for e in eps):
        warns.append("no panel period closed in this log: nothing was taken and handed back (a map or a menu opened and closed is what "
                     "the flight is for)")
    return stops, warns, notes


def _maps_period_screens(e):
    """One line on what the 2D screen did in a panel period's whole-panel windows (no world frame in them), or None when it had none."""
    if not e["pure_windows"]:
        return None
    if e["pure_draws"] is None:
        return ("      in the period's %d whole-panel window(s) (%d frames) the layer took %d 2D screen composite(s); this log's 5 s line does not "
                "count the composites the decision saw" % (e["pure_windows"], e["pure_frames"], e["pure_takes"]))
    return ("      2D screen composites in the period's %d whole-panel window(s) (%d frames): drawn %d, taken %d%s"
            % (e["pure_windows"], e["pure_frames"], e["pure_draws"], e["pure_takes"],
               " -- none was drawn: nothing for the layer to take or the door to skip (a cockpit, a load)" if e["pure_draws"] == 0 else ""))


def print_maps_sharp(text):
    """--maps-sharp: the on-foot maps gate's flight in one report; exit 0 (PASS or WARN), 1 (STOP), 3 (no line of the feature in the log)."""
    p = parse_maps_sharp(text)
    if not p["events"] and not p["windows"]:
        print("[edvr] maps-sharp: no 'on foot maps sharp' line in this log. The key experimental.on_foot_maps_sharp was off, the UI layer "
              "was not live, or this build does not have the feature; with the key on and the layer live the feature prints an ON line and "
              "a 5 s line, zeros included.")
        return 3
    eps = maps_sharp_episodes(p)
    ws = p["windows"]
    tot = {k: sum(w[k] for w in ws) for k in ("frames", "named", "unnamed", "world", "panel", "holds", "releases", "takes", "recognised",
                                               "only", "notempty", "notlive")}
    print("[edvr] maps-sharp: %d event line(s), %d five-second window(s), %d panel period(s)" % (len(p["events"]), len(ws), len(eps)))
    for ev in p["events"]:
        if ev["kind"] == "on":
            print("  %s  ON   frame %d, the gate starts as %s (the journal: %s)" % (ev["ts"], ev["frame"], ev["start"], ev["journal"]))
        elif ev["kind"] == "off":
            print("  %s  OFF  frame %d (%s)" % (ev["ts"], ev["frame"], ev["why"]))
        elif ev["kind"] == "notlive":
            print("  %s  NOT LIVE: %s" % (ev["ts"], ev["why"]))
        elif ev["kind"] == "notempty":
            print("  %s  eye %d sequence %d: %d eye draws, the layer took %d" % (ev["ts"], ev["eye"], ev["seq"], ev["draws"], ev["taken"]))
    print("panel periods (a map or a menu the layer held, or a stretch with nothing for a world camera to name: a load, the arrival after it):")
    for e in eps:
        t = e["take"]
        if t.get("from_on"):
            lead = "%s  ON frame %d, the screen already a panel (the journal: %s)" % (t["ts"], t["frame"], t["journal"])
            lead_open = lead_closed = lead
        else:
            lead_open = "%s  TAKES frame %d after %d world frames (%.1f s)" % (t["ts"], t["frame"], t["world"], t["secs"])
            lead_closed = "%s  TAKES frame %d after %d world frames (%.1f s of world)" % (t["ts"], t["frame"], t["world"], t["secs"])
        if e["end"] is None or e["open"]:
            print("  %s -> still open at the end of the log" % lead_open)
            screens = _maps_period_screens(e)
            if screens:
                print(screens)
            continue
        if e["frames"] is not None:
            tail = "%d frames (%.1f s), %d eyes layer-only, %d kept the upscaler -> %s" % (e["frames"], e["secs"], e["only"], e["kept"], e["why"])
        else:
            tail = "%.1f s, then %s" % (e["secs"], e["why"])
        print("  %s  ->  %s  %s" % (lead_closed, e["end"]["ts"], tail))
        if e["route_released"] is not None:
            print("      the VR world route let go at %s (%+.0f ms from the take: %s)"
                  % (e["route_released"]["ts"], 1000 * (e["route_released"]["t"] - t["t"]), e["route_released"]["why"]))
        if e["route_owns"] is not None:
            print("      the route owned the world again at %s (%.0f ms after the hand-back)"
                  % (e["route_owns"]["ts"], 1000 * (e["route_owns"]["t"] - e["end"]["t"])))
        if e["luma_samples"]:
            print("      luma probe: %d sample(s) in this period, the final stage black in %d (black by content for a loading stretch, a "
                  "defect under a map or a menu)" % (e["luma_samples"], e["luma_black"]))
        screens = _maps_period_screens(e)
        if screens:
            print(screens)
    if ws:
        print("5 s windows, summed (%d): %d frames, named %d / unnamed %d, world %d / panel %d, %d hold(s), %d release(s); %d screen takes, %d "
              "recognised; %d eyes through the layer-only door, %d kept the upscaler; %d frame(s) not decided by naming"
              % (len(ws), tot["frames"], tot["named"], tot["unnamed"], tot["world"], tot["panel"], tot["holds"], tot["releases"], tot["takes"],
                 tot["recognised"], tot["only"], tot["notempty"], tot["notlive"]))
        if tot["panel"]:
            # Against the composites the layer TOOK, not 2 x the panel frames: a panel frame with nothing on the 2D screen (the cockpit after
            # boarding) takes nothing and has nothing for the door to skip (the first flight's "46.1%" counted those frames as failures).
            if tot["takes"]:
                print("  panel frames: %d; composites taken: %d, and the layer-only door ran for %.1f%% of them"
                      % (tot["panel"], tot["takes"], 100.0 * tot["only"] / tot["takes"]))
            else:
                print("  panel frames: %d; the layer took no 2D screen composite" % tot["panel"])
            if all(w["draws"] is not None for w in ws):
                print("  the decision saw %d 2D screen composites in these windows, %d of them taken"
                      % (sum(w["draws"] for w in ws), tot["takes"]))
    stops, warns, notes = maps_sharp_judge(p, eps)
    for n in notes:
        print("  note: %s" % n)
    for w in warns:
        print("  WARN: %s" % w)
    for s in stops:
        print("  STOP: %s" % s)
    verdict = "STOP" if stops else "WARN" if warns else "PASS"
    print("maps-sharp verdict: %s (%d STOP, %d WARN). PASS: every map and menu was taken within 3 frames (the TAKES line) and handed back within "
          "2 named frames of closing, no flap in the world, no black eye, the recognition ran for every taken composite, the door ran for the "
          "eyes. STOP: a release in the world, a flap, a black eye, a taken screen never recognised. WARN: an eye kept the upscaler, a door "
          "short of its eyes, the route and the gate letting go apart." % (verdict, len(stops), len(warns)))
    return 1 if stops else 0


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
                    to_local=datetime.timedelta(0), on_line=None):
    """One pass over a log: its `periodic work:` lines and its long frames on
    the common clock, plus the first and last stamped line.

    kind "gfx": a local time-of-day prefix. base_days is the flight's first day
    (days since 2000-01-01) and start_tod the time of day the file name says the
    log opened; the prefix has no date, so the day rolls over whenever the clock
    goes back by more than half a day (a line stamped just before a midnight
    already crossed, which threads can write out of order, keeps the old day).
    kind "rt": a full UTC prefix, turned to local by to_local.

    on_line(message, t), when given, is called for every stamped line with the
    text after its prefix and its time on the common clock, after the line is
    filed here: --freezes reads its own lines through it, on this one clock,
    instead of keeping a second copy of the day and zone arithmetic."""
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
        msg = raw[m.end():]
        _note_line(scan, msg, t)
        if on_line is not None:
            on_line(msg, t)
    return scan


def scan_flight(gfx_path, gfx_text, rt_path=None, rt_text=None,
                gfx_on_line=None, rt_on_line=None):
    """Both logs scanned onto the one local clock. Returns (graphics scan,
    runtime scan or None, clock), clock naming how the runtime's UTC became
    local. The graphics log's date is the one its file name carries; without a
    usable name it is the runtime log's first local day. gfx_on_line and
    rt_on_line are scan_flight_log's on_line for the two logs."""
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
    gscan = scan_flight_log(gfx_text, "gfx", base_days, start_tod,
                            on_line=gfx_on_line)
    rscan = None
    if rt_text is not None:
        rscan = scan_flight_log(rt_text, "rt", to_local=to_local,
                                on_line=rt_on_line)
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


def open_runtime_log(gfx_path, gfx_ver, want, runtime_file, native_dirs,
                     lacking="native_long_cycle and frame_cycle_report lines "
                             "are unavailable"):
    """The runtime log that goes with a graphics log, read, and whether it is
    the right build: its lines are as much evidence as the graphics log's.
    runtime_file (--runtime-file) names it, else pair_runtime_log finds the one
    that opened nearest. Prints what it found. Returns (path or None, text or
    None, exit code or None): a code means stop and return it (1 for a
    --runtime-file that is not there, 2 for a runtime log of another build than
    the one --expect-build named). `lacking` says what a missing runtime log
    takes away from the report that asked."""
    rt_path = rt_why = None
    if runtime_file:
        rt_path = os.path.abspath(runtime_file)
        if not os.path.isfile(rt_path):
            print("[edvr] no such runtime log: %s" % rt_path)
            return None, None, 1
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
                return rt_path, rt_text, 2
        elif gfx_ver and rt_ver and not version_matches(gfx_ver, rt_ver):
            print("[edvr] WARNING: the runtime log is from build %s and the "
                  "graphics log from %s; they are not one flight of one build."
                  % (rt_ver, gfx_ver))
    else:
        print("[edvr] runtime log: NONE FOUND -- %s.\n"
              "       %s; only the graphics log is read." % (rt_why, lacking))
    return rt_path, rt_text, None


def print_periodic_report(gfx_path, gfx_text, gfx_ver, want, args, native_dirs):
    """The --tally periodic report. Returns the process exit code."""
    window_s = args.window_ms / 1000.0

    rt_path, rt_text, rc = open_runtime_log(gfx_path, gfx_ver, want,
                                            args.runtime_file, native_dirs)
    if rc is not None:
        return rc

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


# ---------------------------------------------------------------------------------------------------------------------------------------
# --vscreen-fit: fix.vscreen_res_width = auto, fitted to what each eye shows (design doc section 82, the "vscreen auto-fit" entry).
# src/common/vscreen_fit.h writes the `vScreen resolution: auto = ...` rule line and the `vscreen footprint 30s:` line; the armed line
# is src/d3d11/vscreen_footprint.cpp's. tools\vscreen_fit_test holds the formatters to tools\vscreen_fit_fixture.log, the file this
# reader's own self-test reads. The constants below are that header's (kMultiplier, kFloorWidth, kLegacyMultiplier, the calibration
# point, the stored quantile); the rig pins the header, and self_test_vscreen_fit pins this copy of it to the header's text.
# What the log's session-frac1= and persisted= tokens carry is the session's p10 (the head-on floor), not a median: a window's own
# fp / range / h / shape stay a median and a min..max.
# ---------------------------------------------------------------------------------------------------------------------------------------
VSCREEN_M = 0.70                # kMultiplier: the fitted width is this share of the screen's head-on footprint
VSCREEN_FLOOR = 2880            # kFloorWidth
VSCREEN_LEGACY_M = 1.25         # kLegacyMultiplier
VSCREEN_CHOSEN = 3504.0         # kChosenWidthPx: the width Sean chose for his rig (4032 px eye, panel distance 0.7)
VSCREEN_SEED = (5006.0, 0.7, 4032.0)   # kSeedFootprintPx, kSeedDistance, kSeedEyeWidthPx: the calibration point (the screen's head-on px there)
VSCREEN_QUANTILE = 0.10         # kFootprintQuantile: the stored estimator, p10
VSCREEN_SHAPE = 16.0 / 9.0
VSCREEN_MIN_FOOT = 12           # a window with fewer on-foot samples than this is a transition (a menu, a map), not a measurement of the screen
VSCREEN_STABLE = 0.08           # the windows' widths at distance 1 may differ this much, (max - min) / median (head turning moves a window by a few %)
VSCREEN_SHAPE_PASS = (-0.09, 0.03)   # the median window shape may sit this far below / above its reference and pass: a turned head stretches the height
VSCREEN_SHAPE_STOP = 0.15       # beyond this far either way the corner arithmetic or the eye size is wrong; between is a WARN
VSCREEN_LAW = 0.02              # footprint at distance 1 may differ this much between windows at different distances
VSCREEN_CALIBRATION = 0.05      # the session's stored p10 may differ this much from the calibration point before m is re-derived
VSCREEN_RULE_TOL = 16           # the width a rule line prints may differ this much from the one its own tokens give (the footprint prints rounded)

VSCREEN_RULE_RE = re.compile(r"^(?:\[(?P<ts>[0-9:.]+)\] )?vScreen resolution: auto = (?P<w>\d+) wide: (?P<rest>.*)$")
VSCREEN_APPLY_RE = re.compile(r"^(?:\[(?P<ts>[0-9:.]+)\] )?vScreen resolution: (?P<sw>\d+)x(?P<sh>\d+) -> (?P<w>\d+)x(?P<h>\d+) at (?P<sites>\d+) site")
VSCREEN_EXPLICIT_RE = re.compile(r"^(?:\[(?P<ts>[0-9:.]+)\] )?vScreen resolution: explicit (?P<w>\d+) wide")
VSCREEN_FOOT_RE = re.compile(r"^(?:\[(?P<ts>[0-9:.]+)\] )?vscreen footprint 30s: (?P<rest>.*)$")
VSCREEN_ARMED_RE = re.compile(r"^(?:\[(?P<ts>[0-9:.]+)\] )?vscreen footprint: (?P<what>armed|not armed|STOOD DOWN)")
# The graphics log's one line that carries the eyes' FOV (src/d3d11/perf_monitor.cpp, logNativeBenchmark, once a benchmark window):
# `native benchmark workload: window N, ... game FOV radians L l/r/u/d R l/r/u/d; order left/right/up/down; ...`. Angles, not
# tangents; the left eye's are the eye the footprint instrument's eye size comes from.
VSCREEN_FOV_RE = re.compile(r"native benchmark workload: .*?game FOV radians L (?P<l>-?\d+\.\d+)/(?P<r>-?\d+\.\d+)/(?P<u>-?\d+\.\d+)/(?P<d>-?\d+\.\d+) R ")


def _vround16(value):
    return int(value / 16.0 + 0.5) * 16


def _vnum(text):
    """A token's number (a leading float), else None: `3504`, `0.8690`, `-` -> 3504.0, 0.869, None."""
    m = re.match(r"^-?\d+(?:\.\d+)?", text or "")
    return float(m.group(0)) if m else None


def parse_vscreen_fit(text):
    """The log's vscreen auto-fit lines: {rule: {ts, width, kv, prose} (the launch's first auto line) or None, explicit: width or None,
    no_eye: bool (auto with no eye width on record), off: bool, applied: {ts, src, w, h, sites} or None, armed: [(ts, what)],
    windows: [{ts, kv}], fov: (left, right, up, down) in radians of the left eye from the log's first `native benchmark workload:`
    line, or None}. A line cut short or garbled is skipped, never fatal."""
    f = {"rule": None, "explicit": None, "no_eye": False, "off": False, "applied": None, "armed": [], "windows": [], "fov": None}
    for raw in text.splitlines():
        try:
            if f["fov"] is None and "game FOV radians" in raw:
                m = VSCREEN_FOV_RE.search(raw)
                if m:
                    l, r, u, d = (float(m.group(k)) for k in "lrud")
                    if l < 0.0 < r and d < 0.0 < u and max(abs(l), r, u, abs(d)) < 1.5:   # a real FOV: a flat log's zeros are none
                        f["fov"] = (l, r, u, d)
                continue
            m = VSCREEN_RULE_RE.match(raw)
            if m:
                head, _, prose = m.group("rest").partition(" -- ")
                if f["rule"] is None:
                    f["rule"] = {"ts": m.group("ts") or "", "width": int(m.group("w")), "kv": _ckv(head), "prose": prose}
                continue
            m = VSCREEN_APPLY_RE.match(raw)
            if m:
                if f["applied"] is None:
                    f["applied"] = {"ts": m.group("ts") or "", "src": (int(m.group("sw")), int(m.group("sh"))),
                                    "w": int(m.group("w")), "h": int(m.group("h")), "sites": int(m.group("sites"))}
                continue
            m = VSCREEN_EXPLICIT_RE.match(raw)
            if m:
                f["explicit"] = int(m.group("w"))
                continue
            if "vScreen resolution: auto, but no per-eye render width is on record" in raw:
                f["no_eye"] = True
                continue
            if "vScreen resolution: off (set to " in raw:
                f["off"] = True
                continue
            m = VSCREEN_ARMED_RE.match(raw)
            if m:
                f["armed"].append((m.group("ts") or "", m.group("what")))
                continue
            m = VSCREEN_FOOT_RE.match(raw)
            if m:
                f["windows"].append({"ts": m.group("ts") or "", "kv": _ckv(m.group("rest"))})
        except (ValueError, TypeError):
            continue
    return f


def vscreen_fit_windows(f):
    """Each window's tokens as numbers: [{window, samples, on_foot, other, skipped, late, draws, why, distance, applied, eye (w, h) or None,
    fp, frac, lo, hi, h, shape, other_fp, at1, frac1, session_n, session_frac1, persisted, fit, legacy, m (the m of the fit= token: a build
    that stored the session's median, before the p10, printed 1.00 here), save_failed (the save-failed= token, read by key: the saves that
    failed so far this session, 0 when the token is absent, as in every log from a build without it and every window before the first failure)}]."""
    out = []
    for w in f["windows"]:
        kv = w["kv"]
        eye = re.match(r"^(\d+)x(\d+)$", kv.get("eye", ""))
        rng = re.match(r"^(-?\d+(?:\.\d+)?)\.\.(-?\d+(?:\.\d+)?)$", kv.get("range", ""))
        out.append({
            "ts": w["ts"], "window": _cint(kv.get("window")), "samples": _cint(kv.get("samples")) or 0, "on_foot": _cint(kv.get("on-foot")) or 0,
            "other": _cint(kv.get("other")) or 0, "skipped": _cint(kv.get("skipped")) or 0, "late": _cint(kv.get("late")) or 0,
            "draws": _cint(kv.get("draws")) or 0, "why": kv.get("why", ""), "distance": _vnum(kv.get("distance")), "applied": _vnum(kv.get("applied")),
            "eye": (int(eye.group(1)), int(eye.group(2))) if eye else None, "fp": _vnum(kv.get("fp")), "frac": _vnum(kv.get("frac")),
            "lo": float(rng.group(1)) if rng else None, "hi": float(rng.group(2)) if rng else None, "h": _vnum(kv.get("h")),
            "shape": _vnum(kv.get("shape")), "other_fp": _vnum(kv.get("other-fp")), "at1": _vnum(kv.get("at1")), "frac1": _vnum(kv.get("frac1")),
            "session_n": _cint(kv.get("session-n")) or 0, "session_frac1": _vnum(kv.get("session-frac1")), "persisted": _vnum(kv.get("persisted")),
            "fit": _vnum(kv.get("fit")), "legacy": _vnum(kv.get("legacy")), "m": _vnum(kv.get("m")), "save_failed": _cint(kv.get("save-failed")) or 0})
    return out


def _vscreen_shape_ref(f, eye):
    """(reference shape, how it was got) for the SHAPE check: 16:9 for square eye pixels, and 16/9 x fx/fy when the log carries the
    eye's FOV (the `native benchmark workload:` line, f["fov"]) and the eye's size: fx and fy are the pixels per unit tangent across and
    down, so a screen that is 16:9 on the page reads 16/9 x fx/fy in an eye whose pixels are not square. Without a FOV the reference is
    16:9 and says so."""
    fov = f.get("fov")
    if fov and eye:
        l, r, u, d = (math.tan(a) for a in fov)
        if r - l > 0.0 and u - d > 0.0 and eye[0] and eye[1]:
            ratio = (eye[0] / (r - l)) / (eye[1] / (u - d))
            return VSCREEN_SHAPE * ratio, "16:9 in this eye's pixels = %.3f (16:9 x fx/fy %.4f, from the log's game FOV)" % (VSCREEN_SHAPE * ratio, ratio)
    return VSCREEN_SHAPE, "16:9 = %.3f (square eye pixels assumed: this log carries no per-eye FOV line)" % VSCREEN_SHAPE


def vscreen_fit_verdict(f):
    """The verdict on one flight: [(tag, status, text)], status PASS, WARN, STOP or n/a (what the log cannot say). The tags are the
    questions the flight plan asks: RULE (did auto choose by the rule it names, and was that what was applied), INSTRUMENT (did the
    footprint instrument run, see the composite and read its sources), ON FOOT (was the screen measured on foot), STABLE (the windows'
    widths at distance 1 agree), SHAPE (the median window's shape is 16:9 in the eye's own pixels), DISTANCE LAW (A varies as 1/d),
    CALIBRATION (is the session's stored head-on floor, p10, the 5006 px behind Sean's 3504), STORED (what the next launch will fit; a WARN
    when any window carries save-failed=N, a save that did not reach the file).
    ON FOOT, STABLE and SHAPE judge only the windows with VSCREEN_MIN_FOOT or more on-foot samples (a window of one or six samples is a
    transition: it is what made a flight read 102% unstable and 1.43 shaped). DISTANCE LAW is unchanged: it keeps every on-foot window."""
    out = []

    def add(tag, status, text):
        out.append((tag, status, text))

    wins = vscreen_fit_windows(f)
    rule = f["rule"]
    applied = f["applied"]

    # ---- RULE ----
    if f["explicit"]:
        add("RULE", "n/a", "an explicit width (%d), used exactly: auto's rule is not in play" % f["explicit"])
    elif f["no_eye"]:
        add("RULE", "n/a", "auto with no per-eye width on record yet: the panel stayed at the game's own 1920x1080 (a fresh install's first launch)")
    elif rule is None:
        add("RULE", "n/a", "no `vScreen resolution: auto =` line: a log that predates the rule, or a session that never reached the panel patch")
    else:
        kv = rule["kv"]
        width = rule["width"]
        name = kv.get("rule")
        eye = _vnum(kv.get("eye"))
        text = ""
        status = "PASS"
        if name == "fitted":
            fp, m, floor, cap = _vnum(kv.get("footprint")), _vnum(kv.get("m")), _vnum(kv.get("floor")), _vnum(kv.get("cap"))
            if None in (fp, m, floor, cap):
                status, text = "WARN", "rule=fitted but its footprint, m, floor or cap token is missing: the width cannot be recomputed"
            else:
                lo = min(floor, cap)
                want = _vround16(min(max(m * fp, lo), cap))
                nudged = kv.get("nudged") == "yes"
                # The footprint prints rounded to a whole pixel, so m x footprint can sit a hair either side of a rounding edge: one step (16)
                # of difference is that, not a wrong width. A nudge off another target's size moves the width by several steps.
                if abs(width - want) <= VSCREEN_RULE_TOL or (nudged and abs(width - want) <= 16 * 8):
                    text = ("FITTED to %d wide from a %s footprint of %.0f px at fix.panel_distance %s on a %d px eye (m=%.3f, floor %d, cap %d, clamp %s%s); "
                            "legacy would have been %s"
                            % (width, kv.get("source", "?"), fp, kv.get("distance", "?"), eye or 0, m, floor, cap, kv.get("clamp", "?"),
                               ", nudged off another target's size" if nudged else "", kv.get("legacy", "?")))
                else:
                    status, text = "STOP", ("the line says %d wide but its own tokens (footprint %.0f, m %.3f, floor %d, cap %d) give %d" % (width, fp, m, floor, cap, want))
        elif name == "legacy":
            want = _vround16((eye or 0) * VSCREEN_LEGACY_M) if eye else None
            why = rule["prose"].partition("because the world route will not run:")[2].partition(". Without the route")[0].strip()
            if want is not None and width != min(max(want, 640), 8192):
                status, text = "STOP", "the legacy width %d is not 125%% of the %d px eye (%d)" % (width, eye, want)
            elif not why:
                status, text = "WARN", "legacy %d wide, but the line does not say which route condition failed" % width
            else:
                text = "LEGACY %d wide (125%% of the %d px eye): the world route will not run: %s" % (width, eye or 0, why)
        else:
            status, text = "WARN", "the auto line has no rule= token (%r): an older build's line" % kv.get("rule")
        if status != "STOP" and applied is not None and applied["w"] != width:
            status, text = "STOP", "%s -- but the panel patch applied %dx%d, not %d wide" % (text, applied["w"], applied["h"], width)
        elif status == "PASS" and applied is None:
            text += " (no `vScreen resolution: ... ->` apply line in this log: the patch did not write, or the line was cut)"
        elif status == "PASS":
            text += "; applied %dx%d at %d site(s)" % (applied["w"], applied["h"], applied["sites"])
        add("RULE", status, text)

    # ---- INSTRUMENT ----
    armed = [a for a in f["armed"] if a[1] == "armed"]
    notarmed = [a for a in f["armed"] if a[1] != "armed"]
    if f["explicit"] and not wins:
        add("INSTRUMENT", "n/a", "not armed by design: an explicit width has nothing to fit%s" % (" (the log says so)" if notarmed else ""))
    elif not f["armed"] and not wins:
        add("INSTRUMENT", "STOP", "no `vscreen footprint` line at all, not even the arming line: the instrument never ran (a build that predates it, the flat "
            "profile, or a session that never reached its first frame boundary)")
    elif notarmed and not armed and not wins:
        add("INSTRUMENT", "n/a", "not armed: %s" % notarmed[0][1])
    elif not wins:
        add("INSTRUMENT", "WARN", "armed, but no 30 s window closed: the session was shorter than 30 s")
    else:
        total = sum(w["samples"] for w in wins)
        foot = sum(w["on_foot"] for w in wins)
        draws = sum(w["draws"] for w in wins)
        skipped = sum(w["skipped"] for w in wins)
        late = sum(w["late"] for w in wins)
        why = {}
        for w in wins:
            for part in w["why"].split(","):
                key, _, n = part.partition(":")
                if key and n.isdigit():
                    why[key] = why.get(key, 0) + int(n)
        base = "%d window(s): %d sample(s) (%d on foot, %d other), %d composite draw(s) seen, %d skipped%s, %d late" % (
            len(wins), total, foot, total - foot, draws, skipped, " (%s)" % ", ".join("%s x%d" % kv for kv in sorted(why.items())) if why else "", late)
        if draws == 0:
            add("INSTRUMENT", "STOP", "armed and ticking, but the 2D screen's composite was never seen (draws=0 in every window): the recognition did not match, "
                "or the screen was never on show. " + base)
        elif total == 0:
            add("INSTRUMENT", "STOP", "the composite was seen but not one sample could be read: " + base)
        elif skipped > total:
            add("INSTRUMENT", "WARN", "more samples skipped than read: " + base)
        elif late:
            add("INSTRUMENT", "WARN", "a copy was not ready in time: " + base)
        else:
            add("INSTRUMENT", "PASS", base)

    # ---- ON FOOT ----
    # foot_any: every window that saw the screen on foot. foot_wins: those with enough samples to judge. A window of one or six on-foot
    # samples is a transition (a menu or a map came up, the commander got off the ship): its median is the head's pose, not the screen.
    foot_any = [w for w in wins if w["on_foot"] and w["fp"] is not None]
    foot_wins = [w for w in foot_any if w["on_foot"] >= VSCREEN_MIN_FOOT]
    if wins:
        if not foot_any:
            others = [w["other_fp"] for w in wins if w["other_fp"] is not None]
            add("ON FOOT", "WARN", "no on-foot sample: the screen was measured at the menu only%s, and nothing on foot was stored for the next launch"
                % (" (%.0f px)" % statistics.median(others) if others else ""))
        elif not foot_wins:
            add("ON FOOT", "WARN", "on-foot samples in %d window(s), but no window has %d: %d sample(s) in all is too thin to judge the screen's width, stability or shape"
                % (len(foot_any), VSCREEN_MIN_FOOT, sum(w["on_foot"] for w in foot_any)))
        else:
            fps = [w["fp"] for w in foot_wins]
            thin = len(foot_any) - len(foot_wins)
            add("ON FOOT", "PASS", "%d on-foot window(s), %d sample(s); the screen spans %.0f px (median of the windows' medians %.0f..%.0f) of the eye%s"
                % (len(foot_wins), sum(w["on_foot"] for w in foot_wins), statistics.median(fps), min(fps), max(fps),
                   "; %d thinner window(s) (under %d on-foot samples) left out" % (thin, VSCREEN_MIN_FOOT) if thin else ""))
            # ---- STABLE ----
            # Each window's median in eye pixels at distance 1 (fp x the distance its constants carried), so a window flown at another panel
            # distance compares with the rest; the spread of those, (max - min) / median. What is inside a window (its lo..hi) is the head
            # moving, which is what a floor estimator is for: it is not judged.
            at1 = [w["fp"] * w["applied"] for w in foot_wins if w["applied"]]
            if len(at1) < 2:
                add("STABLE", "n/a", "one window with %d or more on-foot samples: nothing to compare it with" % VSCREEN_MIN_FOOT)
            else:
                spread = (max(at1) - min(at1)) / statistics.median(at1)
                if spread > VSCREEN_STABLE:
                    add("STABLE", "WARN", "the screen's width moved: %.1f%% between %d windows (%.0f..%.0f px at distance 1; over %.0f%%): the head turned, a menu was up, "
                        "or the screen itself changed during the flight" % (spread * 100.0, len(at1), min(at1), max(at1), VSCREEN_STABLE * 100.0))
                else:
                    add("STABLE", "PASS", "the screen's width held: %.1f%% between %d windows (%.0f..%.0f px at distance 1; under %.0f%%)"
                        % (spread * 100.0, len(at1), min(at1), max(at1), VSCREEN_STABLE * 100.0))
            # ---- SHAPE ----
            # The MEDIAN of the windows' shapes against 16:9 (in the eye's own pixels: _vscreen_shape_ref). A head that is not square on to
            # the screen stretches its height at first order and its width at second, so the shape reads under 16:9 and the pass band is
            # lopsided; a window's own lowest sample is the closest to head-on and is not what is judged.
            shapes = [w["shape"] for w in foot_wins if w["shape"] is not None]
            if not shapes:
                add("SHAPE", "n/a", "no shape= token: the eye's height was not known (the runtime had not published its sizing)")
            else:
                ref, ref_how = _vscreen_shape_ref(f, foot_wins[0]["eye"])
                med_shape = statistics.median(shapes)
                dev = med_shape / ref - 1.0
                lo_ok, hi_ok = VSCREEN_SHAPE_PASS
                status = "STOP" if abs(dev) > VSCREEN_SHAPE_STOP else ("PASS" if lo_ok <= dev <= hi_ok else "WARN")
                add("SHAPE", status, "the median of the windows' footprint shapes reads %.3f against %s: %+.1f%% (the windows run %.3f..%.3f; pass %+.0f%% to %+.0f%%, "
                    "stop past %.0f%% either way)%s"
                    % (med_shape, ref_how, dev * 100.0, min(shapes), max(shapes), lo_ok * 100.0, hi_ok * 100.0, VSCREEN_SHAPE_STOP * 100.0,
                       "" if status == "PASS" else ": the corner arithmetic, the eye size or the screen's stretch is not what the instrument assumes"))
            # ---- DISTANCE LAW ----
            by_d = {}
            for w in foot_any:
                if w["applied"] and w["frac1"] is not None:
                    by_d.setdefault(round(w["applied"], 2), []).append(w["frac1"])
            if len(by_d) < 2:
                add("DISTANCE LAW", "n/a", "one panel distance in this log (%s): a restart leg or a live change at another fix.panel_distance tests the 1/d law"
                    % (", ".join("%.2f" % d for d in by_d) or "unknown"))
            else:
                meds = {d: statistics.median(v) for d, v in by_d.items()}
                lo, hi = min(meds.values()), max(meds.values())
                spread = (hi - lo) / statistics.median(meds.values())
                add("DISTANCE LAW", "PASS" if spread <= VSCREEN_LAW else "STOP",
                    "the footprint at distance 1 over %d distances (%s): %.1f%% apart (%s %.0f%%): A %s 1/d"
                    % (len(meds), ", ".join("%.2f -> %.4f" % (d, meds[d]) for d in sorted(meds)), spread * 100.0,
                       "under" if spread <= VSCREEN_LAW else "over", VSCREEN_LAW * 100.0, "varies as" if spread <= VSCREEN_LAW else "does NOT vary as"))
    # ---- CALIBRATION ----
    # Not the window medians: the session's STORED value, the p10 the next launch will fit from (session-frac1 of the last window with
    # 12 or more samples in the session), at the calibration point's eye (a 4032 px one). session-frac1 is a fraction of the eye at distance
    # 1, so x eye / the calibration distance is the screen's head-on footprint at that distance whatever distance was flown (the 1/d law).
    if wins and foot_any:
        seed_fp, seed_d, seed_e = VSCREEN_SEED
        stored = [w for w in wins if w["session_frac1"] is not None and w["session_n"] >= VSCREEN_MIN_FOOT and w["eye"] and w["eye"][0] == int(seed_e)]
        # A build from before the p10 calibration stored the session's MEDIAN and printed m=1.00 on its window lines: its session-frac1 is about 6%
        # above the head-on floor, so it is not held against the calibration point (a window with no m token, an eye not yet known, passes).
        current = [w for w in stored if w["m"] is None or abs(w["m"] - VSCREEN_M) < 1e-6]
        if stored and not current:
            add("CALIBRATION", "n/a", "this log's windows fit at m=%.3f, not %.3f: a build from before the p10 calibration, whose session value is a median, not the head-on "
                "floor the %.0f px calibration point is held against" % (stored[-1]["m"], VSCREEN_M, VSCREEN_SEED[0]))
        elif current:
            s = current[-1]
            got = s["session_frac1"] * int(seed_e) / seed_d
            ratio = got / seed_fp
            if abs(ratio - 1.0) <= VSCREEN_CALIBRATION:
                add("CALIBRATION", "PASS", "at Sean's calibration point (a %d px eye) the session's stored head-on floor (p10) is %.0f px at distance %.1f against the %.0f px "
                    "his %.0f is fitted from (%.1f%%, within %.0f%%): m = %.2f fits %d wide there"
                    % (int(seed_e), got, seed_d, seed_fp, VSCREEN_CHOSEN, ratio * 100.0, VSCREEN_CALIBRATION * 100.0, VSCREEN_M, _vround16(VSCREEN_M * got)))
            else:
                add("CALIBRATION", "WARN", "at Sean's calibration point (a %d px eye) the session's stored head-on floor (p10) is %.0f px at distance %.1f, not the %.0f px his %.0f "
                    "is fitted from (%.1f%%, over %.0f%% apart): m = %.2f would fit %d wide there; m = %.3f reproduces %.0f -- his call whether the seed or the "
                    "measurement is right (vscreen_fit.h kMultiplier, kSeedFootprintPx)"
                    % (int(seed_e), got, seed_d, seed_fp, VSCREEN_CHOSEN, ratio * 100.0, VSCREEN_CALIBRATION * 100.0, VSCREEN_M, _vround16(VSCREEN_M * got),
                       VSCREEN_CHOSEN / got, VSCREEN_CHOSEN))
        elif any(w["eye"] and w["eye"][0] == int(seed_e) for w in wins):
            add("CALIBRATION", "n/a", "the session has fewer than %d on-foot samples: no stored head-on floor to hold against the calibration point yet" % VSCREEN_MIN_FOOT)
        else:
            add("CALIBRATION", "n/a", "not at the calibration point (a %d px eye)" % int(seed_e))

    # ---- STORED ----
    if wins:
        last = wins[-1]
        persisted = [w for w in wins if w["persisted"] is not None]
        # A save that failed: the 30 s line carries save-failed=N (read by key; the count so far this session, absent before the first failure and in
        # every log from a build without it) and the log has a `vscreen footprint: SAVE FAILED (Win32 error N)` line for the first three. The file kept
        # what it held, so nothing below may claim the session's value is what the next launch will read.
        failed = [w for w in wins if w["save_failed"] > 0]
        if failed:
            reached = ("the last value that did reach it is %.4f of the eye at panel distance 1 (window %s)" % (persisted[-1]["persisted"], persisted[-1]["window"])
                       if persisted else "nothing from this session has reached it (persisted=no on every window)")
            add("STORED", "WARN", "%d save(s) of the on-foot footprint FAILED (first in window %s; the log's `vscreen footprint: SAVE FAILED` line names the Win32 error): "
                "the file keeps its previous value and a later window tries again; %s" % (max(w["save_failed"] for w in wins), failed[0]["window"], reached))
        elif persisted:
            p = persisted[-1]
            fit = p["fit"]
            note = ""
            if applied is not None and fit:
                note = "; the next launch fits %d wide (this launch: %d)" % (fit, applied["w"]) if abs(fit - applied["w"]) > 16 else \
                    "; the next launch fits %d wide, the width this launch ran at (%d)" % (fit, applied["w"])
            if rule is not None and rule["kv"].get("route") == "no":
                note += " -- this launch's route=no (legacy), so a launch fits only once the world route will run"
            # The window lines of a build from before the p10 calibration print m=1.00, and what that build stored is the session's median.
            old_m = next((w["m"] for w in wins if w["m"] is not None and abs(w["m"] - VSCREEN_M) > 1e-6), None)
            what = "the on-foot head-on floor (p10)" if old_m is None else \
                "the on-foot MEDIAN (this log is from a build before the p10 calibration: m=%.3f on its window lines)" % old_m
            add("STORED", "PASS", "%s is stored: %.4f of the eye at panel distance 1 (%d sample(s))%s" % (what, p["persisted"], p["session_n"], note))
        elif foot_any:
            add("STORED", "WARN", "on-foot samples were taken (%d) but nothing was stored: the file needs at least 12 on-foot samples in the session"
                % sum(w["on_foot"] for w in foot_any))
        else:
            add("STORED", "n/a", "nothing was stored: no on-foot samples")
    return out


def vscreen_fit_summary(verdict):
    counts = {"PASS": 0, "WARN": 0, "STOP": 0, "n/a": 0}
    for _, status, _ in verdict:
        counts[status] = counts.get(status, 0) + 1
    worst = "STOP" if counts["STOP"] else ("WARN" if counts["WARN"] else ("PASS" if counts["PASS"] else "n/a"))
    return "vscreen fit verdict: %s (%d PASS, %d WARN, %d STOP, %d n/a)" % (worst, counts["PASS"], counts["WARN"], counts["STOP"], counts["n/a"])


def print_vscreen_fit(text):
    """The --vscreen-fit report. Returns the process exit code: 0 when the log has any of the auto-fit lines, 1 when it has none (a log
    from a build that predates them, or no VR session); the verdict never changes the exit code (read its lines)."""
    f = parse_vscreen_fit(text)
    wins = vscreen_fit_windows(f)
    lines = (1 if f["rule"] else 0) + (1 if f["explicit"] else 0) + (1 if f["no_eye"] else 0) + len(f["armed"]) + len(wins)
    if not lines:
        print("[edvr] no vscreen auto-fit line in this log (a build that predates the fit, or a session that never reached the panel patch).")
        return 1
    print("[edvr] vscreen fit: %s, %s, %d arming line(s), %d footprint window(s)"
          % ("the rule line" if f["rule"] else ("an explicit-width line" if f["explicit"] else "no rule line"),
             "an apply line" if f["applied"] else "no apply line", len(f["armed"]), len(wins)))
    if f["rule"]:
        kv = f["rule"]["kv"]
        print("launch: auto = %d wide, rule=%s source=%s route=%s eye=%s distance=%s footprint=%s clamp=%s legacy=%s%s"
              % (f["rule"]["width"], kv.get("rule", "?"), kv.get("source", "?"), kv.get("route", "?"), kv.get("eye", "?"), kv.get("distance", "?"),
                 kv.get("footprint", "-"), kv.get("clamp", "-"), kv.get("legacy", "?"),
                 "; applied %dx%d" % (f["applied"]["w"], f["applied"]["h"]) if f["applied"] else ""))
    for w in wins:
        print("window %s: samples %d (on foot %d, other %d), draws %d, skipped %d, late %d; eye %s distance %s applied %s; on foot fp=%s frac=%s range=%s..%s "
              "shape=%s; at distance 1 %s px (%s); session n=%d session-frac1 %s persisted %s%s; next launch fits %s (legacy %s)"
              % (w["window"], w["samples"], w["on_foot"], w["other"], w["draws"], w["skipped"], w["late"],
                 "%dx%d" % w["eye"] if w["eye"] else "-", w["distance"], w["applied"], "%.0f" % w["fp"] if w["fp"] is not None else "-",
                 w["frac"], "%.0f" % w["lo"] if w["lo"] is not None else "-", "%.0f" % w["hi"] if w["hi"] is not None else "-", w["shape"],
                 "%.0f" % w["at1"] if w["at1"] is not None else "-", w["frac1"], w["session_n"], w["session_frac1"], w["persisted"],
                 " save-failed %d" % w["save_failed"] if w["save_failed"] else "",
                 "%.0f" % w["fit"] if w["fit"] is not None else "-", "%.0f" % w["legacy"] if w["legacy"] is not None else "-"))
    verdict = vscreen_fit_verdict(f)
    for tag, status, text_ in verdict:
        print("%s (%s) %s" % (status, tag, text_))
    print(vscreen_fit_summary(verdict))
    return 0


# ---------------------------------------------------------------------------------------------------------------------------------------
# --flat-upscale: the flat profile's final copy admitted by structure (design doc section 83), and what follows from it. Below the output
# (Elite's supersampling under 1.0) the game's own copy upscales; the structure admission is what lets DLSS and FSR resolve there whatever
# bloom, depth of field and the tone variant do. The lines are written by src/d3d11/flat_copy_structure.h (`flat copy structure 5s:`, the
# first admission and the declines), flat_standdown.h, flat_elite_settings.h and flat_runtime.cpp; tools\flat_temporal_test holds the
# formatters to tools\flat_upscale_fixture.log (a good flight and three episodes), the file this reader's self-test reads.
# ---------------------------------------------------------------------------------------------------------------------------------------
FLATU_TS = r"^(?:\[(?P<ts>[0-9:.]+)\] )?"
FLATU_KEY_RE = re.compile(FLATU_TS + r"flat hdr route: (?:experimental\.temporal_aa_before_post=)?(?P<key>\w+) \((?P<when>read at startup|changed)\) at frame=(?P<frame>\d+)")
FLATU_TRIGGER_RE = re.compile(FLATU_TS + r"flat hdr route: first trigger at frame=(?P<frame>\d+)")
FLATU_WINDOW_RE = re.compile(FLATU_TS + r"flat copy structure 5s: (?P<rest>.*)$")
FLATU_FIRST_RE = re.compile(
    FLATU_TS + r"flat copy structure: first admission at frame=(?P<frame>\d+) \((?:experimental\.temporal_aa_before_post=auto|the HDR route is on)\): the game's final copy reads a "
    r"(?P<w>\d+)x(?P<h>\d+) R8G8B8A8 image written by one pass, VS=(?P<vs>[0-9A-F]+) PS=(?P<ps>[0-9A-F]+), after the scene HDR's first consumer "
    r"\(VS=(?P<tvs>[0-9A-F]+) PS=(?P<tps>[0-9A-F]+)\), .*?\(the whitelist said (?P<wl>[\w-]+)\); the scene is (?P<sw>\d+)x(?P<sh>\d+) on a "
    r"(?P<ow>\d+)x(?P<oh>\d+) output(?P<menu> \(the 3D menu\))?, route=(?P<route>[\w-]+);")
FLATU_DECLINED_RE = re.compile(
    FLATU_TS + r"flat copy structure: declined at frame=(?P<frame>\d+): (?P<why>[\w-]+) \(the whitelist said (?P<wl>[\w-]+)\); the final copy reads a "
    r"(?P<w>\d+)x(?P<h>\d+) fmt (?P<fmt>\d+) image with (?P<writers>\d+) writer\(s\) .*?, (?P<passes>\d+) R-sized image pass\(es\) between")
FLATU_ROUTE_RE = re.compile(FLATU_TS + r"flat route: (?P<name>[\w-]+) R=(?P<rw>\d+)x(?P<rh>\d+) E=(?P<ew>\d+)x(?P<eh>\d+) D=(?P<dw>\d+)x(?P<dh>\d+)(?P<rest>.*)$")
FLATU_STAND_RE = re.compile(FLATU_TS + r"flat stand-down: (?P<what>entered|resumed|ended|still stood down) (?P<rest>.*)$")
FLATU_STAND_ENTERED_RE = re.compile(r"^at frame=(?P<frame>\d+): every frame for (?P<secs>[\d.]+) s \((?P<frames>\d+) frames\) was refused for (?P<reason>[\w-]+)(?: \((?P<words>[^)]*)\))?, none treated")
FLATU_STAND_STILL_RE = re.compile(r"^at frame=(?P<frame>\d+) after (?P<secs>\d+) s: refused for (?P<reason>[\w-]+)(?: \((?P<words>[^)]*)\))?, (?P<probes>\d+) probes so far")
FLATU_WARN_RE = re.compile(FLATU_TS + r"flat settings warning: (?P<what>shown|changed|hidden)(?: \(mode=(?P<mode>\w+), frames refused for (?P<reason>[\w-]+)(?P<flags>.*?)\): (?P<words>.*))?$")
FLATU_REFUSAL_RE = re.compile(FLATU_TS + r"flat runtime refusal 5s: reason=(?P<reason>[\w-]+) count=(?P<count>\d+)")
FLATU_RUNTIME_RE = re.compile(FLATU_TS + r"flat runtime: treated=(?P<treated>\d+) refused=(?P<refused>\d+) last=(?P<last>[\w-]+)")
FLATU_TONE_REASONS = ("no-known-tone-pass", "invalid-tone-pass")
FLATU_SILENT_REASONS = ("no-3d-scene", "no-known-output-copy")
FLATU_OLD_ADVICE = ("Supersampling is below 1.0", ", supersampling below 1.0 (render ")


def parse_flat_upscale(text):
    """The log's flat-upscale lines: {keys: [{ts, key, when}], triggers: [index of the first-trigger line], windows: [{ts, kv, index}], first: {...} or None,
    declines: [{ts, why, wl, passes}], routes: [{ts, name, r, e, d}], stand: [{ts, what, reason, words, index}], warns: [{ts, what, reason, mode,
    flags, words, index}], refusals: {reason: count}, runtime: [{ts, treated, refused, last}], old_advice: bool, flat: bool}. A line cut short or
    garbled is skipped, never fatal."""
    f = {"keys": [], "triggers": [], "windows": [], "first": None, "declines": [], "routes": [], "stand": [], "warns": [], "refusals": {}, "runtime": [],
         "old_advice": False, "flat": False}
    for index, raw in enumerate(text.splitlines()):
        try:
            if any(token in raw for token in FLATU_OLD_ADVICE):
                f["old_advice"] = True
            m = FLATU_KEY_RE.match(raw)
            if m:
                f["flat"] = True
                f["keys"].append({"ts": m.group("ts") or "", "key": m.group("key"), "when": m.group("when")})
                continue
            m = FLATU_TRIGGER_RE.match(raw)
            if m:
                f["triggers"].append(index)
                continue
            m = FLATU_WINDOW_RE.match(raw)
            if m:
                f["windows"].append({"ts": m.group("ts") or "", "kv": _ckv(m.group("rest")), "index": index})
                continue
            m = FLATU_FIRST_RE.match(raw)
            if m:
                if f["first"] is None:
                    f["first"] = {"ts": m.group("ts") or "", "frame": int(m.group("frame")), "src": (int(m.group("w")), int(m.group("h"))),
                                  "scene": (int(m.group("sw")), int(m.group("sh"))), "output": (int(m.group("ow")), int(m.group("oh"))),
                                  "whitelist": m.group("wl"), "route": m.group("route"), "menu": bool(m.group("menu")), "index": index}
                continue
            m = FLATU_DECLINED_RE.match(raw)
            if m:
                f["declines"].append({"ts": m.group("ts") or "", "why": m.group("why"), "wl": m.group("wl"), "passes": int(m.group("passes")),
                                      "src": (int(m.group("w")), int(m.group("h")))})
                continue
            m = FLATU_ROUTE_RE.match(raw)
            if m:
                f["routes"].append({"ts": m.group("ts") or "", "name": m.group("name"), "r": (int(m.group("rw")), int(m.group("rh"))),
                                    "e": (int(m.group("ew")), int(m.group("eh"))), "d": (int(m.group("dw")), int(m.group("dh")))})
                continue
            m = FLATU_STAND_RE.match(raw)
            if m:
                what, rest = m.group("what"), m.group("rest")
                entry = {"ts": m.group("ts") or "", "what": what, "reason": None, "words": None, "index": index}
                sub = (FLATU_STAND_ENTERED_RE if what == "entered" else FLATU_STAND_STILL_RE if what == "still stood down" else None)
                sm = sub.match(rest) if sub else None
                if sm:
                    entry["reason"], entry["words"] = sm.group("reason"), sm.group("words")
                f["stand"].append(entry)
                continue
            m = FLATU_WARN_RE.match(raw)
            if m:
                f["warns"].append({"ts": m.group("ts") or "", "what": m.group("what"), "reason": m.group("reason"), "mode": m.group("mode"),
                                   "flags": m.group("flags") or "", "words": m.group("words") or "", "index": index})
                continue
            m = FLATU_REFUSAL_RE.match(raw)
            if m:
                f["refusals"][m.group("reason")] = f["refusals"].get(m.group("reason"), 0) + int(m.group("count"))
                continue
            m = FLATU_RUNTIME_RE.match(raw)
            if m:
                f["flat"] = True
                f["runtime"].append({"ts": m.group("ts") or "", "treated": int(m.group("treated")), "refused": int(m.group("refused")), "last": m.group("last")})
        except (ValueError, TypeError):
            continue
    return f


def flat_upscale_windows(f):
    """Each `flat copy structure 5s:` window's tokens as numbers: [{ts, key, copies, whitelist, admitted, declined, selector_refused, no_scene,
    render_size, route_serves, key_off, last, scene (w, h) or None, output (w, h) or None, source (w, h) or None, ldr_before_max, declines {why: n}}]."""
    out = []
    for w in f["windows"]:
        kv = w["kv"]

        def size(token):
            m = re.match(r"^(\d+)x(\d+)$", kv.get(token, ""))
            return (int(m.group(1)), int(m.group(2))) if m and int(m.group(1)) else None
        declines = {}
        text = kv.get("declines", "none")
        if text != "none":
            for part in text.split(","):
                key, sep, value = part.rpartition(":")
                if sep and value.isdigit():
                    declines[key] = int(value)
        out.append({"ts": w["ts"], "key": kv.get("key", "?"), "copies": _cint(kv.get("copies")) or 0, "whitelist": _cint(kv.get("whitelist")) or 0,
                    "admitted": _cint(kv.get("admitted")) or 0, "declined": _cint(kv.get("declined")) or 0,
                    "selector_refused": _cint(kv.get("selector-refused")) or 0, "no_scene": _cint(kv.get("no-scene")) or 0,
                    "render_size": _cint(kv.get("render-size")) or 0, "route_serves": _cint(kv.get("route-serves")) or 0,
                    "key_off": _cint(kv.get("key-off")) or 0, "last": kv.get("last", "?"), "scene": size("scene"), "output": size("output"),
                    "source": size("source"), "ldr_before_max": _cint(kv.get("ldr-passes-before-max")) or 0, "declines": declines})
    return out


def flat_upscale_verdict(f):
    """The verdict on one flight: [(tag, status, text)], status PASS, WARN, STOP or n/a (what the log cannot say). The tags are the questions the
    flight plan asks: KEY (is the admission on), ADMISSION (did it run, and what did it see), TREATED (did frames get treated), UPSCALE (below the
    output, by structure or by the whitelist), TONE REFUSALS (frames the whitelist refused for a tone pass and nothing admitted), STAND-DOWN (which
    reasons stood the work down), F8 WARNING (what the panel said, and the startup false warning), CHAIN (a game anti-aliasing chain the structure
    declined), ADVICE (the old supersampling advice must be gone)."""
    out = []

    def add(tag, status, text):
        out.append((tag, status, text))

    wins = flat_upscale_windows(f)
    total = {k: sum(w[k] for w in wins) for k in ("copies", "whitelist", "admitted", "declined", "selector_refused", "no_scene", "render_size", "route_serves", "key_off")}
    runtime = f["runtime"]
    treated = (runtime[-1]["treated"] - runtime[0]["treated"]) if len(runtime) > 1 else (runtime[-1]["treated"] if runtime else 0)
    # The AA rule's declines: the frames the windows counted (each cause is logged as a line once a session, so the lines are not a count of frames),
    # or, with no window that counted them, the decline lines.
    chain_lines = sum(1 for d in f["declines"] if "r-sized-image-passes" in d["why"])
    chain_frames = sum(n for w in wins for why, n in w["declines"].items() if "r-sized-image-passes" in why)
    passes_declines = chain_frames or chain_lines

    # KEY
    if f["keys"]:
        last = f["keys"][-1]
        if last["key"] == "auto":
            add("KEY", "PASS", "experimental.temporal_aa_before_post=auto (%s): the game's final copy is admitted by structure where the HDR route does not serve the frame" % last["when"])
        else:
            add("KEY", "WARN", "experimental.temporal_aa_before_post=%s (%s): the copy route is the whitelist alone; nothing is admitted by structure" % (last["key"], last["when"]))
    else:
        add("KEY", "n/a", "no `flat hdr route:` key line (a build that predates the route, or a session that never reached a Present)")

    # ADMISSION
    if not wins:
        add("ADMISSION", "STOP", "no `flat copy structure 5s:` window: the admission never ran (a build that predates section 83, or no temporal mode was selected)")
    else:
        add("ADMISSION", "PASS" if total["copies"] else "WARN", "%d window(s): copies %d, whitelist %d, admitted %d, declined %d, selector-refused %d, no-3d-scene %d, render-size %d, route-serves %d, key-off %d%s%s"
            % (len(wins), total["copies"], total["whitelist"], total["admitted"], total["declined"], total["selector_refused"], total["no_scene"], total["render_size"],
               total["route_serves"], total["key_off"],
               "" if total["copies"] else "; NO final copy was ruled on in any window: the admission ran and had nothing to say (a session that never drew a 3D frame, or a copy the reducer never reached)",
               "; first admission at frame %d (%dx%d image, scene %dx%d on %dx%d, route %s%s)" % (
                   f["first"]["frame"], f["first"]["src"][0], f["first"]["src"][1], f["first"]["scene"][0], f["first"]["scene"][1], f["first"]["output"][0],
                   f["first"]["output"][1], f["first"]["route"], ", the 3D menu" if f["first"]["menu"] else "") if f["first"] else "; no frame was admitted by structure"))

    # TREATED
    if not runtime:
        add("TREATED", "n/a", "no `flat runtime:` line")
    elif treated > 0:
        add("TREATED", "PASS", "%d frame(s) treated over the log (the counter went %d -> %d)" % (treated, runtime[0]["treated"], runtime[-1]["treated"]))
    else:
        add("TREATED", "STOP", "no frame was treated (the counter stayed at %d; last verdict %s)" % (runtime[-1]["treated"], runtime[-1]["last"]))

    # UPSCALE
    below = [r for r in f["routes"] if r["r"][0] < r["d"][0] or r["r"][1] < r["d"][1]]
    if not below:
        add("UPSCALE", "n/a", "no frame rendered below the output in this log (no `flat route:` line with R under D)")
    else:
        names = sorted({"%s R=%dx%d D=%dx%d" % (r["name"], r["r"][0], r["r"][1], r["d"][0], r["d"][1]) for r in below})
        if total["admitted"] > 0 and treated > 0:
            add("UPSCALE", "PASS", "below the output (%s): %d frame(s) admitted by structure, treated" % ("; ".join(names), total["admitted"]))
        elif total["whitelist"] > 0 and treated > 0:
            add("UPSCALE", "PASS", "below the output (%s): the whitelist selected %d frame(s), treated; the structure had nothing to do (every frame had a known tone pass)" % ("; ".join(names), total["whitelist"]))
        else:
            add("UPSCALE", "STOP", "below the output (%s) and nothing was admitted or selected, or nothing treated" % "; ".join(names))

    # TONE REFUSALS
    tone = {r: n for r, n in f["refusals"].items() if r in FLATU_TONE_REASONS}
    if not tone:
        add("TONE REFUSALS", "PASS", "no frame was refused for a tone pass")
    else:
        text = ", ".join("%s %d" % (r, n) for r, n in sorted(tone.items()))
        add("TONE REFUSALS", "WARN" if treated > 0 else "STOP", "%s frame(s) refused for a tone pass: %s" % (sum(tone.values()), text))

    # STAND-DOWN
    entered = [s for s in f["stand"] if s["what"] == "entered"]
    if not entered:
        add("STAND-DOWN", "PASS", "the work never stood down")
    else:
        worst, notes = "PASS", []
        for s in entered:
            reason = s["reason"] or "?"
            if reason in FLATU_SILENT_REASONS:
                status = "PASS"
            elif reason == "render-size-does-not-fit-output":
                status = "WARN"
            elif reason in FLATU_TONE_REASONS and passes_declines:
                status = "WARN"
            else:
                status = "STOP"
            notes.append("%s%s: %s%s" % (s["ts"] or "?", "", reason, " (%s)" % s["words"] if s["words"] else ""))
            worst = "STOP" if status == "STOP" or worst == "STOP" else ("WARN" if status == "WARN" or worst == "WARN" else "PASS")
        add("STAND-DOWN", worst, "%d stand-down(s): %s%s" % (len(entered), "; ".join(notes),
            "" if worst == "PASS" else " (no-3d-scene is a startup or a loading screen, silent; render-size-does-not-fit-output is Elite's resolution not being the screen's shape; a tone refusal "
                                            "with declines for R-sized passes is a game anti-aliasing chain; anything else is a chain nothing recognised)"))

    # F8 WARNING
    shown = [w for w in f["warns"] if w["what"] in ("shown", "changed")]
    if not shown:
        add("F8 WARNING", "PASS", "the panel showed no warning")
    else:
        first_scene = min([i for i in f["triggers"]] + ([f["first"]["index"]] if f["first"] else []) or [10 ** 9])
        worst, notes = "PASS", []
        for w in shown:
            reason = w["reason"] or "?"
            if w["index"] < first_scene and reason != "render-size-does-not-fit-output":
                status, why = "STOP", "BEFORE ANY SCENE: the false startup warning"
            elif reason == "render-size-does-not-fit-output":
                status, why = "WARN", "the render size"
            elif reason in FLATU_TONE_REASONS and passes_declines:
                status, why = "WARN", "a game anti-aliasing chain, Anti-aliasing advised"
            else:
                status, why = "STOP", "an unrecognised chain"
            notes.append("%s %s (%s): %s" % (w["ts"] or "?", reason, why, w["words"][:110]))
            worst = "STOP" if status == "STOP" or worst == "STOP" else ("WARN" if status == "WARN" or worst == "WARN" else "PASS")
        add("F8 WARNING", worst, "%d warning(s): %s" % (len(shown), " | ".join(notes)))

    # CHAIN
    if not wins:
        add("CHAIN", "n/a", "no window")
    elif passes_declines:
        longest = max([d["passes"] for d in f["declines"]] + [w["ldr_before_max"] for w in wins])
        add("CHAIN", "WARN", "the structure declined %s with R-sized image passes between the scene HDR's first consumer and the copy (the longest chain: %d): "
            "the game's anti-aliasing filter, which the structure leaves refused; turn Anti-aliasing off in Elite"
            % ("%d frame(s)" % chain_frames if chain_frames else "frames (%d decline line(s); no window counted them)" % chain_lines, longest))
    else:
        add("CHAIN", "PASS", "no R-sized image pass between the scene HDR's first consumer and the copy (the longest chain seen: %d)" % max([w["ldr_before_max"] for w in wins] + [0]))

    # ADVICE
    if f["old_advice"]:
        add("ADVICE", "STOP", "the old supersampling advice is in this log (\"Supersampling is below 1.0\"): a build from before section 83, or the advice is back")
    else:
        add("ADVICE", "PASS", "the supersampling advice is gone (below 1.0 is supported)")
    return out


def flat_upscale_summary(verdict):
    counts = {"PASS": 0, "WARN": 0, "STOP": 0, "n/a": 0}
    for _, status, _ in verdict:
        counts[status] = counts.get(status, 0) + 1
    worst = "STOP" if counts["STOP"] else ("WARN" if counts["WARN"] else ("PASS" if counts["PASS"] else "n/a"))
    return "flat upscale verdict: %s (%d PASS, %d WARN, %d STOP, %d n/a)" % (worst, counts["PASS"], counts["WARN"], counts["STOP"], counts["n/a"])


def print_flat_upscale(text):
    """The --flat-upscale report. Returns the process exit code: 0 when the log has any flat line, 1 when it has none (a VR log, or no flat session);
    the verdict never changes the exit code (read its lines)."""
    f = parse_flat_upscale(text)
    wins = flat_upscale_windows(f)
    if not (f["keys"] or wins or f["runtime"] or f["stand"] or f["warns"]):
        print("[edvr] no flat-profile line in this log (a VR session, or a build that predates the flat runtime).")
        return 1
    print("[edvr] flat upscale: %d key line(s), %d copy-structure window(s), %d route line(s), %d stand-down line(s), %d warning line(s), %d decline line(s)"
          % (len(f["keys"]), len(wins), len(f["routes"]), len(f["stand"]), len(f["warns"]), len(f["declines"])))
    for k in f["keys"]:
        print("key %s: experimental.temporal_aa_before_post=%s (%s)" % (k["ts"] or "?", k["key"], k["when"]))
    for r in f["routes"]:
        print("route %s: %s R=%dx%d E=%dx%d D=%dx%d" % (r["ts"] or "?", r["name"], r["r"][0], r["r"][1], r["e"][0], r["e"][1], r["d"][0], r["d"][1]))
    if f["first"]:
        fi = f["first"]
        print("first admission %s at frame %d: a %dx%d image, scene %dx%d on a %dx%d output, route %s%s; the whitelist said %s"
              % (fi["ts"] or "?", fi["frame"], fi["src"][0], fi["src"][1], fi["scene"][0], fi["scene"][1], fi["output"][0], fi["output"][1], fi["route"],
                 " (the 3D menu)" if fi["menu"] else "", fi["whitelist"]))
    for d in f["declines"]:
        print("declined %s: %s (the whitelist said %s; a %dx%d image, %d R-sized pass(es) between)" % (d["ts"] or "?", d["why"], d["wl"], d["src"][0], d["src"][1], d["passes"]))
    for w in wins:
        print("window %s: key=%s copies %d (whitelist %d, admitted %d, declined %d, selector-refused %d, no-3d-scene %d, render-size %d, route-serves %d, key-off %d); last %s; "
              "scene %s output %s source %s; longest chain %d%s"
              % (w["ts"] or "?", w["key"], w["copies"], w["whitelist"], w["admitted"], w["declined"], w["selector_refused"], w["no_scene"], w["render_size"], w["route_serves"],
                 w["key_off"], w["last"], "%dx%d" % w["scene"] if w["scene"] else "-", "%dx%d" % w["output"] if w["output"] else "-", "%dx%d" % w["source"] if w["source"] else "-",
                 w["ldr_before_max"], "; declines " + ", ".join("%s:%d" % kv for kv in sorted(w["declines"].items())) if w["declines"] else ""))
    for s in f["stand"]:
        print("stand-down %s: %s%s%s" % (s["ts"] or "?", s["what"], " for %s" % s["reason"] if s["reason"] else "", " (%s)" % s["words"] if s["words"] else ""))
    for w in f["warns"]:
        print("F8 warning %s: %s%s%s" % (w["ts"] or "?", w["what"], " for %s" % w["reason"] if w["reason"] else "", ": %s" % w["words"][:200] if w["words"] else ""))
    if f["refusals"]:
        print("refusals (summed from `flat runtime refusal 5s:`): " + ", ".join("%s %d" % kv for kv in sorted(f["refusals"].items(), key=lambda kv: -kv[1])))
    verdict = flat_upscale_verdict(f)
    for tag, status, text_ in verdict:
        print("%s (%s) %s" % (status, tag, text_))
    print(flat_upscale_summary(verdict))
    return 0


# ---------------------------------------------------------------------------------------------------------------------------------------
# --vr-supersampling: Elite's Supersampling below 1.0 in VR, read from the render sizes (design doc section 83, the VR warning). The lines are
# src/common/vr_supersample_notice.h's log line (once a session), vscreen.cpp's own adoption line it follows, and menu.cpp's note that the
# headset notice was queued. tools\vscreen_fit_test (R13) holds the formatter and the wiring; this reader's self-test builds the lines from
# the header's own text.
# ---------------------------------------------------------------------------------------------------------------------------------------
VRSS_RE = re.compile(FLATU_TS + r"vr supersampling: Elite draws the 3D world at (?P<rw>\d+)x(?P<rh>\d+), (?P<pct>\d+)% of the (?P<ew>\d+)x(?P<eh>\d+) eye texture, and scales it up before EDVR sees it: ")
VRSS_ADOPT_RE = re.compile(FLATU_TS + r"vScreen: the world on this rig is rendered at (?P<rw>\d+)x(?P<rh>\d+) and scaled into the (?P<ew>\d+)x(?P<eh>\d+) the headset is handed -- (?P<pct>\d+)% of the width")
VRSS_QUEUED_RE = re.compile(FLATU_TS + r"vr supersampling: (?:the headset notice is queued as a toast|menu\.toasts is off, so no toast)")
VRSS_BELOW_PERCENT = 98    # kBelowPercent in src/common/vr_supersample_notice.h


def vrss_below(r, e):
    """vrss::below: the render size under kBelowPercent of the eye's width AND height, an exact compare (the percent in the log is rounded,
    so 97.6% reads 98 there and is still below)."""
    return bool(r[0] and r[1] and e[0] and e[1] and r[0] * 100 < e[0] * VRSS_BELOW_PERCENT and r[1] * 100 < e[1] * VRSS_BELOW_PERCENT)


def parse_vr_supersampling(text):
    """{notice: {ts, r, pct, e} or None, adopt: {ts, r, e, pct} or None, queued: ts or None, toast: bool, flat: bool}."""
    f = {"notice": None, "adopt": None, "queued": None, "toast": False, "flat": False}
    for raw in text.splitlines():
        try:
            m = VRSS_RE.match(raw)
            if m:
                if f["notice"] is None:
                    f["notice"] = {"ts": m.group("ts") or "", "r": (int(m.group("rw")), int(m.group("rh"))), "pct": int(m.group("pct")), "e": (int(m.group("ew")), int(m.group("eh")))}
                continue
            m = VRSS_ADOPT_RE.match(raw)
            if m:
                if f["adopt"] is None:
                    f["adopt"] = {"ts": m.group("ts") or "", "r": (int(m.group("rw")), int(m.group("rh"))), "e": (int(m.group("ew")), int(m.group("eh"))), "pct": int(m.group("pct"))}
                continue
            m = VRSS_QUEUED_RE.match(raw)
            if m:
                if f["queued"] is None:
                    f["queued"] = m.group("ts") or "?"
                    f["toast"] = "queued as a toast" in raw
                continue
            if FLATU_RUNTIME_RE.match(raw) or FLATU_KEY_RE.match(raw):
                f["flat"] = True
        except (ValueError, TypeError):
            continue
    return f


def vr_supersampling_verdict(f):
    """[(tag, status, text)]: NOTICE (the log line, from the measured sizes), CONSISTENT (it agrees with vScreen's own adoption line), HEADSET (the
    toast was queued, and the Status page's hint has it while the menu is open), FLAT (a flat log never carries it)."""
    out = []

    def add(tag, status, text):
        out.append((tag, status, text))

    n, a = f["notice"], f["adopt"]
    if f["flat"] and (n or a or f["queued"]):
        add("FLAT", "STOP", "a flat-profile log carries the VR notice: it must never")
    elif f["flat"]:
        add("FLAT", "PASS", "a flat-profile log, and no VR notice in it")
    if n:
        if vrss_below(n["r"], n["e"]):
            add("NOTICE", "PASS", "the world is drawn at %dx%d, %d%% of the %dx%d eye texture: Elite's Supersampling is below 1 (or an upscaler sits in the chain)"
                % (n["r"][0], n["r"][1], n["pct"], n["e"][0], n["e"][1]))
        else:
            add("NOTICE", "STOP", "the notice names %dx%d against a %dx%d eye (%d%%), which is not below %d%% on both axes" % (n["r"][0], n["r"][1], n["e"][0], n["e"][1], n["pct"], VRSS_BELOW_PERCENT))
    elif a and vrss_below(a["r"], a["e"]):
        add("NOTICE", "STOP", "vScreen measured the world at %d%% of the eye (%dx%d in %dx%d) and no `vr supersampling:` line followed: the detection did not run" % (a["pct"], a["r"][0], a["r"][1], a["e"][0], a["e"][1]))
    elif a:
        add("NOTICE", "n/a", "vScreen measured the world at %d%% of the eye: not below %d%% on both axes, so no notice is right" % (a["pct"], VRSS_BELOW_PERCENT))
    else:
        add("NOTICE", "n/a", "vScreen adopted no render size: the world may be drawn at the eye's own size, or vScreen's guards held the adoption back (read its `vScreen:` lines), "
                             "or no scene was drawn yet. Elite's Supersampling below 1 cannot be ruled out from this log")
    if n and a:
        if n["r"] == a["r"] and n["e"] == a["e"]:
            add("CONSISTENT", "PASS", "the notice's sizes are vScreen's own adoption line's")
        else:
            add("CONSISTENT", "STOP", "the notice says %dx%d in %dx%d, vScreen's adoption line %dx%d in %dx%d" % (n["r"] + n["e"] + a["r"] + a["e"]))
    elif n:
        add("CONSISTENT", "WARN", "no vScreen adoption line to check the notice against")
    if n:
        if f["queued"]:
            add("HEADSET", "PASS", "%s (%s)" % ("the headset notice was queued as a toast" if f["toast"] else "menu.toasts is off: no toast", f["queued"]) + "; the Status page shows the advice as its hint while the menu is open")
        else:
            add("HEADSET", "WARN", "no `vr supersampling:` menu line: the headset notice was not queued (the menu may not have ticked yet)")
    return out


def print_vr_supersampling(text):
    """The --vr-supersampling report. Returns 0 when the log has any of the lines, 1 when it has none; the verdict never changes the exit code."""
    f = parse_vr_supersampling(text)
    if not (f["notice"] or f["adopt"] or f["queued"]):
        print("[edvr] no `vr supersampling:` or vScreen render-size line in this log (the world may be drawn at the eye's own size, or vScreen's guards held the "
              "adoption back, or this is a build that predates section 83: Supersampling below 1 cannot be ruled out from it).")
        return 1
    if f["adopt"]:
        a = f["adopt"]
        print("adoption %s: the world is drawn at %dx%d and scaled into the %dx%d the headset is handed (%d%% of the width)" % (a["ts"] or "?", a["r"][0], a["r"][1], a["e"][0], a["e"][1], a["pct"]))
    if f["notice"]:
        n = f["notice"]
        print("notice %s: %dx%d, %d%% of the %dx%d eye texture" % (n["ts"] or "?", n["r"][0], n["r"][1], n["pct"], n["e"][0], n["e"][1]))
    verdict = vr_supersampling_verdict(f)
    for tag, status, text_ in verdict:
        print("%s (%s) %s" % (status, tag, text_))
    counts = {"PASS": 0, "WARN": 0, "STOP": 0, "n/a": 0}
    for _, status, _ in verdict:
        counts[status] = counts.get(status, 0) + 1
    worst = "STOP" if counts["STOP"] else ("WARN" if counts["WARN"] else ("PASS" if counts["PASS"] else "n/a"))
    print("vr supersampling verdict: %s (%d PASS, %d WARN, %d STOP, %d n/a)" % (worst, counts["PASS"], counts["WARN"], counts["STOP"], counts["n/a"]))
    return 0


# --ui-composites: the interface composites the UI layer left in the scene (docs/ui-layer-2026-09-23.md, "2026-10-01: Disable GUI effects").
# The layer names an interface draw by its vertex shader; a draw into the lit HDR eye that samples an interface surface and has a vertex shader
# no family names gets no decision and no refusal line, so it stays in the scene, upscaled with it, and the log said nothing: user 5's cockpit
# panels (Elite's Disable GUI effects switches the holo panels to vs 1989E6D3B405FDE0 / ps EAB8A1C95A13FFBE) for a month. Every 30 s the layer
# prints ONE line, zeros included (src/d3d11/ui_scene_composites.h uiSceneCompositeText; tools\ui_composite_census_test holds the formatter and
# tools\ui_composites_fixture.log to each other byte for byte):
#   ui quality: composites left in the scene: <left> of <seen> composite draws (<rate> a frame) in <frames> frames (<live> live) -- <detail>.
# <detail> is `none: every interface composite drawn into an eye went into the layer`, `no draw into an eye sampled an interface surface in
# this window`, or the pairs left -- `vs <16 hex> ps <16 hex> (<family, not taken | no family>) <rate> a frame`, `;`-separated, then `<n> draws
# of pairs past the table's <cap> (<rate> a frame)`; or the line says NOT COUNTED when the interface depth pass was not running (no surface is
# learned, so no draw is a composite and a zero would mean nothing). A composite NO FAMILY names is the defect this reads for; one a family
# named and the layer did not take (the layer not armed for a frame, the world-screen gate holding the 2D screen) is also left in the scene and
# shows with its family, beside the reasons the layer's `left in the game's frame` line already gives. A log with the layer's 30 s lines and no
# composites line is a build from before this census, or a census that never ran: that is what the zeros are for.
# One pair is neither taken nor a defect: the engine's NULL-OUTPUT QUAD (vs B018D143700AB803 / ps 258B95AC99520C1F), which samples a leftover interface
# surface and writes nothing (docs/ui-layer-2026-09-23.md, 2026-10-01, "the composite the 15:09 flight left in the scene"). The census keeps it out of the
# count of composites left and says it in a clause of its own at the end of <detail>: `vs <16 hex> ps <16 hex> (<name>, kept in the scene by design) <n>
# draws, <rate> a frame`, after `none: ...` when nothing else is left. Builds before that (13c62cd6 to 069ebee4) counted it as a composite no family names,
# which this reader called a STOP on every loading screen: the kept clause ends that, and a log from one of those builds is read the same way (its pair is
# in UICOMP_KEPT_PAIRS, so its "no family" line is read as kept, with a note).
UICOMP_STAMP_RE = re.compile(r"^\[(?P<ts>\d\d:\d\d:\d\d\.\d{3})\]\s*(?P<msg>.*)$")
UICOMP_PREFIX = "ui quality: composites left in the scene: "
UICOMP_RE = re.compile(r"^ui quality: composites left in the scene: (?P<left>\d+) of (?P<seen>\d+) composite draws \((?P<rate>[0-9.]+) a frame\) "
                       r"in (?P<frames>\d+) frames \((?P<live>\d+) live\) -- (?P<detail>.*)\.$")
UICOMP_OFF_RE = re.compile(r"^ui quality: composites left in the scene: NOT COUNTED \((?P<why>.*)\) in (?P<frames>\d+) frames\.$")
UICOMP_PAIR_RE = re.compile(r"vs (?P<vs>[0-9A-F]{16}) ps (?P<ps>[0-9A-F]{16}) \((?P<family>[^)]*)\) (?P<rate>[0-9.]+) a frame")
UICOMP_KEPT_RE = re.compile(r"vs (?P<vs>[0-9A-F]{16}) ps (?P<ps>[0-9A-F]{16}) \((?P<name>[^)]*), kept in the scene by design\) (?P<n>\d+) draws, (?P<rate>[0-9.]+) a frame")
UICOMP_PAST_RE = re.compile(r"(?P<n>\d+) draws of pairs past the table's (?P<cap>\d+) \((?P<rate>[0-9.]+) a frame\)")
# Why each kept pair is not a defect, by the name the line gives it (the line says only "kept in the scene by design"; the reason lives here and in the doc).
UICOMP_KEPT_WHY = {
    "null-output quad": "its pixel shader writes zero and reads nothing, its blend is straight alpha and its depth and stencil are off, so it changes no pixel; the surface it "
                        "samples is a leftover binding, and ui depth excludes its vertex shader by hash, so the layer never takes it",
}
# The pairs the DLL keeps (src/d3d11/ui_scene_composites.h kUiSceneKeptPairs; the self-test holds this table to that one). A log from a build between 13c62cd6 and
# 069ebee4 has no kept clause and counts the pair as a composite left with no family: read as kept, with a note, so such a log does not STOP on a no-op.
UICOMP_KEPT_PAIRS = {("B018D143700AB803", "258B95AC99520C1F"): "null-output quad"}
UICOMP_NONE = "none: every interface composite drawn into an eye went into the layer"
UICOMP_IDLE = "no draw into an eye sampled an interface surface in this window"
UICOMP_NO_FAMILY = "no family"
UICOMP_LAYER_RE = re.compile(r"^ui quality: layer: \d+ s, \d+ frames, ")
# Draws a frame at or above which a composite left in the scene is the finding (one draw every other frame); below it a stray at a transition.
UICOMP_SUSTAINED = 0.5


def parse_ui_composites(text):
    """{windows: [{ts, t, kind: 'count' | 'off', left, seen, rate, frames, live, state: 'none' | 'idle' | 'left', pairs: [{vs, ps, family, rate}], kept: [{vs, ps,
    name, n, rate}], past: {n, cap, rate} or None}], layer_windows: the layer's own 30 s lines, unparsed: [raw lines that start with the prefix and are none of
    the shapes]}. `left` is the headline's count of composites left (the kept pair, a clause of its own, is not in it); `seen` includes the kept draws."""
    out = {"windows": [], "layer_windows": 0, "unparsed": []}
    for raw in text.splitlines():
        m = UICOMP_STAMP_RE.match(raw.rstrip("\r"))
        if not m:
            continue
        ts, msg = m.group("ts"), m.group("msg")
        t = _clock_s(ts)
        if UICOMP_LAYER_RE.match(msg):
            out["layer_windows"] += 1
            continue
        if not msg.startswith(UICOMP_PREFIX):
            continue
        off = UICOMP_OFF_RE.match(msg)
        if off:
            out["windows"].append({"ts": ts, "t": t, "kind": "off", "left": 0, "seen": 0, "rate": 0.0, "frames": int(off.group("frames")), "live": 0,
                                   "state": "off", "pairs": [], "kept": [], "past": None, "why": off.group("why")})
            continue
        c = UICOMP_RE.match(msg)
        if not c:
            out["unparsed"].append(raw)
            continue
        detail = c.group("detail")
        w = {"ts": ts, "t": t, "kind": "count", "left": int(c.group("left")), "seen": int(c.group("seen")), "rate": float(c.group("rate")),
             "frames": int(c.group("frames")), "live": int(c.group("live")), "state": "left", "pairs": [], "kept": [], "past": None}
        if detail == UICOMP_IDLE:
            w["state"] = "idle"
        else:
            items = detail.split("; ")
            if items[0] == UICOMP_NONE:
                w["state"] = "none"
                items = items[1:]
            for item in items:
                k = UICOMP_KEPT_RE.fullmatch(item)
                if k:
                    w["kept"].append({"vs": k.group("vs"), "ps": k.group("ps"), "name": k.group("name"), "n": int(k.group("n")), "rate": float(k.group("rate"))})
                    continue
                p = UICOMP_PAIR_RE.fullmatch(item)
                if p:
                    family = p.group("family")
                    w["pairs"].append({"vs": p.group("vs"), "ps": p.group("ps"), "family": UICOMP_NO_FAMILY if family == UICOMP_NO_FAMILY else family.replace(", not taken", ""),
                                       "rate": float(p.group("rate"))})
                    continue
                q = UICOMP_PAST_RE.fullmatch(item)
                if q:
                    w["past"] = {"n": int(q.group("n")), "cap": int(q.group("cap")), "rate": float(q.group("rate"))}
                    continue
                w["state"] = "unreadable"
        if w["state"] == "unreadable" or (w["state"] == "left" and not w["pairs"] and not w["past"]):
            out["unparsed"].append(raw)
            continue
        # An older build (13c62cd6 to 069ebee4) has no kept clause: it counted the null-output quad as a composite left with no family. Read it as kept. When it was the
        # only thing left the headline's own count is the draw count, exact; beside other pairs the draws are the rate over the frames (the line prints the rate to 0.01).
        old = [pr for pr in w["pairs"] if pr["family"] == UICOMP_NO_FAMILY and (pr["vs"], pr["ps"]) in UICOMP_KEPT_PAIRS]
        if old and w["state"] == "left":
            rest = [pr for pr in w["pairs"] if pr not in old]
            alone = not rest and not w["past"] and len(old) == 1
            for pr in old:
                w["kept"].append({"vs": pr["vs"], "ps": pr["ps"], "name": UICOMP_KEPT_PAIRS[(pr["vs"], pr["ps"])],
                                  "n": w["left"] if alone else int(round(pr["rate"] * w["frames"])), "rate": pr["rate"], "legacy": True})
            w["pairs"] = rest
            if alone:
                w["left"], w["rate"], w["state"] = 0, 0.0, "none"
        out["windows"].append(w)
    return out


def ui_composites_verdict(p):
    """[(tag, status, text)]: INSTRUMENT (the census ran: the line is there; zero lines beside the layer's own is a build before it or a census that never ran),
    DETECTOR (a window the interface depth pass was off), UNCLAIMED (a composite no family names, left in the scene: sustained is a STOP, a stray a WARN),
    NAMED (a family named it and it was not taken: a WARN, the layer's `left in the game's frame` line says why), OVERFLOW (more different pairs left than the
    table names), KEPT (a pair the scene keeps on purpose, the null-output quad: a PASS that says why it is not a defect), TAKEN (every composite drawn into an
    eye went into the layer) and SHAPE (a line the reader could not parse)."""
    out = []

    def add(tag, status, text):
        out.append((tag, status, text))

    ws = p["windows"]
    if p["unparsed"]:
        add("SHAPE", "WARN", "%d line(s) start with the census prefix and are none of its shapes (the formatter changed? read them): %s" % (len(p["unparsed"]), p["unparsed"][0][:160]))
    if not ws:
        if p["layer_windows"]:
            add("INSTRUMENT", "STOP", "the layer printed %d 30 s line(s) and not one `composites left in the scene` line: a build from before this census, or a census that never ran "
                                      "(check --expect-build; a current build prints it every window, zeros included)" % p["layer_windows"])
        else:
            add("INSTRUMENT", "n/a", "no `ui quality:` 30 s line at all: fix.ui_quality was off in this log (the census prints only while the layer does)")
        return out
    counted = [w for w in ws if w["kind"] == "count"]
    off = [w for w in ws if w["kind"] == "off"]
    add("INSTRUMENT", "PASS", "%d window(s) of the census line: %d counted, %d NOT COUNTED (zeros are printed, so a line saying 0 is the count having run)" % (len(ws), len(counted), len(off)))
    if off:
        add("DETECTOR", "WARN", "%d window(s) say NOT COUNTED (the interface depth pass was not running -- fix.temporal_aa off or stood down -- so no draw there could be "
                                "recognised as a composite); read the others" % len(off))
    # The pairs left, by (vs, ps, family) across the windows.
    seen_pairs = {}
    for w in counted:
        for pr in w["pairs"]:
            key = (pr["vs"], pr["ps"], pr["family"])
            e = seen_pairs.setdefault(key, {"windows": 0, "max": 0.0, "sum": 0.0, "first": w["ts"]})
            e["windows"] += 1
            e["max"] = max(e["max"], pr["rate"])
            e["sum"] += pr["rate"]
    unclaimed = {k: v for k, v in seen_pairs.items() if k[2] == UICOMP_NO_FAMILY}
    named = {k: v for k, v in seen_pairs.items() if k[2] != UICOMP_NO_FAMILY}
    if unclaimed:
        for (vs, ps, _), e in sorted(unclaimed.items(), key=lambda kv: -kv[1]["max"]):
            text_ = ("vs %s ps %s: left in the scene in %d of %d counted window(s), up to %.2f draws a frame (mean %.2f), first at %s; no family names it, so the layer "
                     "never decides it and the scene upscales it with everything else" % (vs, ps, e["windows"], len(counted), e["max"], e["sum"] / e["windows"], e["first"]))
            add("UNCLAIMED", "STOP" if e["max"] >= UICOMP_SUSTAINED else "WARN", text_)
    if named:
        for (vs, ps, family), e in sorted(named.items(), key=lambda kv: -kv[1]["max"]):
            add("NAMED", "WARN", "vs %s ps %s (%s): named and not taken in %d window(s), up to %.2f draws a frame; the layer's `left in the game's frame` line gives the reason "
                                 "(not armed, a world-screen gate, another fix swallowing it)" % (vs, ps, family, e["windows"], e["max"]))
    past = [w for w in counted if w["past"]]
    if past:
        worst = max(w["past"]["rate"] for w in past)
        add("OVERFLOW", "STOP" if worst >= UICOMP_SUSTAINED else "WARN",
            "%d window(s) left draws of more different pairs than the table names (%d), up to %.2f draws a frame unnamed" % (len(past), past[0]["past"]["cap"], worst))
    # The pairs the scene keeps on purpose (a clause of their own, never in the count of composites left): said, with why, and never a finding.
    kept_pairs = {}
    for w in counted:
        for k in w["kept"]:
            e = kept_pairs.setdefault((k["vs"], k["ps"], k["name"]), {"windows": 0, "max": 0.0, "draws": 0, "first": w["ts"], "legacy": 0})
            e["windows"] += 1
            e["max"] = max(e["max"], k["rate"])
            e["draws"] += k["n"]
            e["legacy"] += 1 if k.get("legacy") else 0
    for (vs, ps, name), e in sorted(kept_pairs.items(), key=lambda kv: -kv[1]["max"]):
        add("KEPT", "PASS", "vs %s ps %s (%s): kept in the scene by design in %d of %d counted window(s), up to %.2f draws a frame (%d draws in all), first at %s; %s%s" % (
            vs, ps, name, e["windows"], len(counted), e["max"], e["draws"], e["first"],
            UICOMP_KEPT_WHY.get(name, "the census names it as not interface, with a pair of its own"),
            "; %d of those window(s) are from a build whose census counted it as a composite left with no family (13c62cd6 to 069ebee4), read here as kept" % e["legacy"]
            if e["legacy"] else ""))
    leftover = [w for w in counted if w["left"]]
    if counted and not leftover:
        kept_draws = sum(k["n"] for w in counted for k in w["kept"])
        total = sum(w["seen"] for w in counted) - kept_draws
        if total > 0:
            add("TAKEN", "PASS", "every interface composite drawn into an eye went into the layer: %d composite draws in %d window(s), none left in the scene%s" % (
                total, len(counted), " (%d more draws are the kept pair's, above, and are not interface)" % kept_draws if kept_draws else ""))
        else:
            add("TAKEN", "WARN", "no composite was drawn into an eye in any counted window%s (a loading screen, or a log without a cockpit or a menu): nothing here shows the pair taken" % (
                " but the kept pair's (%d draws, above)" % kept_draws if kept_draws else ""))
    return out


def print_ui_composites(text):
    """The --ui-composites report. Exit 0 for PASS or WARN, 1 for STOP, 3 when the log has no census line (a build before it, the layer off, or a census that never ran)."""
    p = parse_ui_composites(text)
    verdict = ui_composites_verdict(p)
    ws = p["windows"]
    for w in ws:
        if w["kind"] == "off":
            print("%s  NOT COUNTED in %d frames" % (w["ts"], w["frames"]))
            continue
        if w["state"] == "idle":
            what = "no composite drawn"
        else:
            parts = ["none"] if w["state"] == "none" else []
            parts += ["%s/%s (%s) %.2f" % (pr["vs"], pr["ps"], pr["family"], pr["rate"]) for pr in w["pairs"]]
            if w["past"]:
                parts.append("%d past the table (%.2f)" % (w["past"]["n"], w["past"]["rate"]))
            parts += ["%s/%s (%s, kept by design) %d draws, %.2f" % (k["vs"], k["ps"], k["name"], k["n"], k["rate"]) for k in w["kept"]]
            what = "; ".join(parts)
        print("%s  %d of %d composite draws (%.2f a frame) in %d frames (%d live)  %s" % (w["ts"], w["left"], w["seen"], w["rate"], w["frames"], w["live"], what))
    for tag, status, text_ in verdict:
        print("%s (%s) %s" % (status, tag, text_))
    counts = {"PASS": 0, "WARN": 0, "STOP": 0, "n/a": 0}
    for _, status, _ in verdict:
        counts[status] = counts.get(status, 0) + 1
    worst = "STOP" if counts["STOP"] else ("WARN" if counts["WARN"] else ("PASS" if counts["PASS"] else "n/a"))
    print("ui-composites verdict: %s (%d PASS, %d WARN, %d STOP, %d n/a)" % (worst, counts["PASS"], counts["WARN"], counts["STOP"], counts["n/a"]))
    if not ws:
        return 3
    return 1 if counts["STOP"] else 0


# ---------------------------------------------------------------------------------------------------------------------------------------
# --terrain-checkerboard: Elite's terrain checkerboard rendering in VR, read from the game's active graphics preset on a worker thread (design
# doc section 84, the VR hint). The lines are src/common/terrain_checkerboard_notice.h's: the worker's start line, one line for the first read
# and one for each change (ON, OFF or unknown with its reason), the bound's line, a failed start or a fault, and the menu's note that the
# headset notice was queued as a toast, once per raise. tools\terrain_checkerboard_test holds the formatter, the reader and the wiring, and
# runs this reader over the log the real worker wrote; this reader's self-test builds its lines from the header's own text.
#
# A log WITHOUT the lines means the reader never started: a flat session, a build from before section 84, or a VR frame boundary that never
# reached the menu's tick. A read that found the option off, or found nothing, writes a line too (OFF, unknown), so no line is never "read, off".
# ---------------------------------------------------------------------------------------------------------------------------------------
TCB_START_RE = re.compile(FLATU_TS + r"vr terrain checkerboard: reading Elite's graphics settings on its own thread \((?P<tid>\d+)\), every (?P<secs>\d+) s, off the render thread\.")
TCB_FAILED_RE = re.compile(FLATU_TS + r"vr terrain checkerboard: could not start the reader thread")
TCB_READ_RE = re.compile(FLATU_TS + r"vr terrain checkerboard: (?P<state>ON|OFF|unknown) \((?P<detail>.*)\)(?:: (?P<tail>.*))?$")
TCB_DETAIL_RE = re.compile(r"^preset (?P<preset>.*?), (?P<source>[^,]+?): TerrainCheckerboardRenderingEnabled=(?P<value>.*)$")
TCB_LIMIT_RE = re.compile(FLATU_TS + r"vr terrain checkerboard: (?P<n>\d+) lines logged; further changes")
TCB_FAULT_RE = re.compile(FLATU_TS + r"vr terrain checkerboard: the reader faulted repeatedly")
TCB_QUEUED_RE = re.compile(FLATU_TS + r"vr terrain checkerboard: (?:the headset notice is queued as a toast|menu\.toasts is off, so no toast)")
TCB_LOG_MAX = 16    # kLogMax in src/common/terrain_checkerboard_notice.h


def parse_terrain_checkerboard(text):
    """{start: {ts, tid, secs} or None, failed: ts or None, reads: [{ts, state, preset, source, value, reason, tail}], limit: ts or None, fault: ts or None,
    queued: [{ts, toast}], flat: bool}: the lines in the order the log has them."""
    f = {"start": None, "failed": None, "reads": [], "limit": None, "fault": None, "queued": [], "flat": False}
    for raw in text.splitlines():
        try:
            m = TCB_READ_RE.match(raw)
            if m:
                state = m.group("state")
                d = TCB_DETAIL_RE.match(m.group("detail")) if state != "unknown" else None
                f["reads"].append({"ts": m.group("ts") or "", "state": state, "tail": m.group("tail") or "",
                                   "preset": d.group("preset") if d else "", "source": d.group("source") if d else "", "value": d.group("value") if d else "",
                                   "reason": m.group("detail") if state == "unknown" else ""})
                continue
            m = TCB_START_RE.match(raw)
            if m:
                if f["start"] is None:
                    f["start"] = {"ts": m.group("ts") or "", "tid": int(m.group("tid")), "secs": int(m.group("secs"))}
                continue
            m = TCB_FAILED_RE.match(raw)
            if m:
                if f["failed"] is None:
                    f["failed"] = m.group("ts") or "?"
                continue
            m = TCB_LIMIT_RE.match(raw)
            if m:
                if f["limit"] is None:
                    f["limit"] = m.group("ts") or "?"
                continue
            m = TCB_FAULT_RE.match(raw)
            if m:
                if f["fault"] is None:
                    f["fault"] = m.group("ts") or "?"
                continue
            m = TCB_QUEUED_RE.match(raw)
            if m:
                f["queued"].append({"ts": m.group("ts") or "?", "toast": "queued as a toast" in raw})
                continue
            if FLATU_RUNTIME_RE.match(raw) or FLATU_KEY_RE.match(raw):
                f["flat"] = True
        except (ValueError, TypeError):
            continue
    return f


def terrain_checkerboard_raises(reads):
    """How many times the option RAISED: an ON read whose predecessor was not ON (the first read counts). Each one is a new published version in the
    state On, which is what queues one toast; an ON read after an ON read is a change of preset or file with the option still on, not a raise."""
    return sum(1 for i, r in enumerate(reads) if r["state"] == "ON" and (i == 0 or reads[i - 1]["state"] != "ON"))


def tcb_verdict_reader(f, add):
    """FLAT (a flat log never carries the lines) and READER (the worker started, and read): the first two questions about any such log."""
    reads = f["reads"]
    anything = f["start"] or f["failed"] or reads or f["limit"] or f["fault"] or f["queued"]
    if f["flat"] and anything:
        add("FLAT", "STOP", "a flat-profile log carries the VR terrain checkerboard lines: it must never")
    elif f["flat"]:
        add("FLAT", "PASS", "a flat-profile log, and no terrain checkerboard line in it")
    if f["failed"]:
        add("READER", "STOP", "the reader thread could not be started (%s): the option was not read, so no notice can have been raised" % f["failed"])
    elif f["fault"]:
        add("READER", "STOP", "the reader faulted repeatedly and stopped (%s): the notice kept the last state it published" % f["fault"])
    elif f["start"] and reads:
        add("READER", "PASS", "the worker started (%s, thread %d, every %d s) and read %d time(s) logged, the first at %s"
            % (f["start"]["ts"] or "?", f["start"]["tid"], f["start"]["secs"], len(reads), reads[0]["ts"] or "?"))
    elif f["start"]:
        add("READER", "STOP", "the worker started (%s) and no read line followed: it died, hung or was never given a pass" % (f["start"]["ts"] or "?"))
    elif reads:
        add("READER", "WARN", "read lines but no start line (a log cut at the front?)")


def tcb_verdict_notice(f, add):
    """NOTICE (what the last read says), CHANGES (the reads in order) and LIMIT (the log's bound was reached)."""
    reads = f["reads"]
    if reads:
        last = reads[-1]
        if last["state"] == "ON":
            add("NOTICE", "PASS", "terrain checkerboard rendering is ON (preset %s, %s, =%s): distant terrain shimmers in VR with DLSS, and the notice is due"
                % (last["preset"] or "?", last["source"] or "?", last["value"] or "?"))
        elif last["state"] == "OFF":
            add("NOTICE", "n/a", "terrain checkerboard rendering is OFF (preset %s, %s, =%s): not the cause of any distant terrain shimmer in this log, and no notice is right"
                % (last["preset"] or "?", last["source"] or "?", last["value"] or "?"))
        else:
            add("NOTICE", "WARN", "terrain checkerboard rendering is unknown (%s): it cannot be ruled in or out from this log" % (last["reason"] or "no reason"))
    if len(reads) > 1:
        add("CHANGES", "PASS", "%d read line(s) in the order they were logged: %s" % (len(reads), ", ".join("%s %s" % (r["ts"] or "?", r["state"]) for r in reads)))
    if f["limit"]:
        add("LIMIT", "WARN", "the bound of %d logged reads was reached at %s: later changes are not in this log" % (TCB_LOG_MAX, f["limit"]))


def tcb_verdict_headset(f, add):
    """HEADSET: one queued line per RAISE of the option (a raise is an ON read after a read that was not ON)."""
    raises = terrain_checkerboard_raises(f["reads"])
    toasts = len(f["queued"])
    if not (raises or toasts):
        return
    what = "the headset notice was queued as a toast" if all(q["toast"] for q in f["queued"]) else "queued (menu.toasts off for some: no toast)"
    if f["limit"]:
        add("HEADSET", "WARN", "%d queued line(s), but reads past the bound are not logged, so the raises cannot be counted" % toasts)
    elif toasts == raises:
        add("HEADSET", "PASS", "%d raise(s) of the option, %d queued line(s) (%s): once per raise; the Status page shows the advice as its hint while the menu is open" % (raises, toasts, what))
    elif toasts == 0:
        add("HEADSET", "WARN", "no `vr terrain checkerboard:` menu line: the headset notice was not queued (the menu may not have ticked yet)")
    elif toasts < raises:
        add("HEADSET", "WARN", "%d raise(s) of the option but %d queued line(s): a toast was not said" % (raises, toasts))
    else:
        add("HEADSET", "STOP", "%d queued line(s) for %d raise(s) of the option: a raise was said more than once" % (toasts, raises))


def terrain_checkerboard_verdict(f):
    """[(tag, status, text)]: FLAT, READER, NOTICE, CHANGES, LIMIT, HEADSET (see the three functions above)."""
    out = []
    add = lambda tag, status, text: out.append((tag, status, text))
    tcb_verdict_reader(f, add)
    tcb_verdict_notice(f, add)
    tcb_verdict_headset(f, add)
    return out


def print_terrain_checkerboard(text):
    """The --terrain-checkerboard report. Returns 0 when the log has any of the lines, 1 when it has none; the verdict never changes the exit code."""
    f = parse_terrain_checkerboard(text)
    if not (f["start"] or f["failed"] or f["reads"] or f["limit"] or f["fault"] or f["queued"]):
        print("[edvr] no `vr terrain checkerboard:` line in this log: the reader never started (a flat session, a build from before section 84, or a VR frame "
              "boundary that never reached the menu's tick), so Elite's terrain checkerboard rendering cannot be ruled in or out from it. A read that found the "
              "option off, or found nothing, writes a line too: no line is not \"read, off\".")
        return 1
    if f["start"]:
        print("worker %s: reading Elite's graphics settings on its own thread (%d), every %d s" % (f["start"]["ts"] or "?", f["start"]["tid"], f["start"]["secs"]))
    for r in f["reads"]:
        if r["state"] == "unknown":
            print("read %s: unknown (%s)" % (r["ts"] or "?", r["reason"]))
        else:
            print("read %s: %s in preset %s (%s), TerrainCheckerboardRenderingEnabled=%s" % (r["ts"] or "?", r["state"], r["preset"] or "?", r["source"] or "?", r["value"] or "?"))
    for q in f["queued"]:
        print("queued %s: %s" % (q["ts"], "as a toast" if q["toast"] else "no toast (menu.toasts is off)"))
    verdict = terrain_checkerboard_verdict(f)
    for tag, status, text_ in verdict:
        print("%s (%s) %s" % (status, tag, text_))
    counts = {"PASS": 0, "WARN": 0, "STOP": 0, "n/a": 0}
    for _, status, _ in verdict:
        counts[status] = counts.get(status, 0) + 1
    worst = "STOP" if counts["STOP"] else ("WARN" if counts["WARN"] else ("PASS" if counts["PASS"] else "n/a"))
    print("vr terrain checkerboard verdict: %s (%d PASS, %d WARN, %d STOP, %d n/a)" % (worst, counts["PASS"], counts["WARN"], counts["STOP"], counts["n/a"]))
    return 0


def self_test_terrain_checkerboard():
    """--terrain-checkerboard on lines built from src/common/terrain_checkerboard_notice.h's own text: every format the reader parses is checked against
    the header's string literals, then a good flight (read ON, one toast, then OFF) and logs altered to break each thing the verdict judges. Returns ok."""
    import contextlib
    import io
    ok = True

    def fail(msg):
        nonlocal ok
        print("terrain checkerboard: %s" % msg)
        ok = False

    def report(text):
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            rc = print_terrain_checkerboard(text)
        return rc, buf.getvalue()

    def statuses(text):
        _, out = report(text)
        found = {}
        for row in out.splitlines():
            m = re.match(r"^(PASS|WARN|STOP|n/a) \(([A-Z0-9 -]+)\) ", row)
            if m:
                found[m.group(2)] = m.group(1)
        return found, out

    def want(text, wanted, label, absent=()):
        got, out = statuses(text)
        for tag, status in wanted.items():
            if got.get(tag) != status:
                fail("%s: %s is %r, wanted %r:\n%s" % (label, tag, got.get(tag), status, out))
        for tag in absent:
            if tag in got:
                fail("%s: %s should not be judged, and is %r:\n%s" % (label, tag, got.get(tag), out))

    def literals(source):
        """The C string literals of a source text, adjacent ones joined the way the compiler does, with \\\" and \\\\ undone."""
        out = []
        for run in re.finditer(r'(?:"(?:[^"\\\n]|\\.)*"[ \t\r\n]*)+', source):
            joined = "".join(re.findall(r'"((?:[^"\\\n]|\\.)*)"', run.group(0)))
            out.append(re.sub(r"\\(.)", lambda m: {"n": "\n", "t": "\t"}.get(m.group(1), m.group(1)), joined))
        return out

    header = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "src", "common", "terrain_checkerboard_notice.h")
    if not os.path.isfile(header):
        fail("src\\common\\terrain_checkerboard_notice.h is not where the self-test looks for it (%s)" % header)
        return False
    source = read_text(header)
    have = set(literals(source))
    fmt_on = "vr terrain checkerboard: ON (preset %s, %s: TerrainCheckerboardRenderingEnabled=%s): %s"
    fmt_off = "vr terrain checkerboard: OFF (preset %s, %s: TerrainCheckerboardRenderingEnabled=%s): no notice."
    fmt_unknown = "vr terrain checkerboard: unknown (%s): no notice."
    fmt_start = "vr terrain checkerboard: reading Elite's graphics settings on its own thread (%lu), every %u s, off the render thread."
    fmt_failed = "vr terrain checkerboard: could not start the reader thread, so Elite's terrain checkerboard rendering is not being read: no notice."
    fmt_fault = "vr terrain checkerboard: the reader faulted repeatedly and has stopped; the notice keeps the last state it published."
    fmt_limit = "vr terrain checkerboard: %d lines logged; further changes are followed by the notice but no longer logged."
    fmt_queued = 'vr terrain checkerboard: the headset notice is queued as a toast ("%s"); the Status page shows the advice as its hint while the menu is open.'
    fmt_notoast = "vr terrain checkerboard: menu.toasts is off, so no toast; the Status page shows the advice as its hint while the menu is open."
    sentence = "Elite's terrain checkerboard rendering makes distant terrain shimmer with DLSS. Turn it off in Elite's graphics options."
    toast = "Distant terrain shimmers: turn off terrain checkerboard"
    for text in (fmt_on, fmt_off, fmt_unknown, fmt_start, fmt_failed, fmt_fault, fmt_limit, fmt_queued, fmt_notoast, sentence, toast):
        if text not in have:
            fail("src\\common\\terrain_checkerboard_notice.h has no string literal %r: this reader parses text the DLL no longer writes" % text)
    if "(" in sentence or ")" in sentence:
        fail("the sentence has a parenthesis, which the reader takes as the end of the detail")
    k = re.search(r"constexpr int kLogMax = (\d+);", source)
    if not k or int(k.group(1)) != TCB_LOG_MAX:
        fail("this reader's TCB_LOG_MAX (%d) is not the header's kLogMax (%s)" % (TCB_LOG_MAX, k.group(1) if k else "missing"))

    def row(ts, text):
        return "[%s] %s\n" % (ts, text)

    version = "[09:29:00.000] version v0.18.0-rc.5 (build 5EC0DE01) -- this DLL was linked 2026-10-01 20:05:44 UTC\n"
    start = row("09:29:10.100", fmt_start % (4242, 3))
    on = row("09:29:10.102", fmt_on % ("VRHigh", "OptionDefaults\\VRHigh.fxcfg", "true", sentence))
    queued = row("09:29:10.300", fmt_queued % toast)
    off = row("09:31:00.500", fmt_off % ("Custom", "Custom.4.4.fxcfg", "false"))
    on2 = row("09:33:00.500", fmt_on % ("Custom", "Custom.4.4.fxcfg", "True", sentence))
    queued2 = row("09:33:00.700", fmt_queued % toast)
    unknown = row("09:35:00.000", fmt_unknown % "preset Custom but the folder has no Custom.<major>.<minor>.fxcfg")
    good = version + start + on + queued

    # ---- the parser, on a good flight ----
    p = parse_terrain_checkerboard(good)
    r0 = p["reads"][0] if p["reads"] else {}
    if not p["start"] or (p["start"]["tid"], p["start"]["secs"]) != (4242, 3) or len(p["reads"]) != 1 or \
            (r0.get("state"), r0.get("preset"), r0.get("source"), r0.get("value")) != ("ON", "VRHigh", "OptionDefaults\\VRHigh.fxcfg", "true") or \
            len(p["queued"]) != 1 or not p["queued"][0]["toast"] or p["flat"] or p["limit"] or p["fault"] or p["failed"]:
        fail("the good flight parsed as %r" % (p,))
    g = parse_terrain_checkerboard("vr terrain checkerboard: ON (\nvr terrain checkerboard: reading Elite's graphics settings on its own thread (x)\nnothing\n")
    if g["reads"] or g["start"]:
        fail("a cut-short line was mis-parsed: %r" % (g,))

    # ---- the report on a good flight ----
    rc, out = report(good)
    flat = re.sub(r"[ ]+", " ", out)
    for needle in ("worker 09:29:10.100: reading Elite's graphics settings on its own thread (4242), every 3 s",
                   "read 09:29:10.102: ON in preset VRHigh (OptionDefaults\\VRHigh.fxcfg), TerrainCheckerboardRenderingEnabled=true",
                   "queued 09:29:10.300: as a toast",
                   "PASS (READER) the worker started (09:29:10.100, thread 4242, every 3 s) and read 1 time(s) logged, the first at 09:29:10.102",
                   "PASS (NOTICE) terrain checkerboard rendering is ON (preset VRHigh, OptionDefaults\\VRHigh.fxcfg, =true)",
                   "PASS (HEADSET) 1 raise(s) of the option, 1 queued line(s) (the headset notice was queued as a toast): once per raise",
                   "vr terrain checkerboard verdict: PASS (3 PASS, 0 WARN, 0 STOP, 0 n/a)"):
        if needle not in flat:
            fail("the good flight's report lacks %r:\n%s" % (needle, out))
    if rc != 0:
        fail("the good flight reported exit %d" % rc)
    want(good, {"READER": "PASS", "NOTICE": "PASS", "HEADSET": "PASS"}, "the good flight", absent=("CHANGES", "LIMIT", "FLAT"))

    # ---- what the last read says, and the changes ----
    want(good + off, {"NOTICE": "n/a", "CHANGES": "PASS", "HEADSET": "PASS"}, "turned off after the toast")
    want(good + off + on2 + queued2, {"NOTICE": "PASS", "CHANGES": "PASS", "HEADSET": "PASS"}, "off and on again: two raises, two toasts")
    want(good + off + on2, {"HEADSET": "WARN"}, "a second raise with no second toast")
    want(good + row("09:30:00.000", fmt_on % ("VRUltra", "OptionDefaults\\VRUltra.fxcfg", "true", sentence)), {"HEADSET": "PASS", "CHANGES": "PASS"}, "a preset change with the option still on is no raise")
    want(good + off + unknown + on2 + queued2, {"HEADSET": "PASS", "NOTICE": "PASS"}, "unknown between two raises is also a re-arm")
    want(version + start + unknown, {"READER": "PASS", "NOTICE": "WARN"}, "an unknown read", absent=("HEADSET",))
    want(version + start + off, {"READER": "PASS", "NOTICE": "n/a"}, "a read that found it off", absent=("HEADSET",))
    _, out = statuses(version + start + unknown)
    if "unknown (preset Custom but the folder has no Custom.<major>.<minor>.fxcfg)" not in out:
        fail("an unknown read's reason is not in the report:\n%s" % out)

    # ---- the headset line ----
    want(version + start + on, {"HEADSET": "WARN"}, "no menu line")
    want(good + queued, {"HEADSET": "STOP"}, "one raise said twice")
    want(version + start + off + queued, {"HEADSET": "STOP"}, "a toast with no raise")
    want(version + start + on + row("09:29:10.300", fmt_notoast), {"HEADSET": "PASS"}, "menu.toasts off: no toast, still once")
    _, out = statuses(version + start + on + row("09:29:10.300", fmt_notoast))
    if "queued (menu.toasts off for some: no toast)" not in out:
        fail("a log with menu.toasts off should say so:\n%s" % out)

    # ---- the reader: started, read, failed, faulted, bounded ----
    want(version + start, {"READER": "STOP"}, "started and never read")
    want(version + on, {"READER": "WARN"}, "reads without the start line")
    want(version + row("09:29:10.100", fmt_failed), {"READER": "STOP"}, "a thread that could not start")
    want(good + row("09:40:00.000", fmt_fault), {"READER": "STOP"}, "a reader that faulted")
    want(good + row("09:40:00.000", fmt_limit % TCB_LOG_MAX), {"LIMIT": "WARN", "HEADSET": "WARN"}, "the log's bound")

    # ---- flat, and no lines at all ----
    flat_run = "[09:31:00.000] flat runtime: treated=1 refused=0 last=treated-jittered\n"
    want(good + flat_run, {"FLAT": "STOP"}, "a flat log carrying the VR lines")
    for text, label in ((version, "an empty log"), (version + flat_run, "a flat log with no lines"), (version + "[09:29:10.100] vr supersampling: something\n", "another notice's lines")):
        rc, out = report(text)
        if rc != 1 or "no `vr terrain checkerboard:` line" not in out or "not \"read, off\"" not in out:
            fail("%s should exit 1 and say that no line is not \"read, off\": rc=%d %r" % (label, rc, out))
    return ok


# --route-curve: the curved VR world route (docs/design-flat-temporal-aa-2026-09-23.md, section 82, "The curved route").
# With fix.panel_curvature above 0 the route's layer re-issues the screen through the very strip the game's own draw is substituted with
# (src/d3d11/panel_curve.cpp panelCurveReissue), and the route's log says so: its 5 s line (src/d3d11/vr_world_route_math.h
# vrWorldFormatWindow) carries `curve=` (off, pending, stood-down, or curvature/columns/gain) and `curve-reissues=` (the strips the layer
# drew in the window), its OWNS line (vrWorldFormatEntered) ends with a sentence holding `(curve=<text>)` when the screen is set to curve,
# panel_curve.cpp writes its own `panel curvature:` notes, and the layer's `vr world route layer:` line (ui_layer.cpp logWorldRoute) counts
# the route's refusals by reason. One flight log in, one answer out: did the curve and the route work together.
#
# The 5 s line is read by key=value TOKENS (_ckv), never by position: a later build drops the steady-detail and jitter keys and the tokens
# around them. Both counters are the window's own and are read at the same boundary, which is why a healthy window has them equal. The exit
# code carries the verdict, as --maps-sharp's does: 0 for PASS or WARN, 1 for STOP, 3 when the log has no route line at all (main() answers
# 2 for a wrong build before this runs). self_test_route_curve builds its logs from the formatter-held lines of tools\camera_census_fixture.log
# and pins every token it swaps, so a formatter that moves a token fails the self-test, not a flight.

ROUTE_CURVE_STAMP_RE = re.compile(r"^\[(?P<ts>\d\d:\d\d:\d\d\.\d{3})\]\s*(?P<msg>.*)$")
# What `curve=` says: off (the game's own quad is drawn), pending (asked for, strip not built yet), stood-down (a fault turned it off for the
# session), else the strip in hand as curvature/columns/gain (vrWorldFormatCurve: "%.3f/%d/%.3f").
ROUTE_CURVE_WORDS = ("off", "pending", "stood-down")
ROUTE_CURVE_STRIP_RE = re.compile(r"^-?\d+(?:\.\d+)?/\d+/-?\d+(?:\.\d+)?$")
ROUTE_CURVE_OWNS_RE = re.compile(r"vr world route: OWNS the world from frame=(?P<frame>\d+) ")
# The OWNS line keeps `(curve=<text>)` in whichever sentence it adds for a screen set to curve; a reader keys on that, never on the words.
ROUTE_CURVE_NAMED_RE = re.compile(r"\(curve=(?P<curve>[^)]*)\)")
ROUTE_CURVE_LAYER_RE = re.compile(r"^vr world route layer: (?P<secs>[0-9.]+) s; \d+ screen draws re-issued into the layer .*?"
                                  r"refused by the route's own checks: (?P<checks>.*?); refused by the decision's tests")
ROUTE_CURVE_CHECK_RE = re.compile(r"(?P<why>[a-z][a-z0-9-]*)=(?P<n>\d+)")
ROUTE_CURVE_NOT_TAKEN = "vr world route: layer did not take the screen draw for "
# The refusal the build BEFORE the curved route wrote when the screen was curved (ui_layer_math.h kCurved, deleted since): its short key in
# the layer's 30 s line, and its long text in the first-eight `layer did not take the screen draw` lines. Only a stale build writes either.
ROUTE_CURVE_STALE_KEY = "curved-screen"
ROUTE_CURVE_STALE_TEXT = "the screen is curved (panel_curvature)"
# `panel curvature:` notes the verdicts use, by a phrase each (each sits on one line of panel_curve.cpp, which the self-test pins). fault, size
# and unbound are the three ways the curve stands itself down; reissue prints once, at the layer's first strip.
ROUTE_CURVE_NOTES = (
    ("fault", "faulted, so it is off for the rest"),
    ("size", "the SIZE buffer read back"),
    ("unbound", "nothing is bound to vertex slots 1..3"),
    ("reissue", "the VR world route's layer drew the same"),
)
ROUTE_CURVE_STANDDOWN_KINDS = ("fault", "size", "unbound")
# A healthy window has curve-reissues equal to eye-takes: the layer's take and the strip's draw are one call chain, and the route reads both at the
# boundary it prints on. This many eyes of slack (two frames, at two eyes a frame) is for a boundary that falls inside a frame; it is small on
# purpose, so that a steady flat share (even 1% of a window's 900 eyes) is a WARN and not rounding.
ROUTE_CURVE_EDGE_EYES = 4
# The strip is built about 50 ms after the first composite, while the substitution reads the panel's SIZE: `pending` in more owned windows in a
# row than this is a SIZE that was never read.
ROUTE_CURVE_PENDING_WINDOWS = 2


def _route_curve_kind(text):
    """A window's curve text as a kind: off, pending, stood-down, strip (curvature/columns/gain), unknown (anything else), or None (no token)."""
    if text is None:
        return None
    if text in ROUTE_CURVE_WORDS:
        return text
    return "strip" if ROUTE_CURVE_STRIP_RE.match(text) else "unknown"


def parse_route_curve(text):
    """What --route-curve reads, each item with its 0-based line number (`line`) so a verdict can say what came before what. windows: the
    route's 5 s lines by token (curve and reissues are None for a build before the curved route; `owned` is owned-frames above 0). owns: the
    OWNS lines and the curve each names (None when its line has no `(curve=...)`). notes: the `panel curvature:` lines, `kind` one of
    ROUTE_CURVE_NOTES or other. layer: the layer's `vr world route layer:` lines with the route's refusals by reason; layer_unknown: those it
    could not read. stale: what only a build before the curved route writes (the `curved-screen` refusal)."""
    p = {"windows": [], "owns": [], "notes": [], "layer": [], "layer_unknown": [], "stale": []}
    for n, raw in enumerate(text.splitlines()):
        if "vr world route" not in raw and "panel curvature:" not in raw:
            continue
        m = ROUTE_LINE_RE.match(raw)
        if m:
            if not m.group("inject"):
                kv = _ckv(m.group("rest"))
                frames = _cint(kv.get("owned-frames"))
                p["windows"].append({
                    "line": n, "ts": (m.group("ts") or "").strip("[]"), "state": kv.get("state"), "owned_frames": frames,
                    "owned": frames > 0 if frames is not None else kv.get("state") == "owned",
                    "takes": _cint(kv.get("eye-takes")), "curve": kv.get("curve"), "reissues": _cint(kv.get("curve-reissues"))})
            continue
        s = ROUTE_CURVE_STAMP_RE.match(raw.rstrip())
        ts, msg = (s.group("ts"), s.group("msg")) if s else ("", raw.strip())
        at = {"line": n, "ts": ts}
        if msg.startswith("vr world route: OWNS the world"):
            om = ROUTE_CURVE_OWNS_RE.match(msg)
            if om:
                named = ROUTE_CURVE_NAMED_RE.search(msg, om.end())
                p["owns"].append(dict(at, frame=int(om.group("frame")), named=named.group("curve") if named else None))
        elif msg.startswith("vr world route layer: ") or msg.startswith(ROUTE_CURVE_NOT_TAKEN):
            # The layer's two kinds of line are where a build before the curved route wrote its refusal of a curved screen: the short key in the
            # 30 s line, the long text in either (the first-eight lines carry it alone, and a flight under 30 s has no other).
            if ROUTE_CURVE_STALE_TEXT in msg:
                p["stale"].append(dict(at, what="the layer's refusal reason `%s ...`" % ROUTE_CURVE_STALE_TEXT))
            if not msg.startswith("vr world route layer: "):
                continue
            lm = ROUTE_CURVE_LAYER_RE.match(msg)
            if not lm:
                p["layer_unknown"].append(dict(at, text=msg))
                sm = re.search(r"%s=([1-9]\d*)" % re.escape(ROUTE_CURVE_STALE_KEY), msg)
                if sm:
                    p["stale"].append(dict(at, what="`%s=%s` in a layer line this reader could not otherwise read" % (ROUTE_CURVE_STALE_KEY, sm.group(1))))
                continue
            checks = {k: int(v) for k, v in ROUTE_CURVE_CHECK_RE.findall(lm.group("checks"))}
            p["layer"].append(dict(at, secs=float(lm.group("secs")), checks=checks))
            if checks.get(ROUTE_CURVE_STALE_KEY, 0) > 0:
                p["stale"].append(dict(at, what="`%s=%d` in the layer's refusal line" % (ROUTE_CURVE_STALE_KEY, checks[ROUTE_CURVE_STALE_KEY])))
        elif msg.startswith("panel curvature: "):
            body = msg[len("panel curvature: "):]
            kind = next((k for k, needle in ROUTE_CURVE_NOTES if needle in body), "other")
            p["notes"].append(dict(at, kind=kind, text=body))
    return p


def _rc_when(items, shown=3):
    """The times of some parsed lines for a message: `10:00:15.100, 10:00:20.100, 10:00:25.100 and 4 more`."""
    names = [i["ts"] or "line %d" % (i["line"] + 1) for i in items]
    return ", ".join(names[:shown]) + (" and %d more" % (len(names) - shown) if len(names) > shown else "")


def _rc_curve(p, add):
    """CURVE: what the owned windows said the screen's curve was, and whether it changed. A log with nothing to read it from is a WARN, never a
    PASS: no window, no curve token (a build before the curved route), or a route that never owned the world."""
    ws = p["windows"]
    owned = [w for w in ws if w["owned"]]
    if not ws:
        add("CURVE", "WARN", "no `vr world route 5s:` window in this log (it has other route lines): there is no window to read a curve from")
        return
    if all(w["curve"] is None for w in ws):
        add("CURVE", "WARN", "no curve tokens: this log is from a build before the curved route (its %d route 5 s line(s) carry no curve= or "
            "curve-reissues=), so it says nothing about the curve and the route working together; install the current build and fly again" % len(ws))
        return
    if not owned:
        add("CURVE", "WARN", "the route never owned the world in this log (no owned-frames above 0 in its %d window(s)), so there is nothing to "
            "judge about the curve: the flight did not reach an on-foot world the route could own, or the route stood aside (state= and the OWNS "
            "lines say which)" % len(ws))
        return
    runs = []
    for w in owned:
        text = w["curve"] if w["curve"] is not None else "(no token)"
        if runs and runs[-1][0] == text:
            runs[-1][1] += 1
        else:
            runs.append([text, 1])
    counts = {}
    for text, n in runs:
        counts[text] = counts.get(text, 0) + n
    kinds = {_route_curve_kind(w["curve"]) for w in owned}
    msg = ", ".join("curve=%s in %d of %d owned windows" % (t, n, len(owned)) for t, n in counts.items())
    if len(runs) > 1:
        msg += "; it changed between windows: %s (a live edit of fix.panel_curvature, or the strip finishing its build: not a failure)" \
               % " -> ".join(t for t, _ in runs)
    if None in kinds or "unknown" in kinds:
        add("CURVE", "WARN", msg + ". A curve text this reader does not know (the formatter changed?): it is left out of the other verdicts")
    elif kinds & {"pending", "stood-down"}:
        add("CURVE", "note", msg + " (READY and STOOD DOWN judge the pending and stood-down windows)")
    else:
        add("CURVE", "PASS", msg)
    if kinds == {"off"}:
        add("CURVE", "note", "no curve was configured (fix.panel_curvature 0 at the default column count): the game's own flat quad was drawn and re-issued, "
            "so this log says nothing about the curved route; set fix.panel_curvature above 0 and fly again to test it")


def _rc_reissue(p, add):
    """RE-ISSUE: with the strip in hand for a whole owned window (the same curvature/columns/gain as the window before, so no build or edit fell
    inside it) every eye the layer took was drawn through the strip: curve-reissues equals eye-takes to within ROUTE_CURVE_EDGE_EYES. Takes with no
    strip are the layer's FLAT screen under a curved game draw (STOP); a strip count well off the takes is a WARN; a strip drawn while the curve
    was off in two windows running cannot happen (WARN: the instrument or the build is wrong)."""
    ws = p["windows"]
    strips_owned = [w for w in ws if w["owned"] and _route_curve_kind(w["curve"]) == "strip"]
    stop, low, high, idle, leak = [], [], [], [], []
    compared = eyes = struck = off_held = 0
    for i in range(1, len(ws)):
        w, before = ws[i], ws[i - 1]
        if w["takes"] is None or w["reissues"] is None:
            continue
        if w["curve"] == "off" and before["curve"] == "off":
            off_held += 1
            if w["reissues"] > 0:
                leak.append(w)
        if not (w["owned"] and _route_curve_kind(w["curve"]) == "strip" and before["curve"] == w["curve"]):
            continue
        if w["takes"] == 0 and w["reissues"] == 0:
            idle.append(w)
            continue
        compared += 1
        eyes += w["takes"]
        struck += w["reissues"]
        if w["takes"] > 0 and w["reissues"] == 0:
            stop.append(w)
        elif w["reissues"] < w["takes"] - ROUTE_CURVE_EDGE_EYES:
            low.append(w)
        elif w["reissues"] > w["takes"] + ROUTE_CURVE_EDGE_EYES:
            high.append(w)
    if stop:
        add("RE-ISSUE", "STOP", "%d owned window(s) (%s) took eyes with the strip in hand and drew no strip (the first: eye-takes=%d curve-reissues=0 under "
            "curve=%s): the layer drew a FLAT screen under a curved game draw, so while the route owns the world the headset shows a flat screen and "
            "the curve is lost" % (len(stop), _rc_when(stop), stop[0]["takes"], stop[0]["curve"]))
    if low:
        add("RE-ISSUE", "WARN", "curve-reissues is well below eye-takes in %d owned window(s) (%s; the first: %d strips for %d eyes, more than %d apart): "
            "the layer took the other eyes through the flat re-issue while the strip was in hand, so for those eyes the screen is flat while the "
            "curve is on" % (len(low), _rc_when(low), low[0]["reissues"], low[0]["takes"], ROUTE_CURVE_EDGE_EYES))
    if high:
        add("RE-ISSUE", "WARN", "curve-reissues is above eye-takes in %d owned window(s) (%s; the first: %d strips for %d eyes, more than %d apart): the "
            "layer drew a strip for eyes it then did not take, so the eye route served them (a fault, or a state that would not go back: see FAULT "
            "and the layer's refusal line)" % (len(high), _rc_when(high), high[0]["reissues"], high[0]["takes"], ROUTE_CURVE_EDGE_EYES))
    if leak:
        add("RE-ISSUE", "WARN", "curve-reissues is above 0 while curve=off in %d window(s) (%s; the first: %d) and the window before said off too: impossible "
            "by construction, since the layer only draws a strip the curve built, so this instrument is wrong or the log is from a stale build"
            % (len(leak), _rc_when(leak), leak[0]["reissues"]))
    if compared and not (stop or low or high):
        add("RE-ISSUE", "PASS", "curve-reissues equals eye-takes (to within %d eyes) in all %d owned window(s) that had the strip in hand for the whole "
            "window (%d eye takes, %d strips)" % (ROUTE_CURVE_EDGE_EYES, compared, eyes, struck))
    if strips_owned and not compared:
        add("RE-ISSUE", "WARN", "the strip's re-issue was not compared in any window: it needs an owned window with the same curve= as the window before "
            "it and eye-takes above 0, and this log has none; nothing here shows the layer drawing the strip")
    if idle:
        add("RE-ISSUE", "note", "%d owned window(s) (%s) had the strip in hand and took no eye (eye-takes=0): nothing to compare in them"
            % (len(idle), _rc_when(idle)))
    if off_held and not leak:
        add("RE-ISSUE", "PASS", "no strip was drawn while the curve was off (curve-reissues=0 in the %d window(s) that said curve=off right after a window "
            "that said it too)" % off_held)
    total = sum(w["reissues"] or 0 for w in ws)
    firsts = [n for n in p["notes"] if n["kind"] == "reissue"]
    if total and not firsts:
        add("RE-ISSUE", "note", "%d strip(s) were re-issued but the log has no `panel curvature: the VR world route's layer drew the same ...` line (it "
            "prints once, at the first one): the log is cut, or that note's text changed" % total)
    elif len(firsts) > 1:
        add("RE-ISSUE", "note", "the first-re-issue note appears %d times (%s); it prints once a session" % (len(firsts), _rc_when(firsts)))


def _rc_ready(p, add):
    """READY: `pending` is the strip not built yet, while the substitution reads the panel's SIZE (about 50 ms); the game and the layer both draw the
    flat quad meanwhile, consistent but flat. More than ROUTE_CURVE_PENDING_WINDOWS owned windows of it in a row is a SIZE that was never read."""
    owned = [w for w in p["windows"] if w["owned"]]
    best = cur = 0
    first = last = start = None
    for w in owned:
        if w["curve"] == "pending":
            if cur == 0:
                start = w
            cur += 1
            if cur > best:
                best, first, last = cur, start, w
        else:
            cur = 0
    if best > ROUTE_CURVE_PENDING_WINDOWS:
        add("READY", "WARN", "curve=pending in %d owned windows in a row (%s to %s): the panel's SIZE was never read, so the game and the layer both draw "
            "the flat quad: consistent with each other, but flat at a curvature above 0. The strip builds within about 50 ms of the first composite, "
            "so %d windows is the most a healthy start shows" % (best, first["ts"], last["ts"], ROUTE_CURVE_PENDING_WINDOWS))
    elif best:
        add("READY", "note", "curve=pending in %d owned window(s) (%s%s): the strip was still being built, which a healthy start shows for up to %d%s"
            % (best, first["ts"], " to " + last["ts"] if last is not first else "", ROUTE_CURVE_PENDING_WINDOWS,
               "; the log ends with it still pending" if owned and owned[-1]["curve"] == "pending" else ""))
    elif any(_route_curve_kind(w["curve"]) == "strip" for w in owned):
        add("READY", "PASS", "no owned window said pending: the strip was in hand when the route first owned the world")


def _rc_standdown(p, add):
    """STOOD DOWN: a fault, or a panel SIZE that cannot be one, turns the curve off for the session; the game then draws its own flat quad and the
    layer re-issues it flat, as before the curved route. Any window saying so is a WARN carrying the nearest `panel curvature:` note before it."""
    down = [w for w in p["windows"] if w["curve"] == "stood-down"]
    if not down:
        add("STOOD DOWN", "PASS", "no window said curve=stood-down")
        return
    cause = [n for n in p["notes"] if n["kind"] in ROUTE_CURVE_STANDDOWN_KINDS and n["line"] < down[0]["line"]]
    why = ("The nearest note before it (%s): panel curvature: %s" % (cause[-1]["ts"], cause[-1]["text"][:320] + ("..." if len(cause[-1]["text"]) > 320 else ""))
           if cause else "No `panel curvature:` note before it names the cause (this log may start after it).")
    add("STOOD DOWN", "WARN", "curve=stood-down in %d window(s) (%s): the curve turned itself off for the rest of the session, so the game draws its own flat "
        "quad and the layer re-issues it flat, as before the curved route. %s" % (len(down), _rc_when(down), why))


def _rc_stale(p, add, say_pass):
    """STALE BUILD: the layer's `curved-screen` refusal was deleted with the build that made the route re-issue a curved screen; a log that carries
    it flew the build before, and its numbers say nothing about this one. say_pass is False for a log with no curve token (CURVE has already said
    it is from a build before the curved route): a PASS there would read as if that build were fine."""
    if p["stale"]:
        add("STALE BUILD", "STOP", "%d line(s) carry the refusal that only the build before the curved route writes (the first at %s: %s): that build's route "
            "stood aside for any curved screen, so what this log says about the route and the curve is that build's, not this one's; install the "
            "current build and fly again" % (len(p["stale"]), p["stale"][0]["ts"], p["stale"][0]["what"]))
    elif not say_pass:
        return
    elif p["layer"]:
        add("STALE BUILD", "PASS", "no curved-screen refusal in the layer's %d refusal line(s)" % len(p["layer"]))
    else:
        add("STALE BUILD", "note", "no `vr world route layer:` line in this log (the layer prints one every 30 s while the route key is auto), so a stale build "
            "could not be ruled out from it")


def _rc_owns(p, add):
    """OWNS: the OWNS line names a curve (`(curve=<text>)` in the sentence it adds) exactly when the 5 s window after it shows a curve other than off.
    `pending` and `stood-down` are curves here (the strip was still being built; the sentence says so). A mismatch is a note, never a verdict."""
    ws, owns = p["windows"], p["owns"]
    if not owns:
        add("OWNS", "note", "no OWNS line in this log (the route never entered ownership, or this log starts after it)")
        return
    bad, agree, named, wi = [], 0, 0, 0
    for o in owns:
        while wi < len(ws) and ws[wi]["line"] < o["line"]:
            wi += 1
        if wi >= len(ws) or ws[wi]["curve"] is None:
            continue
        w = ws[wi]
        says = o["named"] not in (None, "off")
        shows = w["curve"] != "off"
        named += says
        if says == shows:
            agree += 1
        else:
            bad.append((o, w))
    for o, w in bad[:3]:
        if o["named"] in (None, "off"):
            add("OWNS", "note", "%s: the OWNS line names no curve but the 5 s window after it (%s) says curve=%s: the curve was set or finished building "
                "after the route took the world (a live edit, or the strip's build)" % (o["ts"], w["ts"], w["curve"]))
        else:
            add("OWNS", "note", "%s: the OWNS line says (curve=%s) but the 5 s window after it (%s) says curve=off: the curve was turned off in between "
                "(a live edit)" % (o["ts"], o["named"], w["ts"]))
    if len(bad) > 3:
        add("OWNS", "note", "and %d more OWNS line(s) that disagree with the window after them" % (len(bad) - 3))
    if agree and not bad:
        add("OWNS", "PASS", "%d OWNS line(s): each names a curve exactly when the 5 s window after it shows one (%d named; pending counts as a curve: the "
            "strip was still being built)" % (agree, named))
    elif not agree and not bad:
        add("OWNS", "note", "no 5 s window after the OWNS line(s) to compare them with")


def _rc_fault(p, add):
    """FAULT: a `fault` refusal in the layer's refusal line while strips were re-issued in the same span (the route windows printed since the
    layer's last line: its window is 30 s, the route's 5 s) is a strip draw that faulted after the layer's bracket opened; the eye route served
    those eyes, and one fault stands the curve down for the session."""
    ws = p["windows"]
    if not p["layer"]:
        return
    hits, flat, prev = [], 0, -1
    for l in p["layer"]:
        faults = l["checks"].get("fault", 0)
        if faults:
            strips = sum(w["reissues"] or 0 for w in ws if prev < w["line"] < l["line"])
            if strips:
                hits.append((l, faults, strips))
            else:
                flat += faults
        prev = l["line"]
    if hits:
        l, faults, strips = hits[0]
        add("FAULT", "WARN", "fault refusals beside re-issued strips in %d layer line(s) (%s; the first: fault=%d with %d strips in its span): a strip draw "
            "faulted after the layer's bracket opened, so the eye route served those eyes; one fault stands the curve down for the session (see "
            "STOOD DOWN and the `panel curvature:` note)" % (len(hits), _rc_when([h[0] for h in hits]), faults, strips))
    else:
        add("FAULT", "PASS", "no fault refusal beside re-issued strips in the layer's %d refusal line(s)" % len(p["layer"]))
    if flat:
        add("FAULT", "note", "%d fault refusal(s) in spans with no strip re-issued: the flat re-issue's own bind, not the curve's" % flat)


def _rc_layer_unknown(p, add):
    if p["layer_unknown"]:
        add("LAYER", "WARN", "%d `vr world route layer:` line(s) this reader does not know (the formatter changed?), so STALE BUILD and FAULT did not see them; "
            "the first: %s" % (len(p["layer_unknown"]), p["layer_unknown"][0]["text"][:200]))


def route_curve_judge(p):
    """[(tag, status, text)] in the order of the rules: CURVE, RE-ISSUE, READY, STOOD DOWN, STALE BUILD, OWNS, FAULT (and LAYER, a layer line this
    reader cannot read). status is PASS, WARN, STOP or note; notes are facts and never change the verdict. A log with no curve token (a build
    before the curved route) is judged by what it can still say (STALE BUILD), and its CURVE line is a WARN that says so."""
    out = []

    def add(tag, status, text):
        out.append((tag, status, text))

    ws = p["windows"]
    tokens = any(w["curve"] is not None for w in ws)
    _rc_curve(p, add)
    if tokens and any(w["owned"] for w in ws):
        _rc_reissue(p, add)
        _rc_ready(p, add)
    if tokens:
        _rc_standdown(p, add)
    _rc_stale(p, add, tokens)
    if tokens:
        _rc_owns(p, add)
        _rc_fault(p, add)
    _rc_layer_unknown(p, add)
    return out


def _rc_runs(ws):
    """The route's windows as runs of the same ownership and curve: [{owned, curve, n, first, last, takes, strips}]."""
    runs = []
    for w in ws:
        if runs and runs[-1]["owned"] == w["owned"] and runs[-1]["curve"] == w["curve"]:
            r = runs[-1]
            r["n"] += 1
            r["last"] = w["ts"]
            r["takes"] += w["takes"] or 0
            r["strips"] += w["reissues"] or 0
        else:
            runs.append({"owned": w["owned"], "curve": w["curve"], "n": 1, "first": w["ts"], "last": w["ts"],
                         "takes": w["takes"] or 0, "strips": w["reissues"] or 0})
    return runs


def print_route_curve(text, path=None):
    """--route-curve: the curved VR world route's flight in one report; exit 0 (PASS or WARN), 1 (STOP), 3 (no route line in the log). `path` is the log's
    file, named in the header beside the build (main() has already printed its full path and the version line)."""
    p = parse_route_curve(text)
    ws = p["windows"]
    if not (ws or p["owns"] or p["layer"] or p["layer_unknown"]):
        print("[edvr] route-curve: no `vr world route` line in this log. The key experimental.temporal_aa_on_foot_world was not auto, the UI layer was not "
              "live (the route stays off then), or this is not a VR flight; with the key auto the route prints a 5 s line every window, zeros included.")
        return 3
    ver = version_line(text)[1]
    print("[edvr] route-curve: %sbuild %s; %d route window(s), %d owned; %d OWNS line(s), %d `panel curvature:` note(s), %d layer refusal line(s)"
          % (os.path.basename(path) + ", " if path else "", ver or "(no version line)", len(ws), sum(1 for w in ws if w["owned"]), len(p["owns"]),
             len(p["notes"]), len(p["layer"])))
    runs = _rc_runs(ws)
    if runs:
        print("route windows, by ownership and curve (%d run(s)):" % len(runs))
        shown = runs if len(runs) <= 12 else runs[:6] + [None] + runs[-5:]
        for r in shown:
            if r is None:
                print("  ... %d run(s) left out ..." % (len(runs) - 11))
                continue
            print("  %s .. %s  %3d window(s)  %-9s  curve=%s  eye-takes %d, curve-reissues %d"
                  % (r["first"], r["last"], r["n"], "owned" if r["owned"] else "not owned", r["curve"] if r["curve"] is not None else "(no token)",
                     r["takes"], r["strips"]))
    if p["owns"]:
        print("OWNS lines (%d): %s%s" % (len(p["owns"]), "; ".join("%s frame %d%s" % (o["ts"], o["frame"], " (curve=%s)" % o["named"] if o["named"] is not None else "")
                                                                 for o in p["owns"][:6]), " ..." if len(p["owns"]) > 6 else ""))
    if p["notes"]:
        print("`panel curvature:` notes (%d):" % len(p["notes"]))
        for n in p["notes"][:8]:
            print("  %s  %s" % (n["ts"], n["text"][:110] + ("..." if len(n["text"]) > 110 else "")))
        if len(p["notes"]) > 8:
            print("  ... %d more" % (len(p["notes"]) - 8))
    if p["layer"]:
        listed = [l for l in p["layer"] if l["checks"]]
        print("layer refusal lines (%d): %s" % (len(p["layer"]), "; ".join("%s %s" % (l["ts"], ", ".join("%s=%d" % kv for kv in l["checks"].items()))
                                                                          for l in listed[:6]) + (" ..." if len(listed) > 6 else "")
                                                if listed else "none lists a refusal by the route's own checks"))
    findings = route_curve_judge(p)
    for tag, status, msg in findings:
        print("  %-4s  %s: %s" % (status, tag, msg))
    stops = sum(1 for f in findings if f[1] == "STOP")
    warns = sum(1 for f in findings if f[1] == "WARN")
    verdict = "STOP" if stops else "WARN" if warns else "PASS"
    print("route-curve verdict: %s (%d STOP, %d WARN). PASS: strips drawn equal eye takes wherever the strip was in hand, built within %d windows, nothing "
          "stood down, no stale-build refusal, no fault beside strips. STOP: eyes taken and no strip drawn (a flat screen under a curved game draw), or a "
          "`curved-screen` refusal (a stale build). WARN: a strip count off its eye takes, a strip still pending, a stood-down curve, a fault beside "
          "strips, or nothing to judge (no curve token, no owned window)." % (verdict, stops, warns, ROUTE_CURVE_PENDING_WINDOWS))
    return 1 if stops else 0


MAP_BOUNCE_PREFIX = "flat map bounce 5s:"
MAP_BOUNCE_DECISION_PREFIX = "flat map bounce: decision"
MAP_BOUNCE_PENDING_PREFIX = "flat map bounce: pending"
MAP_BOUNCE_FIELDS = ("state", "key", "maps", "bounced", "flushes", "trips",
                     "bank-sample-bytes",
                     "verify-samples", "verify-mismatches", "bank-ns-per-kb")


def _map_bounce_fields(line):
    """Read the key=value tokens from one map-bounce summary/decision line."""
    return dict(re.findall(r"(?<!\S)([a-z][a-z0-9-]*)=([^\s,;]+)", line))


def parse_map_bounce(text):
    """Return summary lines, malformed summary lines, and decisions for issue 65."""
    summaries, malformed, decisions, pending = [], [], [], []
    for line_no, line in enumerate(text.splitlines(), 1):
        if MAP_BOUNCE_PREFIX in line:
            fields = _map_bounce_fields(line)
            missing = [key for key in MAP_BOUNCE_FIELDS if key not in fields]
            integer_fields = MAP_BOUNCE_FIELDS[2:-1]
            bad_numbers = [key for key in integer_fields if key in fields and
                           (not re.fullmatch(r"\d{1,20}", fields[key]) or
                            int(fields[key]) > 18446744073709551615)]
            if ("bank-ns-per-kb" in fields and
                    (len(fields["bank-ns-per-kb"]) > 64 or
                     not re.fullmatch(r"\d+(?:\.\d*)?|\.\d+", fields["bank-ns-per-kb"]) or
                     not math.isfinite(float(fields["bank-ns-per-kb"])))):
                bad_numbers.append("bank-ns-per-kb")
            if (missing or bad_numbers or
                    fields.get("state") not in ("pending", "on", "off", "tripped") or
                    fields.get("key") not in ("auto", "on", "off")):
                malformed.append({"line": line_no, "text": line,
                                  "missing": missing, "bad_numbers": bad_numbers,
                                  "fields": fields})
            else:
                for key in ("maps", "bounced", "flushes", "trips",
                            "bank-sample-bytes", "verify-samples", "verify-mismatches"):
                    fields[key] = int(fields[key])
                fields["bank-ns-per-kb"] = float(fields["bank-ns-per-kb"])
                fields["line"] = line_no
                summaries.append(fields)
        elif MAP_BOUNCE_DECISION_PREFIX in line:
            fields = _map_bounce_fields(line)
            result = re.search(r"threshold\s*(?:=\s*)?([^\s,;]+)\s*->\s*(ON|OFF)\b", line)
            if result:
                fields["result"] = result.group(2)
                fields["threshold"] = result.group(1)
            fields["line"] = line_no
            fields["text"] = line
            decisions.append(fields)
        elif MAP_BOUNCE_PENDING_PREFIX in line:
            pending.append({"line": line_no, "text": line})
    return {"summaries": summaries, "malformed": malformed,
            "decisions": decisions, "pending": pending}


def print_map_bounce(text, path=None):
    """Report issue 65 map-bounce instrumentation and verdict."""
    p = parse_map_bounce(text)
    windows, decisions = p["summaries"], p["decisions"]
    if not windows and not p["malformed"] and not decisions and not p["pending"]:
        print("[edvr] map-bounce: NEVER RAN; no `flat map bounce 5s:` summary or decision line in this log. Enable the instrument and fly a full summary window.")
        print("map-bounce verdict: NEVER RAN")
        return 3

    latest = windows[-1] if windows else None
    print("[edvr] map-bounce: %sbuild %s; %d summary window(s), %d decision line(s), %d malformed summary line(s)" %
          (os.path.basename(path) + ", " if path else "",
           version_line(text)[1] or "(no version line)", len(windows),
           len(decisions), len(p["malformed"])))
    if decisions:
        for d in decisions[-3:]:
            print("  DECISION %s" % d["text"].strip())
    if latest:
        print("  latest window: state=%s key=%s maps=%d bounced=%d flushes=%d bank-ns-per-kb=%.3f verify-samples=%d mismatches=%d trips=%d" %
              (latest["state"], latest["key"], latest["maps"], latest["bounced"],
               latest["flushes"], latest["bank-ns-per-kb"],
               latest["verify-samples"], latest["verify-mismatches"], latest["trips"]))

    findings = []
    if p["malformed"]:
        findings.append(("WARN", "FORMAT", "%d summary line(s) have missing or malformed fields; first at line %d" %
                         (len(p["malformed"]), p["malformed"][0]["line"])))
        for record in p["malformed"]:
            safety = record["fields"]
            for field, tag in (("trips", "TRIPS"), ("verify-mismatches", "VERIFY")):
                raw = safety.get(field)
                if raw is None or not re.fullmatch(r"\d{1,20}", raw) or int(raw) > 0:
                    findings.append(("STOP", tag, "unsafe or unreadable %s in malformed summary at line %d" %
                                     (field, record["line"])))
    if not windows:
        findings.append(("WARN", "INSTRUMENT", "no valid 5 s summary proves the instrument ran"))
    else:
        states = [w["state"] for w in windows]
        trips = max(w["trips"] for w in windows)  # C++ reports the cumulative trip count.
        mismatches = sum(w["verify-mismatches"] for w in windows)
        if "tripped" in states or trips:
            findings.append(("STOP", "TRIPS", "fail-safe tripped (%d recorded trip(s))" % trips))
        if mismatches:
            findings.append(("STOP", "VERIFY", "%d verification mismatch(es) recorded" % mismatches))
        if latest["state"] == "pending":
            findings.append(("WARN", "PENDING", "the latest adaptive decision is still pending"))
        if latest["state"] == "off":
            measured = [w["bank-ns-per-kb"] for w in windows if w["bank-sample-bytes"] > 0 and w["bank-ns-per-kb"] > 0]
            if latest["key"] == "auto":
                if decisions and decisions[-1].get("result") == "OFF" and measured:
                    findings.append(("PASS", "OFF", "adaptive decision selected OFF after sampled bank reads"))
                else:
                    findings.append(("WARN", "OFF", "adaptive OFF lacks both an OFF decision and sampled bank-read timing"))
            elif latest["bank-ns-per-kb"] > 1024.0:
                findings.append(("WARN", "SLOW OFF", "bank reads are slow at %.1f ns/KB while bouncing is off" % latest["bank-ns-per-kb"]))
        elif latest["state"] == "on":
            if latest["bounced"]:
                findings.append(("PASS", "ACTIVE", "map bounce is active; %d maps bounced in the latest window" % latest["bounced"]))
            else:
                findings.append(("WARN", "ACTIVE", "state says on but no map bounce was counted in the latest window"))
        # Forced-on mode verifies every 16th flush, across window boundaries.
        if latest["key"] == "on":
            expected = sum(w["flushes"] for w in windows) // 16
            observed = sum(w["verify-samples"] for w in windows)
            if observed < expected:
                findings.append(("WARN", "VERIFY COVERAGE", "%d forced-on flushes imply at least %d verification samples; saw %d" %
                                 (sum(w["flushes"] for w in windows), expected, observed)))
    for status, tag, msg in findings:
        print("  %-4s  %s: %s" % (status, tag, msg))
    stops = sum(1 for status, _, _ in findings if status == "STOP")
    warns = sum(1 for status, _, _ in findings if status == "WARN")
    verdict = "STOP" if stops else "WARN" if warns else "PASS"
    print("map-bounce verdict: %s (%d STOP, %d WARN)" % (verdict, stops, warns))
    return 1 if stops else 0


def self_test_route_curve():
    """--route-curve on logs built from the route's own formatter output: the 5 s lines of tools\\camera_census_fixture.log (which
    tools\\vr_world_route_test holds to the formatter) with only their curve tokens, eye-takes, owned-frames and state swapped, the flat OWNS line of
    tools\\maps_sharp_fixture.log with each state's sentence added, and the layer's line as a real flight wrote it. Every case asserts the verdict AND the
    exit code; the pins at the top tie the reader's tokens and phrases to the sources that write them. Returns ok."""
    import contextlib
    import io
    import shutil
    import tempfile
    ok = True

    def fail(msg):
        nonlocal ok
        print("route-curve: %s" % msg)
        ok = False

    root = repo_root()
    tools = os.path.join(root, "tools")
    try:
        census = read_text(os.path.join(tools, CENSUS_FIXTURE))
        maps = read_text(os.path.join(tools, "maps_sharp_fixture.log"))
    except OSError as e:
        fail("a fixture the self-test builds from is missing (%s)" % e)
        return False

    # The pins: each phrase the reader keys on must still be in the source that writes it (each sits on one line there, which is what makes it
    # pinnable), and the one key that marks a stale build, `curved-screen`, must not be in the header that deleted it. A reworded note or a
    # key brought back fails here, with the file named, instead of making a flight's verdict quietly wrong.
    for rel, needles, absent in (
            (("src", "d3d11", "vr_world_route_math.h"), ("(curve=",), ()),
            (("src", "d3d11", "panel_curve.cpp"), tuple(n for _, n in ROUTE_CURVE_NOTES), ()),
            (("src", "d3d11", "ui_layer.cpp"), ("vr world route layer: ", "route's own checks: %s; refused by the decision's tests"), ()),
            (("src", "d3d11", "ui_layer_math.h"), ('return "fault";', ROUTE_CURVE_NOT_TAKEN + "eye %d: %s"), ('"%s"' % ROUTE_CURVE_STALE_KEY,))):
        try:
            body = read_text(os.path.join(root, *rel))
        except OSError:
            fail("%s is not where the self-test looks for it" % "\\".join(rel))
            continue
        for needle in needles:
            if needle not in body:
                fail("%s no longer has %r, which --route-curve reads; change this reader and its pin together" % ("\\".join(rel), needle))
        for needle in absent:
            if needle in body:
                fail("%s has %s again: --route-curve reads it as the sign of a build before the curved route" % ("\\".join(rel), needle))

    # The base route line: the fixture's owned window, whose curve tokens are the formatter's. Everything a case swaps is pinned to occur once.
    owned_lines = [l for l in census.splitlines() if "vr world route 5s:" in l and " state=owned " in l]
    base = owned_lines[0] if len(owned_lines) == 1 else ""
    tok = " curve=off curve-reissues=0 "
    counts = [base.count(tok), base.count(" state=owned "), len(re.findall(r" owned-frames=\d+ ", base)), len(re.findall(r" eye-takes=\d+ ", base))]
    if not base or counts != [1, 1, 1, 1]:
        fail("the census fixture's owned route line is not one line with ' curve=off curve-reissues=0 ', ' state=owned ', one owned-frames= and one "
             "eye-takes= (found %d line(s), counts %s): the formatter moved a token, or the fixture was regenerated without it" % (len(owned_lines), counts))
        return False
    owned0 = int(re.search(r" owned-frames=(\d+) ", base).group(1))
    takes0 = int(re.search(r" eye-takes=(\d+) ", base).group(1))
    if takes0 < 100:
        fail("the census fixture's owned window has eye-takes=%d: the tolerance cases need a window of at least 100 eyes" % takes0)
        return False
    owns_lines = [l for l in maps.splitlines() if "vr world route: OWNS the world" in l]
    if len(owns_lines) != 1 or "(curve=" in owns_lines[0]:
        fail("tools\\maps_sharp_fixture.log should hold one flat OWNS line, found %d (or it names a curve)" % len(owns_lines))
        return False
    owns_flat = re.sub(r"^\[[\d:.]+\] ", "", owns_lines[0])
    owns_frame = int(re.search(r"OWNS the world from frame=(\d+) ", owns_flat).group(1))

    # The sentences vrWorldFormatEntered adds for a screen set to curve, one per state (the reader keys on `(curve=<text>)` in them, not the words).
    sent_strip = ("; the screen is curved (curve=%s): the layer draws the same strip the game's own draw is substituted with, so the bend and the "
                  "placement are the game's")
    sent_pending = "; the screen is set to curve (curve=pending): the strip is not built yet, so the game and the layer both draw the flat quad until it is"
    sent_down = ("; the screen is set to curve but the curve stood down (curve=stood-down): the game draws its own flat quad and the layer "
                 "re-issues it flat")
    # The layer's line, verbatim from a real flight log (2026-10-01 09:21:27.190, Frontier): no rig holds this one, since it is a printf in ui_layer.cpp.
    layer_real = ("vr world route layer: 30 s; 0 screen draws re-issued into the layer (0.00 a frame); refused by the route's own checks: none; refused by "
                  "the decision's tests (every 2D screen draw this window, owned frames or not): none; 0 draws into a re-issued eye left in the game's "
                  "frame (lost while the route owns that eye). The first eight distinct reasons are named in full, once each, in the lines "
                  "\"vr world route: layer did not take the screen draw for eye N\".")
    # A route line from a real flight of a build before the curved route (2026-10-01 09:23:52.317): no curve tokens.
    route_old = ("[09:23:52.317] vr world route 5s: key=auto state=observing layer=live gate=no frames=432 gate-frames=432 gate-flips=0 hdr-frames=0 "
                 "trigger=0 none=0 ambiguous=0 treated=0 declined=0 owned-frames=0 eye-takes=0 door-layer-only=0 enters=0 releases=0 (last=none) "
                 "scene-resets=0 late-hdr-writes=0 (in 0 frames) last=none jitter=idle phase=0.0000,0.0000 rows=0.0000,0.0000 fp-mode=0/0/0 "
                 "steady-detail=on last-trigger=VS=0000000000000000 PS=0000000000000000 target=0x0 hdr=0x0 selection=none")
    if layer_real.count("route's own checks: none;") != 1:
        fail("the self-test's own layer line lost its 'route's own checks: none;'")
        return False

    strip = "0.300/64/0.531"
    eyes = takes0

    def win(i, curve="off", reissues=0, takes=None, owned=True):
        """Window i of a flight (5 s apart): the base line with its curve tokens, eye-takes, owned-frames and state swapped."""
        takes = eyes if takes is None else takes
        line = re.sub(r"^\[[\d:.]+\]", "[10:%02d:%02d.100]" % (5 * i // 60, 5 * i % 60), base)
        line = line.replace(tok, " curve=%s curve-reissues=%d " % (curve, reissues))
        line = re.sub(r" owned-frames=\d+ ", " owned-frames=%d " % (owned0 if owned else 0), line)
        line = re.sub(r" eye-takes=\d+ ", " eye-takes=%d " % (takes if owned else 0), line)
        return line.replace(" state=owned ", " state=%s " % ("owned" if owned else "observing"))

    def many(n, curve, reissues=None, start=0, **kw):
        """n windows of one curve; reissues defaults to the window's eye takes, the healthy count."""
        return [win(start + i, curve, eyes if reissues is None else reissues, **kw) for i in range(n)]

    def owns(sentence="", at="10:00:00.050"):
        return "[%s] %s%s" % (at, owns_flat, sentence)

    def note(text, at="10:00:14.000"):
        return "[%s] panel curvature: %s" % (at, text)

    def layer(checks="none", at="10:00:30.200"):
        return "[%s] %s" % (at, layer_real.replace("route's own checks: none;", "route's own checks: %s;" % checks))

    def lines(*parts):
        flat = []
        for part in parts:
            flat.extend(part if isinstance(part, list) else [part])
        return "\n".join(flat) + "\n"

    def run(text, path=None):
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            rc = print_route_curve(text, path)
        return rc, buf.getvalue()

    def case(what, text, rc_want, verdict, *has, absent=()):
        rc, out = run(text)
        problems = []
        if rc != rc_want:
            problems.append("exit %d, wanted %d" % (rc, rc_want))
        if verdict and ("route-curve verdict: %s (" % verdict) not in out:
            problems.append("the verdict is not %s" % verdict)
        problems += ["lacks %r" % h for h in has if h not in out]
        problems += ["has %r" % a for a in absent if a in out]
        if problems:
            fail("%s: %s:\n%s" % (what, "; ".join(problems), out))

    no_stop_warn = ("  STOP  ", "  WARN  ")
    fault_note = note("the VR world route's re-issue of the strip faulted, so it is off for the rest of this session and the game's own quad is drawn "
                      "again. The input assembler was put back, so the screen should look exactly as it did before -- if it does not, restart the game "
                      "and report the log.")

    # ---- what the parser reads out of a formatter-held line ----
    pw = parse_route_curve(win(3, strip, eyes))["windows"]
    if len(pw) != 1 or (pw[0]["state"], pw[0]["owned"], pw[0]["owned_frames"], pw[0]["takes"], pw[0]["curve"], pw[0]["reissues"]) != (
            "owned", True, owned0, eyes, strip, eyes):
        fail("the parser reads a swapped fixture line as %r" % pw)
    if parse_route_curve(census)["windows"][-1]["curve"] != "off" or len(parse_route_curve(census)["windows"]) != 2:
        fail("the census fixture's own route lines (two windows, the inject lines between skipped) read as %r" % parse_route_curve(census)["windows"])

    # ---- the verdicts ----
    healthy = lines(owns(sent_strip % strip), many(11, strip), layer())
    case("a healthy curved run", healthy, 0, "PASS", "curve=0.300/64/0.531 in 11 of 11 owned windows",
         "curve-reissues equals eye-takes (to within 4 eyes) in all 10 owned window(s)", "PASS  OWNS", "PASS  STALE BUILD", "PASS  STOOD DOWN",
         "PASS  READY", "OWNS lines (1): 10:00:00.050 frame %d (curve=%s)" % (owns_frame, strip),
         "layer refusal lines (1): none lists a refusal by the route's own checks", absent=no_stop_warn)
    case("a log with no version line says so in the header", healthy, 0, "PASS", "[edvr] route-curve: build (no version line); 11 route window(s), 11 owned")
    rc, out = run("[10:00:00.000] version 0.14.1-93-gf78eba4 (build 68C0A1F2)\n" + healthy, os.path.join("somewhere", "edvr_gfx_20260101_000000.log"))
    if rc != 0 or "[edvr] route-curve: edvr_gfx_20260101_000000.log, build 0.14.1-93-gf78eba4; 11 route window(s), 11 owned" not in out:
        fail("the header should name the log's file and the build:\n%s" % out)
    case("the census fixture as it is (a curve of off and no OWNS line)", census, 0, "PASS", "curve=off in 1 of 1 owned windows", "no curve was configured")
    case("the same run with CRLF line ends", healthy.replace("\n", "\r\n"), 0, "PASS", "in 11 of 11 owned windows")
    case("the same run with no time stamps", re.sub(r"(?m)^\[[\d:.]+\] ", "", healthy), 0, "PASS", "in 11 of 11 owned windows")

    case("the flat screen under a curved draw: eye takes and no strip", lines(many(11, strip, 0)), 1, "STOP",
         "STOP  RE-ISSUE", "10 owned window(s)", "the layer drew a FLAT screen under a curved game draw", "eye-takes=%d curve-reissues=0 under curve=%s" % (eyes, strip))
    case("curve off throughout", lines(many(6, "off", 0)), 0, "PASS", "PASS  CURVE: curve=off in 6 of 6 owned windows", "note  CURVE: no curve was configured",
         "PASS  RE-ISSUE: no strip was drawn while the curve was off", absent=no_stop_warn)

    case("pending in three owned windows in a row", lines(many(3, "pending", 0), many(3, strip, start=3)), 0, "WARN",
         "WARN  READY: curve=pending in 3 owned windows in a row", "the panel's SIZE was never read", "10:00:00.100 to 10:00:10.100")
    case("pending in two owned windows is the healthy start", lines(many(2, "pending", 0), many(4, strip, start=2)), 0, "PASS",
         "note  READY: curve=pending in 2 owned window(s)", absent=no_stop_warn)
    case("pending, then not, then pending: the run is the longest consecutive one", lines(many(2, "pending", 0), many(1, strip, start=2),
         many(2, "pending", 0, start=3), many(2, strip, start=5)), 0, "PASS", "note  READY: curve=pending in 2 owned window(s)")
    case("the strip built between two windows: the window it was built in is not judged", lines(win(0, "pending", 0), win(1, strip, eyes // 2), win(2, strip, eyes)),
         0, "PASS", "all 1 owned window(s) that had the strip in hand", absent=("  STOP  ",))

    case("stood down after a fault note", lines(many(3, strip), fault_note, many(3, "stood-down", 0, start=3)), 0, "WARN",
         "WARN  STOOD DOWN: curve=stood-down in 3 window(s)", "the VR world route's re-issue of the strip faulted", "panel curvature: the VR world route's re-issue",
         "`panel curvature:` notes (1):\n  10:00:14.000  the VR world route's re-issue of the strip faulted")
    case("stood down with no note before it", lines(many(3, strip), many(2, "stood-down", 0, start=3)), 0, "WARN",
         "WARN  STOOD DOWN: curve=stood-down in 2 window(s)", "No `panel curvature:` note before it names the cause")
    size_note = note("the SIZE buffer read back 0.000 x 0.000, which cannot be a panel size. The bend needs a gain in model units and there is none to be "
                     "had, so it stands down.", "10:00:01.000")
    unbound_note = note("nothing is bound to vertex slots 1..3, so the panel's SIZE cannot be read and the bend has no gain to put it in model units. "
                        "Standing down.", "10:00:00.500")
    case("stood down after a SIZE note, the nearer of two causes", lines(unbound_note, size_note, many(3, "stood-down", 0)), 0, "WARN",
         "The nearest note before it (10:00:01.000): panel curvature: the SIZE buffer read back 0.000 x 0.000",
         absent=("The nearest note before it (10:00:00.500)",))
    case("a fault note AFTER the first stood-down window is not its cause", lines(many(2, "stood-down", 0), fault_note), 0, "WARN",
         "No `panel curvature:` note before it names the cause", absent=("The nearest note before it (",))
    case("only fault, SIZE and unbound notes are causes (the build note is not)", lines(
        note("built a 64-column strip -- 130 vertices, 384 indices -- at curvature 0.300, depth sign +1."), many(2, "stood-down", 0)), 0, "WARN",
         "No `panel curvature:` note before it names the cause")

    case("a live edit from the curve to off", lines(many(3, strip), many(3, "off", 0, start=3)), 0, "PASS",
         "curve=0.300/64/0.531 in 3 of 6 owned windows, curve=off in 3 of 6 owned windows", "it changed between windows: 0.300/64/0.531 -> off",
         absent=no_stop_warn)
    case("off, pending, then the strip", lines(many(2, "off", 0), many(1, "pending", 0, start=2), many(4, strip, start=3)), 0, "PASS",
         "it changed between windows: off -> pending -> 0.300/64/0.531")
    case("a live edit of the curvature itself", lines(many(3, strip), many(3, "0.500/64/0.531", start=3)), 0, "PASS",
         "0.300/64/0.531 -> 0.500/64/0.531")
    case("a window of a live edit to off may still count strips from before it", lines(many(3, strip), win(3, "off", eyes // 2)), 0, "PASS",
         absent=("while curve=off",))

    case("a curved-screen refusal in the layer's line", lines(many(4, "off", 0), layer("curved-screen=1800")), 1, "STOP",
         "STOP  STALE BUILD", "`curved-screen=1800` in the layer's refusal line", "the build before the curved route")
    case("a curved-screen refusal beside other refusals", lines(many(4, strip), layer("depth-state=3, curved-screen=12, fault=1")), 1, "STOP", "STOP  STALE BUILD")
    old_text = ("vr world route: layer did not take the screen draw for eye 0: the screen is curved (panel_curvature): the game draws its own mesh, which a "
                "repeat of its draw would not reproduce")
    case("the old build's long refusal text alone (a flight shorter than the layer's 30 s line)", lines(many(3, "off", 0), "[10:00:01.000] " + old_text),
         1, "STOP", "STOP  STALE BUILD", "the layer's refusal reason `the screen is curved (panel_curvature) ...`")
    case("the old long text inside the layer's own line", lines(many(3, strip), layer("the screen is curved (panel_curvature): the game draws its own mesh")), 1, "STOP",
         "STOP  STALE BUILD", "the layer's refusal reason `the screen is curved (panel_curvature) ...`")
    case("curved-screen in a layer line the reader cannot otherwise read", lines(many(3, strip),
         "[10:00:20.000] vr world route layer: a format this reader never saw; curved-screen=7"), 1, "STOP", "STOP  STALE BUILD",
         "`curved-screen=7` in a layer line this reader could not otherwise read", "WARN  LAYER")
    case("curved-screen=0 in an unreadable layer line is no refusal", lines(many(3, strip),
         "[10:00:20.000] vr world route layer: a format this reader never saw; curved-screen=0"), 0, "WARN", "WARN  LAYER", absent=("STOP  STALE BUILD",))
    case("a stale log has no curve tokens and a curved-screen refusal: STOP wins over the WARN",
         lines([w.replace(tok, " ") for w in many(4, "off", 0)], layer("curved-screen=900")), 1, "STOP", "WARN  CURVE: no curve tokens", "STOP  STALE BUILD")
    case("no curved-screen in a log with a layer line", lines(many(3, "off", 0), layer("depth-state=3")), 0, "PASS", "PASS  STALE BUILD")
    case("no layer line to rule a stale build out", lines(many(3, strip)), 0, "PASS", "note  STALE BUILD: no `vr world route layer:` line")

    case("no route line at all", "[10:00:00.000] version 0.14.1-93-gf78eba4 (build 68C0A1F2)\n[10:00:01.000] nothing of the route's here\n", 3, None,
         "no `vr world route` line in this log", absent=("route-curve verdict",))
    case("a log with only the curve's own notes has no route line", lines(note("off; the game's own quad is drawn.")), 3, None, "no `vr world route` line")

    case("OWNS names the curve and the window shows it", lines(owns(sent_strip % strip), many(3, strip)), 0, "PASS",
         "PASS  OWNS: 1 OWNS line(s): each names a curve exactly when the 5 s window after it shows one (1 named")
    case("OWNS names no curve and the window shows off", lines(owns(), many(3, "off", 0)), 0, "PASS", "PASS  OWNS: 1 OWNS line(s)", "(0 named")
    case("OWNS names no curve but the window shows one: a note, not a verdict", lines(owns(), many(3, strip)), 0, "PASS",
         "note  OWNS: 10:00:00.050: the OWNS line names no curve but the 5 s window after it (10:00:00.100) says curve=%s" % strip, absent=("PASS  OWNS",))
    case("OWNS names a curve but the window shows off: a note", lines(owns(sent_strip % strip), many(3, "off", 0)), 0, "PASS",
         "the OWNS line says (curve=%s) but the 5 s window after it (10:00:00.100) says curve=off" % strip, absent=("PASS  OWNS",))
    case("OWNS pending, the window shows the strip: pending is a curve", lines(owns(sent_pending), many(3, strip)), 0, "PASS", "PASS  OWNS: 1 OWNS line(s)",
         "(1 named", absent=("note  OWNS",))
    case("OWNS stood-down, the window stood-down", lines(owns(sent_down), many(3, "stood-down", 0)), 0, "WARN", "PASS  OWNS: 1 OWNS line(s)", absent=("note  OWNS",))
    case("OWNS keys on (curve=...), not on the sentence", lines(owns("; the screen now bends (curve=%s) for you" % strip), many(3, strip)), 0, "PASS",
         "PASS  OWNS: 1 OWNS line(s)", "(1 named")
    case("an OWNS line with no window after it", lines(many(3, strip), owns(sent_strip % strip, "10:00:20.000")), 0, "PASS",
         "note  OWNS: no 5 s window after the OWNS line(s)")
    case("each OWNS line is judged against the window after it", lines(owns(), many(2, "off", 0), owns(sent_strip % strip, "10:00:12.000"), many(2, strip, start=2),
         owns("", "10:00:22.000"), many(1, strip, start=4)), 0, "PASS", "note  OWNS: 10:00:22.000: the OWNS line names no curve")

    shuffled = [re.sub(r" curve=(\S+) curve-reissues=(\d+) ", " ", w).replace(" key=auto ", " curve-reissues=%s curve=%s key=auto " % (eyes, strip), 1)
                for w in many(11, strip)]
    case("the curve tokens moved to the front of the line, reversed", lines(owns(sent_strip % strip), shuffled, layer()), 0, "PASS",
         "curve=0.300/64/0.531 in 11 of 11 owned windows", "in all 10 owned window(s)", absent=no_stop_warn)
    slim = [re.sub(r" (?:jitter|phase|rows|fp-mode|steady-detail)=\S+", "", w) for w in many(11, strip)]
    if any(" steady-detail=" in w or " jitter=" in w for w in slim):
        fail("the self-test's own slimming left a steady-detail or jitter token in a window line")
    case("the steady-detail, jitter, phase, rows and fp-mode tokens removed (a later build)", lines(owns(sent_strip % strip), slim, layer()), 0, "PASS",
         "curve=0.300/64/0.531 in 11 of 11 owned windows", "in all 10 owned window(s)", absent=no_stop_warn)

    case("a fault beside re-issued strips", lines(many(11, strip), layer("fault=3")), 0, "WARN", "WARN  FAULT: fault refusals beside re-issued strips in 1 layer line(s)",
         "fault=3 with %d strips in its span" % (10 * eyes + eyes), "layer refusal lines (1): 10:00:30.200 fault=3")
    case("a fault in a flat log is not the curve's", lines(many(6, "off", 0), layer("fault=3")), 0, "PASS", "PASS  FAULT", "3 fault refusal(s) in spans with no strip")
    case("a fault line BEFORE the strips has no strips in its span", lines(layer("fault=2", "10:00:00.050"), many(6, strip)), 0, "PASS", "PASS  FAULT",
         "2 fault refusal(s) in spans with no strip", absent=("WARN  FAULT",))
    case("the second layer line's span starts after the first", lines(many(3, strip), layer("depth-state=1", "10:00:15.200"), many(3, "off", 0, start=3),
         layer("fault=1", "10:00:30.200")), 0, "PASS", "PASS  FAULT", absent=("WARN  FAULT",))
    case("an unknown layer line", lines(many(3, "off", 0), "[10:00:20.000] vr world route layer: something the formatter never wrote"), 0, "WARN",
         "WARN  LAYER: 1 `vr world route layer:` line(s) this reader does not know", "something the formatter never wrote")

    case("a build before the curved route (the fixture's lines without the two tokens)", lines([w.replace(tok, " ") for w in many(4, "off", 0)], layer()), 0, "WARN",
         "WARN  CURVE: no curve tokens: this log is from a build before the curved route", "4 route 5 s line(s)",
         absent=("PASS  CURVE", "PASS  RE-ISSUE", "PASS  STOOD DOWN", "PASS  OWNS", "PASS  STALE BUILD", "PASS  FAULT", "note  STALE BUILD"))
    case("a real route line of a build before the curved route", lines(route_old, layer()), 0, "WARN",
         "no curve tokens: this log is from a build before the curved route", absent=("PASS  CURVE", "PASS  STALE BUILD", "route-curve verdict: PASS"))
    case("a route that never owned the world", lines(many(4, strip, 0, owned=False)), 0, "WARN",
         "WARN  CURVE: the route never owned the world in this log", absent=("PASS  CURVE",))
    just_entered = [re.sub(r" owned-frames=\d+ ", " owned-frames=0 ", w) for w in many(3, strip, 0, takes=0)]
    case("state=owned with no owned frame in the window is not an owned window", lines(just_entered), 0, "WARN",
         "WARN  CURVE: the route never owned the world in this log")
    case("a route line stream with only the other route lines", lines(owns(), layer()), 0, "WARN", "WARN  CURVE: no `vr world route 5s:` window")

    case("strips a few eyes under the takes: window-edge slack", lines(win(0, strip, eyes), win(1, strip, eyes - 4)), 0, "PASS", "to within 4 eyes")
    case("strips five eyes under the takes: well below", lines(win(0, strip, eyes), win(1, strip, eyes - 5)), 0, "WARN",
         "WARN  RE-ISSUE: curve-reissues is well below eye-takes in 1 owned window(s)", "%d strips for %d eyes, more than 4 apart" % (eyes - 5, eyes))
    case("half the eyes through the strip", lines(many(1, strip), win(1, strip, eyes // 2), win(2, strip, eyes // 2)), 0, "WARN", "in 2 owned window(s)",
         "the layer took the other eyes through the flat re-issue", absent=("STOP  RE-ISSUE",))
    case("strips four eyes over the takes: slack", lines(win(0, strip, eyes), win(1, strip, eyes + 4)), 0, "PASS", "to within 4 eyes")
    case("strips five eyes over the takes", lines(win(0, strip, eyes), win(1, strip, eyes + 5)), 0, "WARN",
         "WARN  RE-ISSUE: curve-reissues is above eye-takes in 1 owned window(s)", "the layer drew a strip for eyes it then did not take")
    case("one eye taken and no strip is still the flat screen", lines(win(0, strip, 2), win(1, strip, 0, takes=2)), 1, "STOP", "eye-takes=2 curve-reissues=0")
    case("a strip window of one: nothing before it to hold it against", lines(win(0, strip, 0)), 0, "WARN", "the strip's re-issue was not compared in any window",
         absent=("STOP  RE-ISSUE",))
    case("windows that took no eye with the strip in hand", lines(many(3, strip, 0, takes=0)), 0, "WARN", "the strip's re-issue was not compared in any window",
         "note  RE-ISSUE: 2 owned window(s)", "had the strip in hand and took no eye")
    case("strips while the curve was off, twice running", lines(win(0, "off", 0), win(1, "off", 50)), 0, "WARN",
         "WARN  RE-ISSUE: curve-reissues is above 0 while curve=off in 1 window(s)", "impossible by construction")
    case("a first-reissue note is expected once strips were drawn", lines(many(4, strip)), 0, "PASS",
         "note  RE-ISSUE: %d strip(s) were re-issued but the log has no `panel curvature: the VR world route's layer drew the same" % (4 * eyes))
    first_note = note("the VR world route's layer drew the same 64-column strip at curvature 0.300 that the game's own draw is substituted with, from the mipped "
                      "resolved screen.", "10:00:00.090")
    case("with the first-reissue note there is no such note", lines(first_note, many(4, strip)), 0, "PASS", absent=("were re-issued but the log has no",))
    case("an unknown curve text is a WARN, not a crash", lines(many(3, "weird/curve", 0)), 0, "WARN", "WARN  CURVE: curve=weird/curve in 3 of 3 owned windows",
         "does not know")

    # ---- the command line: the flag, the build check, the exit codes ----
    tmp = tempfile.mkdtemp(prefix="edvr_route_curve_")
    try:
        version = "[09:59:59.000] version 0.14.1-93-gf78eba4 (build 68C0A1F2) -- this DLL was linked 2026-09-09 20:34:39 UTC\n"
        logs = {"ok": version + healthy, "stop": version + lines(many(4, strip, 0)), "none": version + "[10:00:00.000] nothing\n"}
        for name, body in logs.items():
            with open(os.path.join(tmp, name + ".log"), "wb") as f:
                f.write(body.encode("utf-8"))

        def cli(name, *extra):
            buf = io.StringIO()
            with contextlib.redirect_stdout(buf):
                rc = main(["--file", os.path.join(tmp, name + ".log"), "--route-curve"] + list(extra))
            return rc, buf.getvalue()

        for name, extra, want_rc, want_in, want_not in (
                ("ok", ["--expect-build", "0.14.1-93-gf78eba4"], 0, "route-curve verdict: PASS", ""),
                ("ok", [], 0, "route-curve verdict: PASS", ""),
                ("ok", ["--expect-build", "0.14.1-40-gc468661"], 2, "BUILD MISMATCH", "route-curve verdict"),
                ("stop", ["--expect-build", "0.14.1-93-gf78eba4"], 1, "route-curve verdict: STOP", ""),
                ("none", [], 3, "no `vr world route` line", "route-curve verdict")):
            rc, out = cli(name, *extra)
            if rc != want_rc or want_in not in out or (want_not and want_not in out):
                fail("--route-curve through main() on the %s log with %s returned %d (wanted %d, with %r and without %r):\n%s"
                     % (name, extra or "no --expect-build", rc, want_rc, want_in, want_not, out))
        rc, out = cli("ok")
        if "[edvr] route-curve: ok.log, build 0.14.1-93-gf78eba4; 11 route window(s), 11 owned" not in out:
            fail("--route-curve through main() should name the log's file and the build in its header:\n%s" % out)
        rc, out = cli("ok", "--version")
        if rc != 0 or "route-curve verdict" in out:
            fail("--version ahead of --route-curve should print the version and stop (exit %d):\n%s" % (rc, out))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    return ok


# --freezes: one flight's freeze diagnostics (issue 63). A frame or runtime cycle of 250 ms or more is a FREEZE and ALWAYS gets a line, whatever the
# rate limit says (src/common/freeze_book.h kFreezeAlwaysLogMs), so this reads both logs for those lines, joins them across the two clocks, and says
# whether the instrument itself ran. What each source writes (a graphics log line is `[HH:MM:SS.mmm] ` local time, a runtime log line
# `YYYY-MM-DD HH:MM:SS.mmm UTC pid=.. tid=.. `; scan_flight puts both on the one local clock, --tally periodic's own):
#
#   graphics log   `monitor: FREEZE -- <ms> ms between Presents, ended now: frame N, runtime sequence S; runtime cycle <ms ...|unavailable ...>;
#                  freeze K of this session[; stall sampler <state>]. ...` (perf_monitor.cpp freezeLine), written right after the frame's LONG
#                  FRAME line, so both carry the END of the freeze, which covers [time - ms, time]; `monitor: long frame counts reason=<periodic|
#                  session_close|shutdown> key=value ...; prose` (writeFreezeSummary: the frames by size, written or not, blips apart); `monitor:
#                  worst long frame R of N: ...` (up to five, a later set replaces an earlier one); the stall sampler's `stall sampler: armed|off`,
#                  `stall: ...` (one line per sample, its tail the suspension and whether EDVR's own code was on the stack, or one that failed)
#                  and `stall sampler counts reason=...`; the GPU census's frame-gap line, which keeps a pair over 1 s as a stall and names its
#                  runtime sequence (gpu_frame_gap.h formatGapDetail).
#   runtime log    `native_long_cycle,...` (every cycle over twice the period; from 250 ms up never rate limited), `native_long_cycle_worst,rank=R,
#                  of=N,utc=...,...` (the same fields, a cycle's end in UTC), `native_long_cycle_counts,reason=...,...` every five minutes and
#                  `native_long_cycle_summary,...` at session close (the three old fields, then the counts by size).
#
# The exit code carries the verdict, as --route-curve's does: 0 for PASS or WARN, 1 for STOP, 3 when the log holds none of the freeze lines and no slow
# regime (a build from before the freeze logging; main() answers 2 for a wrong build before this runs, and this answers 2 for a runtime log of another
# build), 4 when it holds a slow regime the vendor runtime or EDVR holds (the headset-lock arc, below: the verdict is SLOW, never PASS or WARN; a STOP
# still outranks it; a regime the game's own frame holds sets neither). A line
# that opens like one of these and matches none of the formats is counted and named, never dropped, because a reader that drifted from the C++ would
# otherwise look like a quiet flight. self_test_freezes builds its logs from the exact lines the C++ writes.

FREEZE_MS = 250.0               # a frame or cycle this long is a freeze (freeze_book.h kFreezeAlwaysLogMs)
FREEZE_LONG_FRAME_S = 0.050     # a LONG FRAME line goes with a FREEZE line when its runtime sequence agrees or its time is this close
FREEZE_STALL_PAD_S = 0.050      # a stall sample goes with a freeze when it was taken in [end - gap - this, end + this]
FREEZE_CYCLE_S = 0.150          # a runtime cycle goes with a FREEZE line when its sequence is within one or its time this close
FREEZE_SUSPEND_WARN_US = 5000   # a sample that stopped the render thread longer than this is a WARN
FREEZE_SUSPEND_STOP_US = 50000  # ...and longer than this a STOP: far longer than a sample should take
FREEZE_DETAIL_MAX = 12          # the FREEZES section prints the full joins for this many freezes, the longest; the rest get a row and one summary line
# The size buckets of the counts lines, in the order the keys are written (freeze_book.h freezeBucketKey), each with its words. Only the first
# three can be unwritten: from 250 ms up every frame is written, so the counts line has no unwritten_ key for the others and their cell prints `-`.
FREEZE_BUCKETS = (("lt50", "under 50 ms"), ("50_100", "50-100 ms"), ("100_250", "100-250 ms"), ("250_1000", "250-1000 ms"), ("ge1000", "1 s and over"))

FZ_FREEZE_RE = re.compile(
    r"monitor: FREEZE -- (?P<ms>[0-9.]+) ms between Presents, ended now: frame (?P<frame>\d+), runtime sequence (?P<seq>\d+); "
    r"runtime cycle (?P<cycle>.*?); freeze (?P<n>\d+) of this session")
FZ_FREEZE_CYCLE_RE = re.compile(
    r"(?P<ms>[0-9.]+) ms \((?P<head>[0-9.]+) ms from the pose wait's return to this Present, the previous cycle (?P<prev>[0-9.]+) ms\)")
# The stall sampler's own clause, which sits right before the final sentence (and is absent from a build without the sampler): its state is
# `off (...)`, `took N sample(s), the last in <module>+0x<rva>` or `took no sample (...)`, and may hold periods, so it ends at the sentence's start.
FZ_FREEZE_SAMPLER_RE = re.compile(r"; stall sampler (?P<state>.+?)(?:\. A frame of |\.?$)")
FZ_KV_RE = re.compile(r"(?P<k>\w+)=(?P<v>\d+)")
FZ_GFX_COUNTS_RE = re.compile(r"monitor: long frame counts reason=(?P<reason>\w+) (?P<kv>[^;]*)")
FZ_GFX_WORST_RE = re.compile(
    r"monitor: worst long frame (?P<rank>\d+) of (?P<of>\d+): (?P<ms>[0-9.]+) ms between Presents, ended (?P<ended>\d\d:\d\d:\d\d\.\d{3}) "
    r"\(frame (?P<frame>\d+), runtime sequence (?P<seq>\d+), runtime cycle (?P<cycle>[0-9.]+ ms|unavailable)\)(?:; (?P<note>.*?))?\s*$")
# The sampler's one state line, at the first Present: `armed. ...`, `off (advanced.freeze_location = off). ...` or `could not start its watchdog thread; ...`.
FZ_SAMPLER_STATE_RE = re.compile(r"stall sampler: (?P<text>(?P<state>armed|off|could not start)\b.*?)\s*$")
FZ_STALL_COUNTS_RE = re.compile(r"stall sampler counts reason=(?P<reason>\w+) (?P<kv>[^;]*)")
# advanced.freeze_test_ms: the test trigger's two lines (the key as read, then the sleep itself, written just before the render thread sleeps).
FZ_TEST_ARMED_RE = re.compile(r"freeze test: advanced\.freeze_test_ms = (?P<ms>\d+)\.")
FZ_TEST_SLEEP_RE = re.compile(r"freeze test: the render thread sleeps (?P<ms>\d+) ms now\.")
# A sample's line: the head is fixed; what follows `thread N` is read piece by piece, because it grew (`suspended N us`, then a note that the stack
# pointer was outside the thread's stack, then `EDVR code on the stack: yes|no`, with the frame count and the innermost EDVR frame when yes).
FZ_STALL_RE = re.compile(
    r"stall: the render thread stalled (?P<age>[0-9.]+) ms in (?P<top>[^;]+?); owner (?P<owner>[^;]+?); stack (?P<stack>[^;]*?); "
    r"sample (?P<k>\d+) of (?P<of>\d+), last Present returned in frame (?P<frame>\d+), thread (?P<tid>\d+)")
FZ_STALL_SUSPEND_RE = re.compile(r"\bsuspended (?P<us>\d+) us\b")
FZ_STALL_EDVR_RE = re.compile(r"EDVR code on the stack: (?P<yes>yes|no)\b(?: \((?P<frames>\d+) frames?, innermost (?P<where>[^)]+)\))?")
FZ_STALL_FAIL_RE = re.compile(
    r"stall: sample (?P<k>\d+) of (?P<of>\d+) at (?P<at>[0-9.]+) ms failed: (?P<why>[^;]+?); last Present returned in frame (?P<frame>\d+), "
    r"thread (?P<tid>\d+)")
FZ_GPU_HEAD = "EDVR GPU census, frame gap:"
FZ_GPU_STALL_RE = re.compile(
    r"(?P<n>\d+) pairs? over 1 s KEPT as stalls \([^)]*?longest (?P<longest>[0-9.]+) ms[^)]*\):(?P<pairs>.*)$")
FZ_GPU_PAIR_RE = re.compile(r"\bsequence (?P<seq>\d+) (?P<ms>[0-9.]+) ms")
FZ_GPU_MORE_RE = re.compile(r"\(and (?P<n>\d+) more\)")
FZ_UTC_RE = re.compile(r"^(\d{4})-(\d\d)-(\d\d)T(\d\d):(\d\d):(\d\d)\.(\d{3})Z$")
# The phases of a long cycle that can hold its time, in the order the line writes them. When the Present split is unavailable the three
# between the second Submit and the next wait are one phase.
FZ_PHASES_BEFORE = ("game_before_first_submit", "first_submit_roundtrip", "between_eye_calls", "second_submit_roundtrip")
FZ_PHASES_SPLIT = ("pre_present", "present_hook", "post_present")
FZ_PHASE_UNSPLIT = "post_second_submit_to_next_wait"
FZ_PHASES_AFTER = ("next_wait_roundtrip",)


def _fz_unparsed(fz, msg):
    """A line that opens like one of the freeze lines but matches none of the formats: the C++ wording changed. Counted and shown, as _unparsed does
    for the periodic report."""
    fz["unparsed"]["count"] += 1
    if len(fz["unparsed"]["samples"]) < 2:
        fz["unparsed"]["samples"].append(msg[:120])


def _fz_fields(msg):
    """A runtime log line's comma-separated key=value fields, its first word (the line's name) as `_name`. The first of a repeated key wins."""
    parts = msg.strip().split(",")
    out = {"_name": parts[0]}
    for part in parts[1:]:
        key, sep, val = part.partition("=")
        key = key.strip()
        if sep and key and key not in out:
            out[key] = val.strip()
    return out


def _fz_num(fields, key):
    """A field as a float, or None when the line does not have it (an older build) or it is not a number."""
    try:
        return float(fields[key])
    except (KeyError, ValueError):
        return None


def _fz_ints(fields):
    """The all-digit fields of a runtime counts line as ints (count, logged, candidates, long_lt50, ...)."""
    return {k: int(v) for k, v in fields.items() if k != "_name" and v.isdigit()}


def _fz_utc(text):
    """The naive UTC datetime of a worst-list line's `utc=YYYY-MM-DDTHH:MM:SS.mmmZ`, or None (`utc=unknown`, or not a date)."""
    u = FZ_UTC_RE.match(text)
    if not u:
        return None
    try:
        return datetime.datetime(int(u.group(1)), int(u.group(2)), int(u.group(3)), int(u.group(4)), int(u.group(5)), int(u.group(6)),
                                 int(u.group(7)) * 1000)
    except ValueError:
        return None


def _fz_worst_add(sets, entry):
    """File one worst-list line. The lines of one set are written together with rising ranks, so a rank that does not rise starts the next set (a
    later set replaces an earlier one: the list is reprinted when it changed, and at the end)."""
    if not sets or entry["rank"] <= sets[-1][-1]["rank"]:
        sets.append([entry])
    else:
        sets[-1].append(entry)


def _fz_gfx_line(fz, msg, t):
    """File one graphics log line (the text after its prefix, t its time on the common clock) if it is one of the freeze lines. LONG FRAME lines are
    scan_flight_log's own (its frames), read from there. The headset-lock arc's lines (vram, slow test) and the reconstruction's (a LONG FRAME's period, vScreen
    totals) are _fz_arc_gfx_line's."""
    if _fz_arc_gfx_line(fz, msg, t):
        return
    if msg.startswith("monitor: FREEZE -- "):
        m = FZ_FREEZE_RE.match(msg)
        if not m:
            _fz_unparsed(fz, msg)
            return
        cyc = FZ_FREEZE_CYCLE_RE.match(m.group("cycle"))
        smp = FZ_FREEZE_SAMPLER_RE.search(msg, m.end())
        fz["freezes"].append({
            "t": t, "ms": float(m.group("ms")), "frame": int(m.group("frame")), "seq": int(m.group("seq")), "n": int(m.group("n")),
            "cycle_ms": float(cyc.group("ms")) if cyc else None, "head_ms": float(cyc.group("head")) if cyc else None,
            "prev_ms": float(cyc.group("prev")) if cyc else None, "sampler": smp.group("state") if smp else None})
    elif msg.startswith("monitor: long frame counts "):
        m = FZ_GFX_COUNTS_RE.match(msg)
        kv = {k: int(v) for k, v in FZ_KV_RE.findall(m.group("kv"))} if m else {}
        if not m or "candidates" not in kv:
            _fz_unparsed(fz, msg)
            return
        fz["gfx_counts"].append({"t": t, "reason": m.group("reason"), "kv": kv})
    elif msg.startswith("monitor: worst long frame "):
        m = FZ_GFX_WORST_RE.match(msg)
        if not m:
            _fz_unparsed(fz, msg)
            return
        cycle = m.group("cycle")
        _fz_worst_add(fz["gfx_worst_sets"], {
            "t": t, "rank": int(m.group("rank")), "of": int(m.group("of")), "ms": float(m.group("ms")), "ended": m.group("ended"),
            "frame": int(m.group("frame")), "seq": int(m.group("seq")), "cycle_ms": None if cycle == "unavailable" else float(cycle.split()[0]),
            "note": m.group("note")})
    elif msg.startswith("stall sampler: "):
        m = FZ_SAMPLER_STATE_RE.match(msg)
        if not m:
            _fz_unparsed(fz, msg)
            return
        fz["sampler_state"] = {"t": t, "state": "failed" if m.group("state") == "could not start" else m.group("state"), "text": m.group("text")}
    elif msg.startswith("freeze test: "):
        armed, slept = FZ_TEST_ARMED_RE.match(msg), FZ_TEST_SLEEP_RE.match(msg)
        if not (armed or slept):
            _fz_unparsed(fz, msg)
            return
        fz["tests"].append({"kind": "armed" if armed else "sleep", "ms": int((armed or slept).group("ms")), "t": t})
    elif msg.startswith("stall sampler counts "):
        m = FZ_STALL_COUNTS_RE.match(msg)
        if not m:
            _fz_unparsed(fz, msg)
            return
        fz["stall_counts"].append({"t": t, "reason": m.group("reason"), "kv": {k: int(v) for k, v in FZ_KV_RE.findall(m.group("kv"))}})
    elif msg.startswith("stall: "):
        m = FZ_STALL_RE.match(msg)
        if m:
            sus = FZ_STALL_SUSPEND_RE.search(msg, m.end())
            if not sus:
                _fz_unparsed(fz, msg)
                return
            edvr = FZ_STALL_EDVR_RE.search(msg, m.end())
            fz["stalls"].append({
                "kind": "stall", "t": t, "age_ms": float(m.group("age")), "top": m.group("top").strip(), "owner": m.group("owner").strip(),
                "stack": [p.strip() for p in m.group("stack").split(" < ") if p.strip()], "k": int(m.group("k")), "of": int(m.group("of")),
                "frame": int(m.group("frame")), "tid": int(m.group("tid")), "us": int(sus.group("us")),
                "outside": "outside the thread's stack" in msg[m.end():],
                "edvr": None if not edvr else edvr.group("yes") == "yes",
                "edvr_frames": int(edvr.group("frames")) if edvr and edvr.group("frames") else None,
                "edvr_where": edvr.group("where").strip() if edvr and edvr.group("where") else None})
            return
        m = FZ_STALL_FAIL_RE.match(msg)
        if not m:
            _fz_unparsed(fz, msg)
            return
        fz["stall_fails"].append({
            "kind": "fail", "t": t, "k": int(m.group("k")), "of": int(m.group("of")), "at_ms": float(m.group("at")), "why": m.group("why").strip(),
            "frame": int(m.group("frame")), "tid": int(m.group("tid"))})
    elif msg.startswith(FZ_GPU_HEAD) and " KEPT as stalls" in msg:
        m = FZ_GPU_STALL_RE.search(msg)
        if not m:
            _fz_unparsed(fz, msg)
            return
        more = FZ_GPU_MORE_RE.search(m.group("pairs"))
        fz["gpu"].append({
            "t": t, "n": int(m.group("n")), "longest_ms": float(m.group("longest")), "more": int(more.group("n")) if more else 0,
            "pairs": [{"seq": int(p.group("seq")), "ms": float(p.group("ms"))} for p in FZ_GPU_PAIR_RE.finditer(m.group("pairs"))]})


def _fz_rt_line(fz, msg, t):
    """File one runtime log line (the text after its UTC prefix, t its time converted to local) if it is one of the freeze lines. A worst-list
    line's own time is when the list was written; the cycle's end is its `utc=` field, turned to local once the clock is known (parse_freezes). The headset-lock
    arc's lines and the older lines its reconstruction reads (native_submit_phases, display_frequency, native_producer_gpu) are _fz_arc_rt_line's."""
    if _fz_arc_rt_line(fz, msg, t):
        return
    if msg.startswith("native_long_cycle,") or msg.startswith("native_long_cycle_worst,"):
        worst = msg.startswith("native_long_cycle_worst,")
        f = _fz_fields(msg)
        try:
            seq, ms = int(f["sequence"]), float(f["cycle_ms"])
            rank, of = (int(f["rank"]), int(f["of"])) if worst else (0, 0)
        except (KeyError, ValueError):
            _fz_unparsed(fz, msg)
            return
        if not worst:
            fz["cycles"].append({"src": "line", "t": t, "seq": seq, "ms": ms, "f": f, "key": (seq, f["cycle_ms"])})
            return
        _fz_worst_add(fz["rt_worst_sets"], {
            "line_t": t, "t": None, "rank": rank, "of": of, "seq": seq, "ms": ms, "f": f, "key": (seq, f["cycle_ms"]),
            "utc": _fz_utc(f.get("utc", ""))})
    elif msg.startswith("native_long_cycle_counts,") or msg.startswith("native_long_cycle_summary,"):
        f = _fz_fields(msg)
        kv = _fz_ints(f)
        if "count" not in kv:
            _fz_unparsed(fz, msg)
            return
        fz["rt_counts"].append({"t": t, "kind": "summary" if msg.startswith("native_long_cycle_summary,") else "counts",
                                "reason": f.get("reason"), "kv": kv})


# ---- the headset-lock arc (docs/headset-lock-vdxr-2026-10-02.md): a slow regime, and who holds it ----
# A Quest 3 user's game ran at 10 fps for a minute, every frame's xrEndFrame held ~83 ms by the vendor's runtime, and every line before this arc was blind to it:
# 100 ms frames are under the 250 ms FREEZE line, and the LONG FRAME line is one in five seconds. The build that follows writes, as the C++ formats them:
#
#   graphics log   `vram: reason=armed|periodic|pressure|over_budget|back_under_budget|cap_reached local_used_mb=N local_budget_mb=N local_pct=P
#                  nonlocal_used_mb=N nonlocal_budget_mb=N nonlocal_pct=P[; ...]` and `vram: unavailable -- <why>. No vram: line will be written this
#                  session.` (vram_watch.h, vram_tick.cpp); `slow test: ...` (perf_monitor.cpp slowTestTick, advanced.slow_test_ms).
#   runtime log    `native_xr_events,armed=1,...`, `native_xr_event,n=N,type=...`, `native_xr_events_summary,...` (vendor_events.h);
#                  `native_end_frame_episodes,armed=1,...`, `native_end_frame_episode,episode=N,...`, `native_end_frame_episode_end,...`,
#                  `native_end_frame_episodes_summary,...` (end_frame_episodes.h); `native_slow_regime,armed=1,...`, `native_slow_regime,event=SLOW|
#                  still_slow|end,...,summary=<words>`, `native_slow_regime_summary,...` (slow_regime.h); `native_end_frame_hold,test=1,state=began|
#                  ended,...` (native_runtime_host.h holdEndFrame).
#
# A build from BEFORE them (v0.18.0) wrote none of that, but its older lines still hold the regime, and this reads it from them, labelled a reconstruction:
# the LONG FRAME lines of 3 display periods or more (the period is on the line), the FREEZE lines, the runtime's native_long_cycle lines of 3 periods or
# more (all three carry the runtime sequence), and the graphics log's `vScreen totals ... N frames in M ms is X fps` windows under 40% of the display's
# rate. Marks (the first three) and slow windows within FZ_SLOW_LINK_S of each other are one run. A run of FZ_SLOW_MIN_S or more is a regime when its frame
# rate is under 40% of the display's: the rate is how far the runtime sequence rose over the run's time, and the same for the graphics frame counter (the
# median of the two when both are there: long frames now and then with a fine rate between them are hitches, not a regime), and the vScreen windows' own
# rates when no counter can say. WHO comes from the runtime's native_submit_phases windows (their `first=..,last=..` sequences overlap the run), the p50
# of each phase against the frame's length. The rate limits (LONG FRAME is one in five seconds, 60 a session) mean a run's start and end are its first
# and last line: within a few seconds of the real ones, not sharper.
FZ_SLOW_FRACTION = 0.40         # slow_regime.h kSlowFraction: a frame rate under this share of the display's is slow
FZ_SLOW_PERIODS = 3.0           # end_frame_episodes.h kEndFramePeriods: a frame or call of this many display periods or more is a mark
FZ_SLOW_LINK_S = 5.5            # marks and windows this close are one run (the LONG FRAME limiter writes one every 5 s)
FZ_SLOW_MIN_S = 5.0             # slow_regime.h kSlowHoldSeconds: a run shorter than this is a hitch, not a regime
FZ_SLOW_SHARE = 0.35            # slow_regime.h kSlowOwnerShare: an owner holding less of a frame than this is not named
FZ_SLOW_EVENT_PAD_S = 10.0      # vendor events this close to a regime are listed with it
FZ_SLOW_TEST_PAD_S = 15.0       # a regime that begins within this of the test hold's start is the test's
FZ_SLOW_PHASE_SLACK_S = 30.0    # when no sequence says, a native_submit_phases window that ends this long after a run still covers it
FZ_OWNER_TEXT = {               # slow_regime.h slowOwnerText, key for key
    "vendor_end_frame": "the vendor runtime's xrEndFrame",
    "vendor_wait_frame": "the vendor runtime's xrWaitFrame and xrBeginFrame (the pose wait)",
    "vendor_swapchain": "the vendor runtime's swapchain calls (xrAcquire, xrWait, xrReleaseSwapchainImage)",
    "edvr_copy": "EDVR's copy of the eyes",
    "edvr_work": "EDVR's own work in the runtime",
    "game": "the game's own frame",
    "no_frames": "nothing: no frame reached xrEndFrame",
    "none": "no single owner",
}
FZ_OWNER_ORDER = ("vendor_end_frame", "vendor_wait_frame", "vendor_swapchain", "edvr_copy", "edvr_work", "game")
# Who may set SLOW. A regime the vendor runtime, EDVR's copy of the eyes or EDVR's own work holds is a finding about the runtime or EDVR. One the game's own
# frame holds is the game's: a load (INFO) when the runtime's frames were its own loading frames or ends with no layers (slow_regime.h `context=`), a slow
# stretch of the game's scenes (WARN) otherwise. A regime with no named owner (none, no_frames, unknown) is a WARN: nothing says whose it is.
FZ_SLOW_OWNERS = ("vendor_end_frame", "vendor_wait_frame", "vendor_swapchain", "edvr_copy", "edvr_work")
FZ_LOAD_CONTEXTS = ("loading", "no_layers")
FZ_VRAM_REASON_RE = re.compile(r"^vram: reason=(?P<reason>\w+)\b")
FZ_VRAM_KV_RE = re.compile(r"\b(?P<k>(?:local|nonlocal)_(?:used_mb|budget_mb|pct))=(?P<v>[0-9.]+|-)")
FZ_VRAM_ADAPTER_RE = re.compile(r"; adapter (?P<name>.+?); this process's")
FZ_VRAM_UNAVAILABLE_RE = re.compile(r"^vram: unavailable -- (?P<why>.*?)\. No vram: line")
FZ_VRAM_KEYS = ("local_used_mb", "local_budget_mb", "local_pct", "nonlocal_used_mb", "nonlocal_budget_mb", "nonlocal_pct")
FZ_SLOWTEST_ARMED_RE = re.compile(
    r"^slow test: advanced\.slow_test_ms = (?P<ms>\d+)\. (?P<start>\d+) s from now the runtime holds every xrEndFrame it makes (?P<ms2>\d+) ms longer, "
    r"for (?P<span>\d+) s")
FZ_SLOWTEST_BEGAN_RE = re.compile(r"^slow test: the hold begins now: every xrEndFrame is held (?P<ms>\d+) ms longer for (?P<span>\d+) s\.")
FZ_SLOWTEST_ENDED_RE = re.compile(r"^slow test: the hold ended\.")
FZ_LF_PERIOD_RE = re.compile(r"^monitor: LONG FRAME -- (?P<ms>[0-9.]+) ms between Presents \((?:runtime predicted period|budget) (?P<p>[0-9.]+)")
FZ_LF_FRAME_RE = re.compile(r"\bThis is frame (?P<frame>\d+)\b")
FZ_VSCREEN_FPS_RE = re.compile(r"^vScreen totals: .*?\b(?P<n>\d+) frames in (?P<ms>\d+) ms is (?P<fps>\d+) fps")
# The runtime's lines of the arc by the name each begins with, then the older lines the reconstruction reads (their malformed forms are ignored, not counted).
FZ_RT_ARC_NAMES = ("native_xr_events", "native_xr_event", "native_xr_events_summary", "native_end_frame_episodes", "native_end_frame_episode",
                   "native_end_frame_episode_end", "native_end_frame_episodes_summary", "native_slow_regime", "native_slow_regime_summary",
                   "native_end_frame_hold", "native_submit_phases", "display_frequency", "native_producer_gpu")
# The phases of native_submit_phases the owner attribution reads: four numbers each, p50/p95/p99/max (the line's own `percentiles=` says so).
FZ_PHASE_KEYS = ("producer_dispatch", "receive", "xr_acquire", "xr_wait", "xr_draw_submit", "xr_release", "xr_end_frame", "wait_frame", "pacer_block")


def _fz_arc_state():
    """The keys parse_freezes adds for the arc: the armed lines (None until seen), the lines of each instrument, and the older lines the reconstruction reads
    (phases, display_hz, producer_gpu, lf_marks, vscreen)."""
    return {"xr_armed": None, "xr_events": [], "xr_summaries": [], "ep_armed": None, "episodes": [], "episode_ends": [], "ep_summaries": [],
            "slow_armed": None, "slow_lines": [], "slow_summaries": [], "holds": [], "phases": [], "display_hz": None, "producer_gpu": [],
            "vram": [], "vram_state": None, "vram_fail": None, "slowtests": [], "lf_marks": [], "vscreen": []}


def _fz_int(fields, key):
    """A field as an int, or None."""
    try:
        return int(fields[key])
    except (KeyError, ValueError):
        return None


def _fz_p50s(fields):
    """{phase: [p50, p95, p99, max]} of a native_submit_phases line's phases that carry four slash-separated numbers; one that does not is left out."""
    out = {}
    for key in FZ_PHASE_KEYS:
        parts = str(fields.get(key, "")).split("/")
        if len(parts) == 4:
            try:
                out[key] = [float(p) for p in parts]
            except ValueError:
                pass
    return out


def _fz_vram_fields(fields):
    """The `vram_*` figures of a SLOW line as {local_used_mb: n, ...} (None for a `-`), or None when the line says `vram=unavailable`."""
    if not any(("vram_" + k) in fields for k in FZ_VRAM_KEYS):
        return None
    out = {}
    for k in FZ_VRAM_KEYS:
        try:
            out[k] = float(fields["vram_" + k])
        except (KeyError, ValueError):
            out[k] = None
    return out


def _fz_arc_rt_line(fz, msg, t):
    """File one runtime log line of the arc (and of the older lines its reconstruction reads), the text after the UTC prefix, t its time on the common
    clock. True when the line was one of these. A line that opens like one of the arc's and matches nothing is counted as unparsed, never dropped."""
    name = msg.split(",", 1)[0]
    if name not in FZ_RT_ARC_NAMES:
        return False
    f = _fz_fields(msg)
    try:
        if name == "native_xr_events":
            fz["xr_armed"] = {"t": t, "f": f}
        elif name == "native_xr_event":
            if f.get("suppressed") == "1":
                fz["xr_events"].append({"t": t, "f": f, "n": int(f["n"]), "type": None, "suppressed": True})
            else:
                fz["xr_events"].append({"t": t, "f": f, "n": int(f["n"]), "type": f["type"], "suppressed": False})
        elif name == "native_xr_events_summary":
            fz["xr_summaries"].append({"t": t, "f": f, "n": int(f["received"])})
        elif name == "native_end_frame_episodes":
            fz["ep_armed"] = {"t": t, "f": f}
        elif name == "native_end_frame_episode":
            fz["episodes"].append({"t": t, "f": f, "n": int(f["episode"]), "ms": float(f["ms"]), "seq": int(f["sequence"]), "periods": float(f["periods"])})
        elif name == "native_end_frame_episode_end":
            fz["episode_ends"].append({"t": t, "f": f, "n": int(f["episode"]), "slow": int(f["slow_calls"]), "calls": int(f["calls"]),
                                       "duration_ms": float(f["duration_ms"]), "max_ms": float(f["max_ms"]), "p50_ms": float(f["p50_ms"])})
        elif name == "native_end_frame_episodes_summary":
            fz["ep_summaries"].append({"t": t, "f": f, "n": int(f["episodes"])})
        elif name == "native_slow_regime":
            if f.get("armed") == "1":
                fz["slow_armed"] = {"t": t, "f": f}
            else:
                event = f["event"]
                if event not in ("SLOW", "still_slow", "end"):
                    raise ValueError(event)
                fz["slow_lines"].append({"t": t, "f": f, "event": event, "regime": int(f["regime"]), "duration_s": float(f["duration_s"]),
                                         "window_s": float(f["window_s"]), "frames": int(f["frames"]), "fps": float(f["fps"]), "hz": float(f["display_hz"]),
                                         "fraction": float(f["fraction"]), "frame_ms": float(f["frame_ms"]), "owner": f["held_by"],
                                         "share": float(f["held_share"]), "vendor_share": float(f["vendor_share"]), "vram": _fz_vram_fields(f),
                                         "context": f.get("context"), "loading_frames": _fz_int(f, "loading_frames"), "empty_frames": _fz_int(f, "empty_frames"),
                                         "summary": msg.partition(",summary=")[2]})
        elif name == "native_slow_regime_summary":
            fz["slow_summaries"].append({"t": t, "f": f, "regimes": int(f["regimes"])})
        elif name == "native_end_frame_hold":
            fz["holds"].append({"t": t, "f": f, "state": f["state"]})
        elif name == "native_submit_phases":
            p = _fz_p50s(f)
            if "xr_end_frame" in p:
                fz["phases"].append({"t": t, "window": f.get("window", "?"), "first": _fz_int(f, "first"), "last": _fz_int(f, "last"), "p": p})
        elif name == "display_frequency":
            hz = _fz_num(f, "hz")
            if hz and f.get("source") == "runtime":
                fz["display_hz"] = hz
        elif name == "native_producer_gpu":
            parts = str(f.get("copy", "")).split("/")
            if "window" in f and len(parts) == 4:
                fz["producer_gpu"].append({"t": t, "window": f["window"], "copy": [float(p) for p in parts]})
    except (KeyError, ValueError):
        if name in FZ_RT_ARC_NAMES[:10]:
            _fz_unparsed(fz, msg)
    return True


def _fz_arc_gfx_line(fz, msg, t):
    """File one graphics log line of the arc (vram, the slow test's trigger) or of the reconstruction (a LONG FRAME's period, sequence and frame; a vScreen
    totals window), the text after the prefix. True when the line needs nothing more; a LONG FRAME line returns False, its other readers are the scan's."""
    if msg.startswith("vram: "):
        m = FZ_VRAM_REASON_RE.match(msg)
        if m:
            kv = {k: (None if v == "-" else float(v)) for k, v in FZ_VRAM_KV_RE.findall(msg.partition(";")[0])}
            if "local_used_mb" not in kv:
                _fz_unparsed(fz, msg)
                return True
            ad = FZ_VRAM_ADAPTER_RE.search(msg)
            entry = {"t": t, "reason": m.group("reason"), "kv": kv, "adapter": ad.group("name") if ad else None}
            if entry["reason"] == "armed":
                fz["vram_state"] = {"t": t, "state": "armed", "adapter": entry["adapter"], "text": msg}
            fz["vram"].append(entry)
            return True
        m = FZ_VRAM_UNAVAILABLE_RE.match(msg)
        if m:
            fz["vram_state"] = {"t": t, "state": "unavailable", "adapter": None, "text": m.group("why")}
            return True
        if msg.startswith("vram: QueryVideoMemoryInfo failed"):
            fz["vram_fail"] = {"t": t, "text": msg[:160]}
            return True
        _fz_unparsed(fz, msg)
        return True
    if msg.startswith("slow test: "):
        armed, began, ended = FZ_SLOWTEST_ARMED_RE.match(msg), FZ_SLOWTEST_BEGAN_RE.match(msg), FZ_SLOWTEST_ENDED_RE.match(msg)
        if armed:
            fz["slowtests"].append({"kind": "armed", "t": t, "ms": int(armed.group("ms")), "span": int(armed.group("span")), "start": int(armed.group("start"))})
        elif began:
            fz["slowtests"].append({"kind": "began", "t": t, "ms": int(began.group("ms")), "span": int(began.group("span")), "start": 0})
        elif ended:
            fz["slowtests"].append({"kind": "ended", "t": t, "ms": 0, "span": 0, "start": 0})
        else:
            _fz_unparsed(fz, msg)
        return True
    if msg.startswith("monitor: LONG FRAME -- "):
        m = FZ_LF_PERIOD_RE.match(msg)
        if m:
            seq, frame = LONG_FRAME_SEQ_RE.search(msg), FZ_LF_FRAME_RE.search(msg)
            fz["lf_marks"].append({"t": t, "ms": float(m.group("ms")), "period": float(m.group("p")), "seq": int(seq.group("seq")) if seq else 0,
                                   "frame": int(frame.group("frame")) if frame else 0})
        return False
    if msg.startswith("vScreen totals: "):
        m = FZ_VSCREEN_FPS_RE.match(msg)
        if m and int(m.group("ms")) > 0:
            fz["vscreen"].append({"t": t, "frames": int(m.group("n")), "ms": int(m.group("ms")), "fps": int(m.group("n")) * 1000.0 / int(m.group("ms"))})
        return True
    return False


def parse_freezes(gfx_path, gfx_text, rt_path=None, rt_text=None):
    """What --freezes reads out of a flight's two logs, on the one local clock (scan_flight's). freezes: the FREEZE lines. long_frames: the LONG FRAME
    lines (scan_flight_log's, with their runtime sequence). gfx_counts / rt_counts: the counts lines (the runtime's include the summary), in file
    order. gfx_worst_sets / rt_worst_sets: the worst-list lines in sets, the last set the live one. sampler_state, stalls, stall_fails, stall_counts:
    the stall sampler's lines. gpu: the GPU census's kept stalls. tests: the freeze test trigger's lines (advanced.freeze_test_ms). cycles: the
    runtime's long cycles, one per (sequence, cycle_ms), a `native_long_cycle` line preferred over the worst-list line that repeats it. unparsed:
    lines that open like these and match nothing. gscan, rscan, clock are scan_flight's. The headset-lock arc's keys are _fz_arc_state's: the armed lines
    and each instrument's lines (xr_*, ep_*/episodes, slow_*, holds, vram*, slowtests) and the older lines the slow-regime reconstruction reads (phases,
    display_hz, producer_gpu, lf_marks, vscreen)."""
    fz = {"freezes": [], "gfx_counts": [], "gfx_worst_sets": [], "sampler_state": None, "stalls": [], "stall_fails": [], "stall_counts": [],
          "gpu": [], "tests": [], "cycles": [], "rt_worst_sets": [], "rt_counts": [], "unparsed": {"count": 0, "samples": []}}
    fz.update(_fz_arc_state())
    gscan, rscan, clock = scan_flight(gfx_path, gfx_text, rt_path, rt_text,
                                      lambda msg, t: _fz_gfx_line(fz, msg, t), lambda msg, t: _fz_rt_line(fz, msg, t))
    fz["gscan"], fz["rscan"], fz["clock"] = gscan, rscan, clock
    fz["long_frames"] = [f for f in gscan["frames"] if f["src"] == "gfx"]
    to_local = clock["to_local"]
    seen = {c["key"] for c in fz["cycles"]}
    for entries in fz["rt_worst_sets"]:
        for e in entries:
            if e["utc"] is not None and to_local is not None:
                e["t"] = (e["utc"] + to_local - EPOCH).total_seconds()
            if e["key"] not in seen:
                seen.add(e["key"])
                fz["cycles"].append({"src": "worst", "t": e["t"], "seq": e["seq"], "ms": e["ms"], "f": e["f"], "key": e["key"]})
    fz["cycles"].sort(key=lambda c: (c["t"] is None, c["t"] or 0.0))
    return fz


def _fz_has_lines(fz):
    """Does the log hold any line of the freeze logging itself? The runtime's `native_long_cycle` line and its old three-field summary are not
    that: a build from before the freeze logging wrote both. A line that opened like one of these and could not be read counts, so a reader
    that drifted from the C++ says so instead of calling the build old."""
    return bool(fz["freezes"] or fz["gfx_counts"] or fz["gfx_worst_sets"] or fz["sampler_state"] or fz["stalls"] or fz["stall_fails"]
                or fz["stall_counts"] or fz["gpu"] or fz["tests"] or fz["rt_worst_sets"] or fz["unparsed"]["count"]
                or any(c["kind"] == "counts" or "candidates" in c["kv"] for c in fz["rt_counts"])
                or any(fz[k] for k in ("xr_armed", "xr_events", "xr_summaries", "ep_armed", "episodes", "episode_ends", "ep_summaries", "slow_armed", "slow_lines",
                                       "slow_summaries", "holds", "vram", "vram_state", "vram_fail", "slowtests")))


def _fz_paired(freeze, cycle):
    """Does a runtime cycle belong to a FREEZE line: its sequence within one of the freeze's, or (sequences unknown or apart) its time within
    FREEZE_CYCLE_S of the freeze's end. Both ends of a freeze are on the one local clock."""
    if freeze["seq"] and cycle["seq"] and abs(freeze["seq"] - cycle["seq"]) <= 1:
        return True
    return cycle["t"] is not None and abs(cycle["t"] - freeze["t"]) <= FREEZE_CYCLE_S


def _fz_phase(fields):
    """(name, ms) of the largest phase of a long cycle's fields, or None when it has none. Present is split into pre_present, present_hook and
    post_present when present_split is ok, and is one phase otherwise; a present_hook whose real Present call (hook_real_present) is more than half
    of it is named real_Present, and carries that call's ms."""
    names = list(FZ_PHASES_BEFORE) + (list(FZ_PHASES_SPLIT) if fields.get("present_split") == "ok" else [FZ_PHASE_UNSPLIT]) + list(FZ_PHASES_AFTER)
    vals = [(n, _fz_num(fields, n)) for n in names]
    vals = [(n, v) for n, v in vals if v is not None]
    if not vals:
        return None
    name, ms = max(vals, key=lambda nv: nv[1])
    if name == "present_hook":
        real = _fz_num(fields, "hook_real_present")
        if real is not None and real > ms / 2.0:
            return "real_Present", real
    return name, ms


def _fz_phase_text(fields):
    p = _fz_phase(fields)
    return "-" if p is None else "%s %.1f ms" % p


def _fz_module(where):
    """The module of a `module+0xrva` address."""
    return where.split("+", 1)[0]


def freeze_rows(fz):
    """One row per FREEZE line, joined: lf (its LONG FRAME line, the same runtime sequence or within FREEZE_LONG_FRAME_S, the one whose ms agrees
    first), cycle (the runtime's long cycle that goes with it, see _fz_paired; the same sequence first, a native_long_cycle line before a
    worst-list one), stalls (the stall sampler's lines taken in [end - gap - pad, end + pad], by time) and gpu (the GPU census's kept stalls with
    its runtime sequence), test (the freeze test trigger's sleep line inside it, or None). Returns (rows, loose): loose is the stall samples that belong
    to no freeze."""
    rows = []
    for i, f in enumerate(fz["freezes"], 1):
        near = [lf for lf in fz["long_frames"] if (f["seq"] and lf["seq"] == f["seq"]) or abs(lf["t"] - f["t"]) <= FREEZE_LONG_FRAME_S]
        lf = min(near, key=lambda lf: (abs(lf["ms"] - f["ms"]) > 0.05, abs(lf["t"] - f["t"]))) if near else None
        cycles = [c for c in fz["cycles"] if _fz_paired(f, c)]
        cycle = min(cycles, key=lambda c: (abs(c["seq"] - f["seq"]) if f["seq"] and c["seq"] else 99, c["src"] != "line",
                                           abs(c["t"] - f["t"]) if c["t"] is not None else 1e9)) if cycles else None
        gpu = [p for g in fz["gpu"] for p in g["pairs"] if f["seq"] and p["seq"] == f["seq"]]
        # The test trigger's sleep (written just before the thread sleeps) lies in the freeze it makes: after the Present that began it.
        test = next((t for t in fz["tests"] if t["kind"] == "sleep"
                     and f["t"] - f["ms"] / 1000.0 - FREEZE_STALL_PAD_S <= t["t"] <= f["t"] + FREEZE_STALL_PAD_S), None)
        rows.append({"i": i, "f": f, "lf": lf, "cycle": cycle, "stalls": [], "gpu": gpu, "test": test})
    loose = []
    for s in sorted(fz["stalls"] + fz["stall_fails"], key=lambda s: s["t"]):
        best = None
        for row in rows:
            f = row["f"]
            begin = f["t"] - f["ms"] / 1000.0
            if begin - FREEZE_STALL_PAD_S <= s["t"] <= f["t"] + FREEZE_STALL_PAD_S:
                key = (0 if begin <= s["t"] <= f["t"] else 1, abs(s["t"] - f["t"]))
                if best is None or key < best[0]:
                    best = (key, row)
        if best:
            best[1]["stalls"].append(s)
        else:
            loose.append(s)
    return rows, loose


def _fz_cycle_pairs(rows, cycles):
    """[(cycle, row or None)] for the runtime cycles of FREEZE_MS or more: the FREEZE row each goes with (_fz_paired), None when no FREEZE line
    does, which is `runtime only`."""
    return [(c, next((r for r in rows if _fz_paired(r["f"], c)), None)) for c in cycles if c["ms"] >= FREEZE_MS]


def _fz_cell(kv, key):
    """A counts-line value for a table cell: `-` when the line does not carry the key (an older build, a half with no such count, no line)."""
    return "-" if kv is None or key not in kv else str(kv[key])


def _fz_edvr_text(s):
    """A stall sample's `EDVR code on the stack`: yes (with the frame count and the innermost EDVR frame the line gives), no, or `-` for a sample
    line that does not say."""
    if s["edvr"] is None:
        return "-"
    if not s["edvr"]:
        return "no"
    bits = []
    if s["edvr_frames"] is not None:
        bits.append("%d frame%s" % (s["edvr_frames"], "" if s["edvr_frames"] == 1 else "s"))
    if s["edvr_where"]:
        bits.append("innermost %s" % s["edvr_where"])
    return "yes (%s)" % ", ".join(bits) if bits else "yes"


def _fz_counts_problems(kv, half):
    """What does not add up in one counts line, as sentences. freeze_book.h keeps these identities by construction, so a line that breaks one has
    been cut, or its writer has a bug. A key the line lacks is nothing to add up."""
    out = []
    longs = ["long_" + k for k, _ in FREEZE_BUCKETS]
    blips = ["blip_" + k for k, _ in FREEZE_BUCKETS]
    if "long" in kv and all(k in kv for k in longs) and sum(kv[k] for k in longs) != kv["long"]:
        out.append("long=%d but its size buckets add to %d" % (kv["long"], sum(kv[k] for k in longs)))
    if half == "graphics" and "blips" in kv and all(k in kv for k in blips) and sum(kv[k] for k in blips) != kv["blips"]:
        out.append("blips=%d but its size buckets add to %d" % (kv["blips"], sum(kv[k] for k in blips)))
    if half == "graphics" and all(k in kv for k in ("candidates", "long", "blips")) and kv["candidates"] != kv["long"] + kv["blips"]:
        out.append("candidates=%d is not long=%d plus blips=%d" % (kv["candidates"], kv["long"], kv["blips"]))
    if half == "runtime" and all(k in kv for k in ("candidates", "long")) and kv["candidates"] != kv["long"]:
        out.append("candidates=%d is not long=%d" % (kv["candidates"], kv["long"]))
    if all(k in kv for k in ("long", "written", "unwritten")) and kv["written"] + kv["unwritten"] != kv["long"]:
        out.append("written=%d plus unwritten=%d is not long=%d" % (kv["written"], kv["unwritten"], kv["long"]))
    if all(k in kv for k in ("over_250ms", "long_250_1000", "long_ge1000")) and kv["over_250ms"] != kv["long_250_1000"] + kv["long_ge1000"]:
        out.append("over_250ms=%d is not long_250_1000=%d plus long_ge1000=%d" % (kv["over_250ms"], kv["long_250_1000"], kv["long_ge1000"]))
    return out


def _fz_row_summary(row, rt_read):
    """One line of what joined to a freeze whose full joins are left out of the report (see FREEZE_DETAIL_MAX), and what did not."""
    lf, c = row["lf"], row["cycle"]
    bits = ["LONG FRAME line %s" % ("yes" if lf is not None else "NO LONG FRAME LINE")]
    if c is not None:
        bits.append("runtime cycle %.1f ms, main phase %s" % (c["ms"], _fz_phase_text(c["f"])))
    else:
        bits.append("no runtime cycle" if rt_read else "runtime cycle - (no runtime log was read)")
    got = [s for s in row["stalls"] if s["kind"] == "stall"]
    if got:
        bits.append("%d stall sample(s), owner %s" % (len(got), ", ".join(sorted({s["owner"] for s in got}))))
    elif row["stalls"]:
        bits.append("%d failed stall sample(s)" % len(row["stalls"]))
    if row["gpu"]:
        bits.append("GPU clock kept %.1f ms as a stall" % row["gpu"][0]["ms"])
    if row["test"] is not None:
        bits.append("holds the deliberate test sleep of %d ms" % row["test"]["ms"])
    return "; ".join(bits)


def _fz_print_freezes(rows, rt_read, clk):
    # A bad flight can hold hundreds of freezes: every one gets its row, and the longest FREEZE_DETAIL_MAX their full joins.
    detail = {r["i"] for r in sorted(rows, key=lambda r: (-r["f"]["ms"], r["i"]))[:FREEZE_DETAIL_MAX]}
    print("FREEZES (%d FREEZE line(s); each covers [end - gap, end]%s):" % (
        len(rows), "; the joins are printed in full for the %d longest" % FREEZE_DETAIL_MAX if len(rows) > FREEZE_DETAIL_MAX else ""))
    print("  %-3s %-14s %9s %8s %9s  %s" % ("#", "end (local)", "gap ms", "frame", "sequence", "runtime cycle ms"))
    for row in rows:
        f, lf, c = row["f"], row["lf"], row["cycle"]
        print("  %-3d %-14s %9.1f %8d %9d  %s" % (row["i"], clk(f["t"]), f["ms"], f["frame"], f["seq"],
                                                  "unavailable" if f["cycle_ms"] is None else "%.1f" % f["cycle_ms"]))
        if row["i"] not in detail:
            print("      joins: %s" % _fz_row_summary(row, rt_read))
            continue
        if f["head_ms"] is not None:
            print("      the graphics half's view: %.1f ms from the pose wait's return to this Present, the previous cycle %.1f ms"
                  % (f["head_ms"], f["prev_ms"]))
        if lf is not None:
            print("      LONG FRAME line: %s, %.1f ms, runtime sequence %s" % (clk(lf["t"]), lf["ms"], lf["seq"] or "-"))
        else:
            print("      LONG FRAME line: NO LONG FRAME LINE (a LONG FRAME line is written first, and from 250 ms up not under the rate limit)")
        if c is not None:
            split = c["f"].get("present_split")
            print("      runtime cycle: %s sequence %d, %.1f ms, ended %s, main phase %s%s"
                  % ("native_long_cycle" if c["src"] == "line" else "native_long_cycle_worst", c["seq"], c["ms"], clk(c["t"]), _fz_phase_text(c["f"]),
                     "" if split in (None, "ok") else " (Present split unavailable: %s)" % split))
        elif rt_read:
            print("      runtime cycle: none with sequence %s or within one of it, or ending within %.0f ms of this freeze"
                  % (f["seq"] or "-", FREEZE_CYCLE_S * 1000.0))
        else:
            print("      runtime cycle: - (no runtime log was read)")
        for s in row["stalls"]:
            if s["kind"] == "stall":
                print("      stall sample %d of %d at %s: stalled %g ms in %s, owner %s, suspended %d us, EDVR code on the stack: %s%s"
                      % (s["k"], s["of"], clk(s["t"]), s["age_ms"], s["top"], s["owner"], s["us"], _fz_edvr_text(s),
                         "; the stack pointer was outside the thread's stack: one frame only" if s["outside"] else ""))
            else:
                print("      stall sample %d of %d at %s: FAILED at %g ms (%s)" % (s["k"], s["of"], clk(s["t"]), s["at_ms"], s["why"]))
        if f["sampler"] is not None:
            print("      the FREEZE line's stall sampler clause: %s" % f["sampler"])
        for p in row["gpu"]:
            print("      GPU clock: the GPU census kept the gap before sequence %d, %.1f ms, as a stall" % (p["seq"], p["ms"]))
        if row["test"] is not None:
            print("      freeze test: this freeze holds the deliberate sleep of %d ms (advanced.freeze_test_ms), logged at %s"
                  % (row["test"]["ms"], clk(row["test"]["t"])))


def _fz_print_cycles(pairs, clk):
    print("RUNTIME CYCLES of 250 ms or more (%d, one per sequence):" % len(pairs))
    print("  %-14s %9s %9s  %-36s %s" % ("end (local)", "sequence", "cycle ms", "main phase", "FREEZE line"))
    for c, row in pairs:
        print("  %-14s %9d %9.1f  %-36s %s%s" % (
            clk(c["t"]), c["seq"], c["ms"], _fz_phase_text(c["f"]), "runtime only" if row is None else "#%d (frame %d)" % (row["i"], row["f"]["frame"]),
            "" if c["src"] == "line" else "  [worst list only]"))


def _fz_print_counts(fz, rt_read, clk):
    g = fz["gfx_counts"][-1] if fz["gfx_counts"] else None
    r = fz["rt_counts"][-1] if rt_read and fz["rt_counts"] else None
    if g is None and r is None:
        return
    gk = g["kv"] if g else None
    rk = r["kv"] if r else None
    print("COUNTS (the latest counts line of each half: graphics %s; runtime %s):" % (
        "reason=%s at %s" % (g["reason"], clk(g["t"])) if g else "none",
        "%s%s at %s" % ("native_long_cycle_summary" if r["kind"] == "summary" else "native_long_cycle_counts",
                        " reason=%s" % r["reason"] if r["reason"] else "", clk(r["t"])) if r else "none"))
    print("  %-14s %9s %9s %13s  %8s %13s" % ("size", "gfx long", "gfx blips", "gfx unwritten", "rt long", "rt unwritten"))
    for key, words in FREEZE_BUCKETS:
        print("  %-14s %9s %9s %13s  %8s %13s" % (words, _fz_cell(gk, "long_" + key), _fz_cell(gk, "blip_" + key), _fz_cell(gk, "unwritten_" + key),
                                                  _fz_cell(rk, "long_" + key), _fz_cell(rk, "unwritten_" + key)))
    print("  %-14s %9s %9s %13s  %8s %13s" % ("total", _fz_cell(gk, "long"), _fz_cell(gk, "blips"), _fz_cell(gk, "unwritten"),
                                              _fz_cell(rk, "long"), _fz_cell(rk, "unwritten")))
    print("  graphics: candidates %s, written %s; over 250 ms %s, of them unwritten %s"
          % (_fz_cell(gk, "candidates"), _fz_cell(gk, "written"), _fz_cell(gk, "over_250ms"), _fz_cell(gk, "over_250ms_unwritten")))
    print("  runtime:  candidates %s, written %s; over 250 ms %s, of them unwritten %s; count %s, logged %s"
          % (_fz_cell(rk, "candidates"), _fz_cell(rk, "written"), _fz_cell(rk, "over_250ms"), _fz_cell(rk, "over_250ms_unwritten"),
             _fz_cell(rk, "count"), _fz_cell(rk, "logged")))
    print("  `-` = the line does not carry that count (from 250 ms up a frame is always written, so no unwritten count exists for it; blips are "
          "the graphics half's alone; an older runtime's summary has only count and logged)")
    for kv, half in ((gk, "graphics"), (rk, "runtime")):
        if kv:
            bad = _fz_counts_problems(kv, half)
            if bad:
                print("  the %s counts do not add up: %s" % (half, "; ".join(bad)))


def _fz_print_worst(fz, rt_read, clk):
    sets = fz["gfx_worst_sets"]
    if sets:
        last = sets[-1]
        print("WORST (graphics log: the latest of %d set(s), %d line(s), written %s):" % (len(sets), len(last), clk(last[0]["t"])))
        for e in last:
            print("  %d of %d  %9.1f ms  ended %s  frame %d  sequence %d  runtime cycle %s%s"
                  % (e["rank"], e["of"], e["ms"], e["ended"], e["frame"], e["seq"], "unavailable" if e["cycle_ms"] is None else "%.1f ms" % e["cycle_ms"],
                     "  %s" % e["note"] if e["note"] else ""))
    sets = fz["rt_worst_sets"]
    if sets and rt_read:
        last = sets[-1]
        print("WORST (runtime log: the latest of %d set(s), %d line(s), written %s):" % (len(sets), len(last), clk(last[0]["line_t"])))
        for e in last:
            print("  %d of %d  %9.1f ms  ended %s  sequence %d  main phase %s  (utc=%s)"
                  % (e["rank"], e["of"], e["ms"], clk(e["t"]), e["seq"], _fz_phase_text(e["f"]), e["f"].get("utc", "-")))


def _fz_print_sampler(fz, rows, loose, clk):
    st = fz["sampler_state"]
    samples, fails = fz["stalls"], fz["stall_fails"]
    if st is None and not (samples or fails or fz["stall_counts"]):
        return
    if st is not None:
        print("STALL SAMPLER: %s" % (st["text"] if len(st["text"]) <= 150 else st["text"][:147] + "..."))
    else:
        print("STALL SAMPLER: the log has no `stall sampler:` state line, but it has the sampler's other lines")
    if samples:
        owners = {}
        for s in samples:
            owners[_fz_module(s["owner"])] = owners.get(_fz_module(s["owner"]), 0) + 1
        inside = sum(1 for r in rows for s in r["stalls"] if s["kind"] == "stall")
        print("  samples by owner module (%d sample line(s), %d inside a freeze, %d outside any): %s"
              % (len(samples), inside, sum(1 for s in loose if s["kind"] == "stall"),
                 ", ".join("%s %d" % kv for kv in sorted(owners.items(), key=lambda kv: (-kv[1], kv[0])))))
        longest = max(samples, key=lambda s: s["us"])
        print("  longest suspended: %d us (sample %d of %d at %s)" % (longest["us"], longest["k"], longest["of"], clk(longest["t"])))
        outside = sum(1 for s in samples if s["outside"])
        if outside:
            print("  samples taken with the stack pointer outside the thread's stack (one frame only): %d of %d" % (outside, len(samples)))
        known = [s for s in samples if s["edvr"] is not None]
        mine = [s for s in samples if s["edvr"]]
        if not known:
            print("  EDVR code on the stack: - (no sample line says)")
        else:
            print("  EDVR code on the stack: %d of %d sample(s)%s" % (len(mine), len(known), "; EDVR's own code was in the freeze:" if mine else ""))
            for s in mine:
                print("    sample %d of %d at %s: %s" % (s["k"], s["of"], clk(s["t"]), _fz_edvr_text(s)))
    if fails:
        reasons = {}
        for s in fails:
            reasons[s["why"]] = reasons.get(s["why"], 0) + 1
        print("  failed samples (%d) by reason: %s" % (len(fails), ", ".join("%s %d" % kv for kv in sorted(reasons.items(), key=lambda kv: (-kv[1], kv[0])))))
    if fz["stall_counts"]:
        c = fz["stall_counts"][-1]
        print("  latest counts (reason=%s, %s): %s" % (c["reason"], clk(c["t"]), " ".join("%s=%d" % kv for kv in c["kv"].items())))


def _fz_print_gpu(fz, rows, clk):
    if not fz["gpu"]:
        return
    pairs = {}
    for g in fz["gpu"]:
        for p in g["pairs"]:
            pairs.setdefault((p["seq"], p["ms"]), p)
    print("GPU CLOCK (the GPU census kept %d frame-gap pair(s) over 1 s as stalls, in %d line(s); each is the gap on the GPU clock before that runtime "
          "sequence):" % (sum(g["n"] for g in fz["gpu"]), len(fz["gpu"])))
    for p in sorted(pairs.values(), key=lambda p: (p["seq"], p["ms"])):
        row = next((r for r in rows if r["f"]["seq"] and r["f"]["seq"] == p["seq"]), None)
        print("  sequence %d  %.1f ms  %s" % (p["seq"], p["ms"], "FREEZE #%d (frame %d, %.1f ms between Presents)" % (row["i"], row["f"]["frame"], row["f"]["ms"])
                                              if row else "no FREEZE line has this sequence"))
    unlisted = sum(g["more"] for g in fz["gpu"])
    if unlisted:
        print("  (and %d more the lines did not list)" % unlisted)


FZ_ARC_ROWS = 12                # the episode, event and VRAM sections print this many rows, then say how many more


def _fz_clock(fz):
    """The common-clock formatter print_freezes uses (HH:MM:SS.mmm, the day only when it is not the flight's first)."""
    day0 = int(fz["gscan"]["first"] // DAY)
    return lambda t: "-" if t is None else fmt_clock(t, day0)


def _fz_median(values):
    values = sorted(values)
    n = len(values)
    if not n:
        return None
    return values[n // 2] if n % 2 else 0.5 * (values[n // 2 - 1] + values[n // 2])


def _fz_vram_text(v):
    """`local 7421 of 10863 MB (68.3%), non-local 316 of 16311 MB (1.9%)` for a figures dict (None where the OS did not answer)."""
    def seg(used, budget, pct):
        if used is None:
            return "no answer"
        return "%d of %s MB%s" % (used, "%d" % budget if budget is not None else "-", " (%.1f%%)" % pct if pct is not None else "")
    return "local %s, non-local %s" % (seg(v.get("local_used_mb"), v.get("local_budget_mb"), v.get("local_pct")),
                                       seg(v.get("nonlocal_used_mb"), v.get("nonlocal_budget_mb"), v.get("nonlocal_pct")))


def _fz_rate(points):
    """(frames per second, (t1, c1, t2, c2)) from the first and last of (time, counter) points whose counter is above 0, or None when there are fewer than two,
    they lie under FZ_SLOW_MIN_S apart, or the counter did not rise (a session that restarted its count)."""
    pts = sorted((t, c) for t, c in points if c and c > 0)
    if len(pts) < 2:
        return None
    (t1, c1), (t2, c2) = pts[0], pts[-1]
    if t2 - t1 < FZ_SLOW_MIN_S or c2 <= c1:
        return None
    return (c2 - c1) / (t2 - t1), (t1, c1, t2, c2)


def _fz_legacy_owner(fz, begin, stop, seq_range, frame_ms, period_ms, rt_read, clk):
    """Who held the frames of a reconstructed run, from the runtime's native_submit_phases window that covers most of it (by the window's `first..last`
    sequences against the run's, else by the time it ended): the p50 of each phase against the frame's length, grouped as slow_regime.h groups them.
    Returns {key, share, held (the sentence), detail (printed lines)}."""
    unknown = {"key": "unknown", "share": None, "detail": []}
    if not rt_read:
        unknown["held"] = "owner unknown: no runtime log was read, so no native_submit_phases window says which call held the frames"
        return unknown
    best = None
    for w in fz["phases"]:
        first, last = w["first"], w["last"]
        if seq_range and first is not None and last is not None and last >= first:
            inside = min(last, seq_range[1]) - max(first, seq_range[0]) + 1
            if inside <= 0:
                continue
            total, rank = last - first + 1, (inside, w["t"])
        elif begin <= w["t"] <= stop + FZ_SLOW_PHASE_SLACK_S:
            inside, total, rank = None, None, (0, w["t"])
        else:
            continue
        if best is None or rank > best[0]:
            best = (rank, w, inside, total)
    if best is None:
        unknown["held"] = "owner unknown: the runtime log has no native_submit_phases window covering the run"
        return unknown
    _, w, inside, total = best
    p = {k: v[0] for k, v in w["p"].items()}
    shares = {"vendor_end_frame": p.get("xr_end_frame", 0.0), "vendor_wait_frame": p.get("wait_frame", 0.0) + p.get("pacer_block", 0.0),
              "vendor_swapchain": p.get("xr_acquire", 0.0) + p.get("xr_wait", 0.0) + p.get("xr_release", 0.0),
              "edvr_copy": p.get("producer_dispatch", 0.0) + p.get("receive", 0.0) + p.get("xr_draw_submit", 0.0), "edvr_work": 0.0}
    shares["game"] = max(0.0, frame_ms - sum(shares.values()))
    key = max(FZ_OWNER_ORDER, key=lambda k: shares[k])      # a tie goes to the first in the order, as the C++ does
    share = shares[key] / frame_ms
    owner = key if share >= FZ_SLOW_SHARE else "none"
    if owner == "none":
        held = "no single owner; the largest is %s at %.0f%% of a %.1f ms frame" % (FZ_OWNER_TEXT[key], 100.0 * share, frame_ms)
    else:
        held = "held by %s: %.1f ms of every %.1f ms frame (%.0f%%)" % (FZ_OWNER_TEXT[key], shares[key], frame_ms, 100.0 * share)
    periods = " = %.1f display periods of %.3f ms" % (shares["vendor_end_frame"] / period_ms, period_ms) if period_ms else ""
    where = "native_submit_phases window %s: sequences %s-%s, ended %s%s" % (
        w["window"], w["first"] if w["first"] is not None else "?", w["last"] if w["last"] is not None else "?", clk(w["t"]),
        "; %d of its %d frames are inside the run" % (inside, total) if inside is not None else "")
    detail = ["a frame went (p50 of each phase in %s): xrEndFrame %.1f ms%s, pose wait %.1f, swapchain calls %.1f, EDVR copy %.1f, the game %.1f (the rest)"
              % (where, shares["vendor_end_frame"], periods, shares["vendor_wait_frame"], shares["vendor_swapchain"], shares["edvr_copy"], shares["game"])]
    return {"key": owner, "share": share, "held": held, "detail": detail}


def _fz_legacy_gpu(fz, begin, stop, clk):
    """One corroborating line from the runtime's native_producer_gpu windows (EDVR's copy of the eyes on the GPU clock) that close in or just after the run,
    or None."""
    inside = [w for w in fz["producer_gpu"] if begin <= w["t"] <= stop + FZ_SLOW_PHASE_SLACK_S]
    if not inside:
        return None
    w = max(inside, key=lambda x: x["copy"][1])
    before = [x for x in fz["producer_gpu"] if x["t"] < begin]
    base = "; p95 %.2f ms in window %s, before it" % (before[-1]["copy"][1], before[-1]["window"]) if before else ""
    return ("GPU clock: native_producer_gpu window %s (ended %s): EDVR's copy of the eyes took p95 %.1f ms, max %.1f ms on the GPU%s"
            % (w["window"], clk(w["t"]), w["copy"][1], w["copy"][3], base))


def _fz_regime_class(g):
    """(status, words) of one regime, by who holds it. SLOW (status "SLOW") only when the vendor runtime, EDVR's copy or EDVR's own work holds most of each
    frame: a finding about the runtime or EDVR. The game's own frame is INFO (status "note") when the runtime says the frames were a load (its own loading
    frames, or ends with no layers: the `context=` of a native_slow_regime line), WARN when they were the game's scenes (a GPU- or CPU-bound stretch) or the
    line has no context; no named owner (none, no_frames, unknown) is a WARN. Neither sets SLOW, and neither exits 4."""
    owner = g["owner"]
    if owner in FZ_SLOW_OWNERS:
        return "SLOW", "held by %s" % ("EDVR" if owner.startswith("edvr_") else "the vendor runtime")
    if owner == "game":
        ctx = g.get("context")
        if ctx in FZ_LOAD_CONTEXTS:
            return "note", "the game's own frame during a load (the runtime's frames were %s)" % ("its own loading frames" if ctx == "loading" else "ends with no layers")
        if ctx == "scene":
            return "WARN", "the game's own frame while it submitted scenes: a GPU- or CPU-bound stretch of the game"
        return "WARN", "the game's own frame (the line does not say whether it was a load)"
    return "WARN", {"none": "no single owner holds 35% of a frame", "no_frames": "no frame reached xrEndFrame", "unknown": "no owner could be named"}.get(owner, "no named owner")


def _fz_slow_regimes_native(fz, clk):
    """The regimes the build wrote itself, one per `regime=N` of its native_slow_regime lines: SLOW is written once the frame rate has been under 40% for 5 s
    (its duration_s says how far back the regime began), still_slow every 30 s after, end when the rate recovered (or the session closed) with the whole
    regime's figures. A regime with no end line was still going when the log stopped (a crash, a kill, or the end of the file)."""
    by = {}
    for ln in fz["slow_lines"]:
        by.setdefault(ln["regime"], []).append(ln)
    out = []
    for n in sorted(by):
        lines = sorted(by[n], key=lambda l: l["t"])
        first = next((l for l in lines if l["event"] == "SLOW"), lines[0])
        end = next((l for l in lines if l["event"] == "end"), None)
        last = end or lines[-1]
        start = first["t"] - first["duration_s"]
        stop = start + last["duration_s"]
        f = last["f"]
        detail = ["rate: %.2f fps against the display's %.2f Hz (%.1f%% of it; a regime is under %.0f%%), frames of %.1f ms"
                  % (last["fps"], last["hz"], 100.0 * last["fraction"], 100.0 * FZ_SLOW_FRACTION, last["frame_ms"]),
                  "owner: %s" % last["summary"],
                  ("context: %s (%s loading frame(s) and %s end(s) with no layers of %d frames)"
                   % (last["context"], "?" if last["loading_frames"] is None else last["loading_frames"], "?" if last["empty_frames"] is None else last["empty_frames"],
                      last["frames"])) if last["context"] else "context: the line has none (a build from before it)",
                  "a frame went (mean ms over %s): xrEndFrame %s (longest %s), pose wait %s, swapchain calls %s, EDVR copy %s, EDVR work %s, the game %s"
                  % ("the whole regime" if end else "its last window", f.get("vendor_end_frame_ms", "-"), f.get("end_frame_max_ms", "-"),
                     f.get("vendor_wait_frame_ms", "-"), f.get("vendor_swapchain_ms", "-"), f.get("edvr_copy_ms", "-"), f.get("edvr_work_ms", "-"),
                     f.get("game_ms", "-"))]
        v1, v2 = first["vram"], last["vram"]
        if v1 is None and v2 is None:
            detail.append("graphics memory: the lines say vram=unavailable")
        else:
            text = ("graphics memory at the SLOW line (%s): %s" % (clk(first["t"]), _fz_vram_text(v1))) if v1 is not None else "graphics memory at the SLOW line: unavailable"
            if v2 is not None and last is not first and v2 != v1:
                text += "; at the %s line (%s): %s" % (last["event"], clk(last["t"]), _fz_vram_text(v2))
            detail.append(text)
        detail.append("lines: %s" % "; ".join("%s %s" % (l["event"], clk(l["t"])) for l in lines[:6]) + ("; ..." if len(lines) > 6 else ""))
        out.append({"src": "native", "n": n, "start": start, "end": stop, "duration": stop - start, "open": end is None,
                    "reason": end["f"].get("reason") if end else None, "fps": last["fps"], "hz": last["hz"], "fraction": last["fraction"],
                    "frame_ms": last["frame_ms"], "owner": last["owner"], "share": last["share"], "held": last["summary"], "lines": lines, "detail": detail,
                    "context": last["context"], "deliberate": False})
    return out


def _fz_slow_regimes_legacy(fz, rt_read, clk):
    """Regimes reconstructed from a build's older lines (see the block comment above the constants). Marks are the LONG FRAME and native_long_cycle lines of 3
    periods or more and every FREEZE line; slow windows are the vScreen totals under 40% of the display's rate. Returns the regimes, oldest first."""
    periods = [lf["period"] for lf in fz["lf_marks"] if lf["period"] > 0]
    hz = fz["display_hz"] or (1000.0 / _fz_median(periods) if periods else None)
    if not hz:
        return []
    period_ms = 1000.0 / hz
    marks = []
    for lf in fz["lf_marks"]:
        if lf["period"] > 0 and lf["ms"] >= FZ_SLOW_PERIODS * lf["period"]:
            marks.append({"t": lf["t"], "kind": "LONG FRAME", "seq": lf["seq"], "frame": lf["frame"]})
    for c in fz["cycles"]:
        p = _fz_num(c["f"], "period_ms")
        if c["t"] is not None and p and c["ms"] >= FZ_SLOW_PERIODS * p:
            marks.append({"t": c["t"], "kind": "native_long_cycle", "seq": c["seq"], "frame": 0})
    for fr in fz["freezes"]:
        marks.append({"t": fr["t"], "kind": "FREEZE", "seq": fr["seq"], "frame": fr["frame"]})
    windows = [v for v in fz["vscreen"] if v["fps"] < FZ_SLOW_FRACTION * hz]
    items = [(m["t"], m["t"], "mark", m) for m in marks] + [(w["t"] - w["ms"] / 1000.0, w["t"], "window", w) for w in windows]
    items.sort(key=lambda i: (i[0], i[1]))
    runs, cur, hi = [], [], None
    for it in items:
        if cur and it[0] - hi > FZ_SLOW_LINK_S:
            runs.append(cur)
            cur, hi = [], None
        cur.append(it)
        hi = it[1] if hi is None else max(hi, it[1])
    if cur:
        runs.append(cur)
    out = []
    for run in runs:
        ms_ = [i[3] for i in run if i[2] == "mark"]
        ws_ = [i[3] for i in run if i[2] == "window"]
        if ms_:
            begin = ms_[0]["t"]
            stop = max([m["t"] for m in ms_] + [w["t"] for w in ws_ if w["t"] > ms_[-1]["t"]])
        else:
            begin, stop = min(w["t"] - w["ms"] / 1000.0 for w in ws_), max(w["t"] for w in ws_)
        if stop - begin < FZ_SLOW_MIN_S or (len(ms_) < 3 and not ws_):
            continue
        rate_seq = _fz_rate([(m["t"], m["seq"]) for m in ms_])
        rate_frame = _fz_rate([(m["t"], m["frame"]) for m in ms_])
        # The runtime's sequence is what the detector counts (a frame that reached xrEndFrame). The graphics frame counter counts the game's Presents, which
        # run at hundreds a second in a menu while the runtime's frames go at the display's rate, so it speaks only when no sequence does.
        counted = [rate_seq[0]] if rate_seq is not None else [rate_frame[0]] if rate_frame is not None else []
        if counted:
            fps = counted[0]
            if fps >= FZ_SLOW_FRACTION * hz:
                continue                                    # long frames now and then, the rate between them fine: a hitchy run, not a regime
        elif ws_:
            fps = _fz_median([w["fps"] for w in ws_])
        else:
            continue                                        # marks and no counter and no window: the rate cannot be judged
        frame_ms = 1000.0 / fps
        seqs = [m["seq"] for m in ms_ if m["seq"] > 0]
        own = _fz_legacy_owner(fz, begin, stop, (min(seqs), max(seqs)) if seqs else None, frame_ms, period_ms, rt_read, clk)
        kinds = {}
        for m in ms_:
            kinds[m["kind"]] = kinds.get(m["kind"], 0) + 1
        ev = ["%d %s line(s)" % (kinds[k], k) for k in ("LONG FRAME", "native_long_cycle", "FREEZE") if k in kinds]
        if ws_:
            ev.append("%d vScreen totals window(s) under %.0f%% of the display's rate (%s fps)"
                      % (len(ws_), 100.0 * FZ_SLOW_FRACTION, ", ".join("%.0f" % w["fps"] for w in ws_[:6]) + (", ..." if len(ws_) > 6 else "")))
        how = []
        if rate_seq is not None:
            how.append("runtime sequence %d to %d over %.1f s = %.2f fps" % (rate_seq[1][1], rate_seq[1][3], rate_seq[1][2] - rate_seq[1][0], rate_seq[0]))
        if rate_frame is not None:
            how.append("graphics frames %d to %d over %.1f s = %.2f fps" % (rate_frame[1][1], rate_frame[1][3], rate_frame[1][2] - rate_frame[1][0], rate_frame[0]))
        if not how:
            how.append("the median of the vScreen windows' own rates")
        detail = ["rate: %.2f fps against the display's %g Hz (%.1f%% of it; a regime is under %.0f%%), frames of %.1f ms"
                  % (fps, round(hz, 2), 100.0 * fps / hz, 100.0 * FZ_SLOW_FRACTION, frame_ms),
                  "owner: %s" % own["held"]] + own["detail"]
        gpu = _fz_legacy_gpu(fz, begin, stop, clk) if rt_read else None
        if gpu:
            detail.append(gpu)
        detail += ["evidence: %s" % "; ".join(ev), "frame rate from: %s" % "; ".join(how)]
        if not ms_:
            detail.append("bounds: no LONG FRAME, native_long_cycle or FREEZE line is in the run (their caps ran out, or the frames stayed under 3 periods), so its start and end are "
                          "those of the vScreen totals windows, good to about 20 s")
        out.append({"src": "reconstructed", "n": len(out) + 1, "start": begin, "end": stop, "duration": stop - begin, "open": False, "reason": None,
                    "fps": fps, "hz": hz, "fraction": fps / hz, "frame_ms": frame_ms, "owner": own["key"], "share": own["share"], "held": own["held"],
                    "lines": [], "detail": detail, "context": None, "deliberate": False})
    return out


def _fz_slow_regimes(fz, rt_read):
    """The slow regimes of a flight: the build's own native_slow_regime lines when its detector was armed (or wrote any), else the reconstruction from the
    older lines. Each is marked `deliberate` when it begins within FZ_SLOW_TEST_PAD_S of the slow test's hold. Cached on fz."""
    if "regimes" not in fz:
        clk = _fz_clock(fz)
        native = fz["slow_armed"] is not None or bool(fz["slow_lines"])
        regimes = _fz_slow_regimes_native(fz, clk) if native else _fz_slow_regimes_legacy(fz, rt_read, clk)
        began = next((x for x in fz["slowtests"] if x["kind"] == "began"), None)
        for g in regimes:
            g["deliberate"] = began is not None and began["t"] - 2.0 <= g["start"] <= began["t"] + FZ_SLOW_TEST_PAD_S
            g["class"] = _fz_regime_class(g)
        fz["regimes"] = regimes
    return fz["regimes"]


def _fz_regime_sentence(g, clk, verdict=False):
    """One regime as a sentence: when, how slow, who held it. verdict=True is the shorter form the verdict line carries."""
    tags = (" (reconstructed from the older lines)" if g["src"] == "reconstructed" else "") + (" (deliberate: advanced.slow_test_ms)" if g["deliberate"] else "") \
        + (" (still going when the log stopped)" if g["open"] else "")
    if verdict:
        return "%s to %s local (%.0f s) at %.1f fps of the display's %g Hz, %s%s" % (clk(g["start"]), clk(g["end"]), g["duration"], g["fps"], round(g["hz"], 2),
                                                                                       g["held"], tags)
    status, words = g["class"]
    return "%s to %s local (%.1f s), %.2f fps against the display's %g Hz (%.0f%%), %s%s%s" % (
        clk(g["start"]), clk(g["end"]), g["duration"], g["fps"], round(g["hz"], 2), 100.0 * g["fraction"], g["held"], tags,
        "" if status == "SLOW" else "; not a SLOW: %s" % words)


def _fz_event_text(e):
    """One native_xr_event line in words."""
    f = e["f"]
    if e["suppressed"]:
        return "the cap of %s decoded event lines was reached; the rest are counted in native_xr_events_summary" % f.get("limit", "?")
    if e["type"] == "session_state_changed":
        return "session_state_changed %s -> %s (read %s ms after the runtime stamped it)" % (f.get("from", "?"), f.get("to", "?"), f.get("lag_ms", "?"))
    if e["type"] == "unknown":
        return "an event of type number %s that the runtime does not decode (the first of its type)" % f.get("number", "?")
    extra = ", ".join("%s=%s" % (k, v) for k, v in f.items() if k not in ("_name", "n", "type"))
    return "%s%s" % (e["type"], " (%s)" % extra if extra else "")


def _fz_print_slow(fz, regimes, clk):
    if not regimes:
        return
    print("SLOW REGIME%s (%d found: the frame rate under %.0f%% of the display's for %.0f s or more; a state the session was in, not one frame; SLOW is set only by a "
          "regime the vendor runtime or EDVR holds, a load the game's frame holds is INFO, a slow stretch of its scenes or no named owner a WARN):"
          % ("S" if len(regimes) > 1 else "", len(regimes), 100.0 * FZ_SLOW_FRACTION, FZ_SLOW_MIN_S))
    for g in regimes:
        status, words = g["class"]
        print("  #%d  %s to %s local  (%.1f s%s)  [%s] %s%s" % (
            g["n"], clk(g["start"]), clk(g["end"]), g["duration"],
            ", ended: %s" % g["reason"] if g["reason"] else ", still going when the log stopped" if g["open"] else "",
            {"SLOW": "SLOW", "WARN": "WARN", "note": "INFO"}[status],
            "the build's own detector" if g["src"] == "native" else "RECONSTRUCTED from the older lines: this build has no slow-regime detector",
            "; DELIBERATE: the slow test's hold (advanced.slow_test_ms)" if g["deliberate"] else ""))
        for line in g["detail"]:
            print("      %s" % line)
        if status != "SLOW":
            print("      not a SLOW: %s" % words)
        near = [e for e in fz["xr_events"] if not e["suppressed"] and g["start"] - FZ_SLOW_EVENT_PAD_S <= e["t"] <= g["end"] + FZ_SLOW_EVENT_PAD_S]
        if near:
            print("      vendor events within %.0f s of it (%d): %s%s" % (FZ_SLOW_EVENT_PAD_S, len(near), "; ".join(
                "%s %s" % (clk(e["t"]), _fz_event_text(e)) for e in near[:6]), "; ..." if len(near) > 6 else ""))
        elif fz["xr_armed"] is not None:
            print("      vendor events within %.0f s of it: none (the vendor runtime sent no event around it)" % FZ_SLOW_EVENT_PAD_S)


def _fz_print_episodes(fz, clk):
    eps, ends, arm, summ = fz["episodes"], fz["episode_ends"], fz["ep_armed"], fz["ep_summaries"]
    if arm is None and not eps and not ends and not summ:
        return
    print("END-FRAME EPISODES (a call into the vendor's xrEndFrame of %.0f display periods or more: a start line when one begins, an end line when it is over):"
          % FZ_SLOW_PERIODS)
    if eps:
        print("  %-3s %-14s %9s %10s %8s  %-11s %-8s %-10s %s" % ("#", "ended (local)", "sequence", "ms", "periods", "path", "pacing", "state", "xrAcquire/Wait/draw/Release, copy ms"))
        for e in eps[:FZ_ARC_ROWS]:
            f = e["f"]
            print("  %-3d %-14s %9d %10.1f %8.2f  %-11s %-8s %-10s %s/%s/%s/%s, %s" % (
                e["n"], clk(e["t"]), e["seq"], e["ms"], e["periods"], f.get("path", "-"), f.get("pacing", "-"), f.get("session_state", "-"),
                f.get("xr_acquire_ms", "-"), f.get("xr_wait_ms", "-"), f.get("xr_draw_ms", "-"), f.get("xr_release_ms", "-"), f.get("copy_ms", "-")))
        if len(eps) > FZ_ARC_ROWS:
            print("  (and %d more start line(s))" % (len(eps) - FZ_ARC_ROWS))
    for e in ends[:FZ_ARC_ROWS]:
        f = e["f"]
        print("  episode %d over at %s (%s): %d call(s), %d of them slow, %.1f s, p50 %.1f ms, max %.1f ms (%s periods), sequences %s-%s%s"
              % (e["n"], clk(e["t"]), f.get("reason", "?"), e["calls"], e["slow"], e["duration_ms"] / 1000.0, e["p50_ms"], e["max_ms"], f.get("max_periods", "?"),
                 f.get("first_sequence", "?"), f.get("last_sequence", "?"), "" if f.get("start_line") == "1" else "; no start line (a rate-limited start)"))
    if len(ends) > FZ_ARC_ROWS:
        print("  (and %d more end line(s))" % (len(ends) - FZ_ARC_ROWS))
    if summ:
        f = summ[-1]["f"]
        print("  latest summary (%s%s): episodes=%s start_lines=%s end_lines=%s calls=%s slow_calls=%s longest_ms=%s open=%s"
              % (clk(summ[-1]["t"]), ", reason=%s" % f["reason"] if "reason" in f else "", f.get("episodes", "-"), f.get("start_lines", "-"), f.get("end_lines", "-"),
                 f.get("calls", "-"), f.get("slow_calls", "-"), f.get("longest_ms", "-"), f.get("open", "-")))


def _fz_print_events(fz, clk):
    evs, arm, summ = [e for e in fz["xr_events"]], fz["xr_armed"], fz["xr_summaries"]
    if arm is None and not evs and not summ:
        return
    print("VENDOR EVENTS (xrPollEvent: what the vendor's runtime told the session):")
    if summ:
        f = summ[-1]["f"]
        names = ("session_state_changed", "events_lost", "instance_loss_pending", "reference_space_change_pending", "interaction_profile_changed",
                 "visibility_mask_changed", "display_refresh_rate_changed", "perf_settings", "unknown_events")
        print("  latest summary (%s%s): received %s, logged %s, suppressed %s; %s; last session state %s" % (
            clk(summ[-1]["t"]), ", reason=%s" % f["reason"] if "reason" in f else "", f.get("received", "-"), f.get("logged", "-"), f.get("suppressed", "-"),
            ", ".join("%s %s" % (k, f[k]) for k in names if f.get(k, "0") != "0") or "no event of any decoded kind", f.get("last_state", "-")))
    for e in evs[:FZ_ARC_ROWS]:
        print("  %s  #%d  %s" % (clk(e["t"]), e["n"], _fz_event_text(e)))
    if len(evs) > FZ_ARC_ROWS:
        print("  (and %d more event line(s))" % (len(evs) - FZ_ARC_ROWS))


def _fz_print_vram(fz, clk):
    st, vr = fz["vram_state"], fz["vram"]
    if st is None and not vr and fz["vram_fail"] is None:
        return
    print("VRAM (this process's graphics memory against the budget the OS gives it, DXGI QueryVideoMemoryInfo; the SLOW lines carry the same figures):")
    if st is None:
        print("  no armed or unavailable line (the first reading's line is missing)")
    elif st["state"] == "unavailable":
        print("  UNAVAILABLE: %s (no vram: line is written for the session)" % st["text"])
    else:
        print("  armed at %s on adapter %s" % (clk(st["t"]), st["adapter"] or "(unnamed)"))
    if vr:
        reasons = {}
        for e in vr:
            reasons[e["reason"]] = reasons.get(e["reason"], 0) + 1
        peak = max(vr, key=lambda e: e["kv"].get("local_pct") if e["kv"].get("local_pct") is not None else -1.0)
        print("  %d line(s) (%s); the highest local use %s at %s: %s" % (
            len(vr), ", ".join("%s %d" % kv for kv in sorted(reasons.items())),
            "%.1f%%" % peak["kv"]["local_pct"] if peak["kv"].get("local_pct") is not None else "-", clk(peak["t"]), _fz_vram_text(peak["kv"])))
        marks = [e for e in vr if e["reason"] not in ("armed", "periodic")]
        for e in marks[:FZ_ARC_ROWS]:
            print("  %s  %-16s %s" % (clk(e["t"]), e["reason"], _fz_vram_text(e["kv"])))
        if len(marks) > FZ_ARC_ROWS:
            print("  (and %d more crossing or pressure line(s))" % (len(marks) - FZ_ARC_ROWS))
    if fz["vram_fail"] is not None:
        print("  a read failed at %s: %s" % (clk(fz["vram_fail"]["t"]), fz["vram_fail"]["text"]))


def _fz_print_slowtest(fz, clk):
    tests = fz["slowtests"]
    if not tests and not fz["holds"]:
        return
    print("SLOW TEST (advanced.slow_test_ms: the runtime holds every xrEndFrame longer, for a while, so one flight shows the whole chain):")
    for x in tests:
        print("  %s  graphics log: %s" % (clk(x["t"]), {"armed": "the key was read (%d ms, the hold begins %d s in)" % (x["ms"], x["start"]),
                                                      "began": "the hold begins now (%d ms for %d s)" % (x["ms"], x["span"]), "ended": "the hold ended"}[x["kind"]]))
    for h in fz["holds"]:
        print("  %s  runtime log: native_end_frame_hold state=%s%s" % (clk(h["t"]), h["state"], " ms=%s" % h["f"]["ms"] if "ms" in h["f"] else
                                                                      " held_calls=%s" % h["f"]["held_calls"] if "held_calls" in h["f"] else ""))


def _fz_arc_findings(fz, rt_read, clk):
    """[(key, status, text)] for the headset-lock arc: SLOW REGIME (a row per regime, its status by who holds it: SLOW, never PASS, for the vendor runtime, EDVR's
    copy or EDVR's own work; INFO (note) for the game's own frame in a load; WARN for the game's scenes or no named owner), INSTRUMENT notes, END-FRAME
    EPISODES, VENDOR EVENTS, VRAM and SLOW TEST. A log with none of the arc's lines gets notes only, so an older flight's verdict is what it was."""
    out = []

    def add(key, status, text):
        out.append((key, status, text))

    regimes = _fz_slow_regimes(fz, rt_read)
    native = fz["slow_armed"] is not None or bool(fz["slow_lines"])
    for g in regimes:
        add("SLOW REGIME", g["class"][0], _fz_regime_sentence(g, clk))
    summ = fz["slow_summaries"][-1] if fz["slow_summaries"] else None
    if native and summ is not None and summ["f"].get("reason") == "session_close":     # a periodic summary is older than the lines after it
        numbers = {l["regime"] for l in fz["slow_lines"]}
        if summ["regimes"] != len(numbers):
            add("SLOW REGIME", "WARN", "the detector's summary at %s counts %d regime(s) but the log has native_slow_regime lines for %d: a line was lost, or the log was cut"
                % (clk(summ["t"]), summ["regimes"], len(numbers)))
    if not regimes:
        if native:
            seen = ("; its last summary (%s) saw %s frame(s) in %s s, %s s of them slow" % (clk(summ["t"]), summ["f"].get("frames", "?"), summ["f"].get("seconds", "?"),
                                                                                           summ["f"].get("slow_seconds", "?"))
                    if summ is not None else "; it wrote no summary, so the session did not close cleanly or ran under five minutes")
            add("SLOW REGIME", "PASS", "the slow-regime detector was armed%s and wrote no SLOW line%s" % (
                " at %s" % clk(fz["slow_armed"]["t"]) if fz["slow_armed"] else "", seen))
        elif not rt_read and (fz["vram_state"] is not None or fz["vram"] or fz["slowtests"]):
            add("SLOW REGIME", "note", "the slow-regime detector writes to the runtime log, which was not read, so its SLOW lines were not looked for; the graphics log's older lines "
                "(LONG FRAME, FREEZE, vScreen totals) hold no run of %.0f s or more with the frame rate under %.0f%% of the display's, which cannot prove there was none"
                % (FZ_SLOW_MIN_S, 100.0 * FZ_SLOW_FRACTION))
        else:
            add("SLOW REGIME", "note", "this build has no slow-regime detector, and its older lines (LONG FRAME, native_long_cycle, FREEZE, vScreen totals) hold no run of %.0f s or "
                "more with the frame rate under %.0f%% of the display's; that cannot prove there was none" % (FZ_SLOW_MIN_S, 100.0 * FZ_SLOW_FRACTION))

    arms = (("vendor-event log (native_xr_events)", fz["xr_armed"]), ("end-frame episodes (native_end_frame_episodes)", fz["ep_armed"]),
            ("slow-regime detector (native_slow_regime)", fz["slow_armed"]))
    st = fz["vram_state"]
    if any(a is not None for _, a in arms) or st is not None:
        for label, a in arms:
            if a is not None:
                add("INSTRUMENT", "note", "the %s armed at %s" % (label, clk(a["t"])))
            elif rt_read:
                add("INSTRUMENT", "WARN", "the runtime log has no armed line for the %s although it has others of this arc: it did not arm, or its line was lost" % label)
        if st is None:
            if fz["vram"] or any(a is not None for _, a in arms):
                add("INSTRUMENT", "WARN", "the graphics log has no `vram:` armed or unavailable line although this build writes one at its first Present: the watch did not run, or its "
                    "line was lost")
        elif st["state"] == "armed":
            add("INSTRUMENT", "note", "the graphics-memory watch armed at %s on adapter %s" % (clk(st["t"]), st["adapter"] or "(unnamed)"))
        else:
            add("INSTRUMENT", "note", "the graphics-memory watch is UNAVAILABLE on this machine (%s): no vram: line is written, and the SLOW lines carry vram=unavailable" % st["text"])
    else:
        add("INSTRUMENT", "note", "none of the headset-lock arc's lines (vendor events, end-frame episodes, slow-regime detector, graphics memory) is in this log: a build before them")

    eps, ends = fz["episodes"], fz["episode_ends"]
    if fz["ep_armed"] is not None or eps or ends:
        if eps:
            top = max(eps, key=lambda e: e["ms"])
            add("END-FRAME EPISODES", "note", "%d start line(s) and %d end line(s); the longest xrEndFrame call %.1f ms (%.1f display periods, sequence %d, at %s)"
                % (len(eps), len(ends), top["ms"], top["periods"], top["seq"], clk(top["t"])))
        else:
            add("END-FRAME EPISODES", "note", "no xrEndFrame call of %.0f display periods or more (no start line, %d end line(s))" % (FZ_SLOW_PERIODS, len(ends)))
        open_eps = sorted({e["n"] for e in eps} - {e["n"] for e in ends})
        if open_eps and not (fz["ep_summaries"] and fz["ep_summaries"][-1]["f"].get("reason") == "session_close"):
            add("END-FRAME EPISODES", "note", "episode%s %s started and ha%s no end line: the session ended inside %s (a crash or a kill), or the log was cut"
                % ("s" if len(open_eps) > 1 else "", ", ".join(str(n) for n in open_eps), "ve" if len(open_eps) > 1 else "s", "them" if len(open_eps) > 1 else "it"))
        for g in regimes:
            if g["src"] != "native" or g["owner"] != "vendor_end_frame":
                continue
            mean_end = _fz_num(g["lines"][-1]["f"], "vendor_end_frame_ms")
            if mean_end is None or not g["hz"] or mean_end < FZ_SLOW_PERIODS * 1000.0 / g["hz"]:
                continue        # the mean call is under 3 periods, so no call need have been: the episode instrument owes no line
            seen = [e for e in eps if g["start"] - 3.0 <= e["t"] <= g["end"] + 3.0] + [e for e in ends if g["start"] <= e["t"] <= g["end"] + 10.0]
            if not seen:
                add("END-FRAME EPISODES", "WARN", "regime #%d is held by the vendor's xrEndFrame but the episode instrument wrote no start or end line in it (a call of %.0f display "
                    "periods or more begins an episode)" % (g["n"], FZ_SLOW_PERIODS))

    evs = [e for e in fz["xr_events"] if not e["suppressed"]]
    if fz["xr_armed"] is not None or evs or fz["xr_summaries"]:
        s = fz["xr_summaries"][-1] if fz["xr_summaries"] else None
        changes = [e for e in evs if e["type"] == "session_state_changed"]
        last = "last session state %s" % (s["f"].get("last_state", "?") if s else (changes[-1]["f"].get("to", "?") if changes else "not reported"))
        add("VENDOR EVENTS", "note", "%d event line(s) in the log%s; %s" % (
            len(evs), ", %s event(s) received by the summary at %s" % (s["f"].get("received", "?"), clk(s["t"])) if s else "", last))
        lost = [e for e in evs if e["type"] == "events_lost"]
        if lost:
            add("VENDOR EVENTS", "WARN", "the vendor runtime reported events_lost %d time(s) (first at %s, lost_count=%s): events were dropped before the runtime read them"
                % (len(lost), clk(lost[0]["t"]), lost[0]["f"].get("lost_count", "?")))
        gone = [e for e in evs if e["type"] == "instance_loss_pending"]
        if gone:
            add("VENDOR EVENTS", "WARN", "the vendor runtime reported instance_loss_pending at %s (loss_time=%s): it told the session it is going away" % (clk(gone[0]["t"]), gone[0]["f"].get("loss_time", "?")))

    over = [e for e in fz["vram"] if e["reason"] == "over_budget"]
    pressure = [e for e in fz["vram"] if e["reason"] == "pressure"]
    if over:
        add("VRAM", "WARN", "graphics memory went over the OS's budget %d time(s), first at %s (%s): above it the OS moves this process's memory into system RAM"
            % (len(over), clk(over[0]["t"]), _fz_vram_text(over[0]["kv"])))
    if pressure:
        top = max(pressure, key=lambda e: e["kv"].get("local_pct") or 0.0)
        add("VRAM", "note", "%d pressure line(s) (local use at or above 90%% of the budget), the highest %.1f%% at %s" % (len(pressure), top["kv"].get("local_pct") or 0.0, clk(top["t"])))
    if fz["vram_fail"] is not None:
        add("VRAM", "note", "a graphics-memory read failed at %s and the watch retries every second: %s" % (clk(fz["vram_fail"]["t"]), fz["vram_fail"]["text"]))
    if fz["vram"] and not over and not pressure:
        top = max(fz["vram"], key=lambda e: e["kv"].get("local_pct") if e["kv"].get("local_pct") is not None else -1.0)
        add("VRAM", "note", "%d vram line(s), none over the budget or at 90%% of it; the highest local use %s at %s" % (
            len(fz["vram"]), "%.1f%%" % top["kv"]["local_pct"] if top["kv"].get("local_pct") is not None else "-", clk(top["t"])))

    tests = fz["slowtests"]
    if tests:
        armed = [x for x in tests if x["kind"] == "armed"]
        began = next((x for x in tests if x["kind"] == "began"), None)
        ended = next((x for x in tests if x["kind"] == "ended"), None)
        key = "advanced.slow_test_ms = %d" % (armed[-1]["ms"] if armed else began["ms"] if began else 0)
        if began is None:
            add("SLOW TEST", "WARN", "%s was read, but the log has no `the hold begins now` line: the session ended before the hold began" % key)
        else:
            lo, hi = began["t"] - 2.0, began["t"] + began["span"] + FZ_SLOW_TEST_PAD_S
            missing = []
            have = []
            if rt_read:
                if not any(h["state"] == "began" for h in fz["holds"]):
                    missing.append("the runtime's native_end_frame_hold state=began line (the hold never reached the runtime)")
                if ended is not None and not any(h["state"] == "ended" for h in fz["holds"]):
                    missing.append("the runtime's native_end_frame_hold state=ended line")
                eps_in = [e for e in eps if lo <= e["t"] <= hi]
                ends_in = [e for e in ends if lo <= e["t"] <= hi + 10.0]
                if not eps_in or max(e["ms"] for e in eps_in) < 0.9 * began["ms"]:
                    missing.append("an end-frame episode start line of at least %d ms" % int(0.9 * began["ms"]))
                else:
                    have.append("an episode start line of %.1f ms" % max(e["ms"] for e in eps_in))
                if not ends_in:
                    missing.append("an end-frame episode end line")
                else:
                    have.append("%d episode end line(s)" % len(ends_in))
                reg = next((g for g in regimes if g["deliberate"]), None)
                if reg is None:
                    missing.append("a SLOW regime that begins within %d s of the hold (a hold of %d ms makes the frames about %d ms; it should show)"
                                   % (int(FZ_SLOW_TEST_PAD_S), began["ms"], began["ms"] + 14))
                else:
                    if reg["owner"] != "vendor_end_frame":
                        missing.append("the regime held by the vendor's xrEndFrame (the line names %s)" % FZ_OWNER_TEXT.get(reg["owner"], reg["owner"]))
                    events = {l["event"] for l in reg["lines"]}
                    if reg["src"] == "native":
                        if began["span"] >= 36 and "still_slow" not in events:
                            missing.append("a still_slow line (the hold lasted %d s; one is written 30 s after the SLOW line)" % began["span"])
                        if ended is not None and "end" not in events:
                            missing.append("the regime's end line")
                        if st is not None and st["state"] == "armed" and reg["lines"][0]["vram"] is None:
                            missing.append("the graphics-memory figures on the SLOW line")
                    have.append("SLOW regime #%d held by %s%s" % (reg["n"], FZ_OWNER_TEXT.get(reg["owner"], reg["owner"]), ", with a still_slow and an end line" if {"still_slow", "end"} <= events else ""))
            if ended is None and fz["gscan"]["last"] > hi:
                missing.append("the graphics log's `the hold ended` line")
            head = "the deliberate %d ms hold at %s (%d s)" % (began["ms"], clk(began["t"]), began["span"])
            if missing:
                add("SLOW TEST", "WARN", "%s, but the chain lacks %s" % (head, "; ".join(missing)))
            elif not rt_read:
                add("SLOW TEST", "note", "%s began; no runtime log was read, so its half of the chain was not checked" % head)
            else:
                add("SLOW TEST", "PASS", "%s: %s" % (head, ", ".join(have)))
    return out


def freezes_judge(fz, rows, pairs, rt_read, clk):
    """[(key, status, text)] in the order of the rules: INSTRUMENT, UNWRITTEN FREEZES, FREEZE LINES, SAMPLER, SUSPENSION, NO FREEZE (and PARSE, a line
    this reader could not read). status is PASS, WARN, STOP or note; a note is a fact and never changes the verdict."""
    out = []

    def add(key, status, text):
        out.append((key, status, text))

    gc = fz["gfx_counts"]
    rc = fz["rt_counts"] if rt_read else []
    rc_new = [c for c in rc if "candidates" in c["kv"]]

    # INSTRUMENT: the counts lines are the proof that the freeze logging ran at all (a quiet session's zeros are written too).
    if gc:
        text = "a `monitor: long frame counts` line is in the graphics log (%d line(s), the last reason=%s at %s): the freeze logging ran" % (
            len(gc), gc[-1]["reason"], clk(gc[-1]["t"]))
        if rc_new:
            text += "; the runtime log has %d summary or counts line(s) with the counts too" % len(rc_new)
        add("INSTRUMENT", "PASS", text)
    else:
        add("INSTRUMENT", "WARN", "no `monitor: long frame counts` line in the graphics log: an older DLL, or a session that ended without a clean runtime "
            "close in under five minutes (the counts are written every five minutes and at the end), so this log cannot show that the freeze logging ran")
    if not rt_read:
        add("INSTRUMENT", "WARN", "no runtime log was read, so the runtime's native_long_cycle lines and its counts are not in this report")
    elif not rc:
        add("INSTRUMENT", "WARN", "the runtime log has no native_long_cycle_summary or native_long_cycle_counts line (its session did not close and ran under "
            "five minutes, or it is an older runtime)")
    elif not rc_new:
        add("INSTRUMENT", "WARN", "the runtime log's summary has only the three old fields (count, logged, threshold): an older runtime, so its counts by "
            "size and over_250ms_unwritten read `-`")

    # UNWRITTEN FREEZES: over_250ms_unwritten is how many frames of 250 ms or more did not get a line. It is 0 by construction; anything else is a bug.
    keyed = [("graphics", c) for c in gc if "over_250ms_unwritten" in c["kv"]] + [("runtime", c) for c in rc if "over_250ms_unwritten" in c["kv"]]
    bad = [(h, c) for h, c in keyed if c["kv"]["over_250ms_unwritten"] > 0]
    if bad:
        h, c = max(bad, key=lambda hc: hc[1]["kv"]["over_250ms_unwritten"])
        add("UNWRITTEN FREEZES", "STOP", "over_250ms_unwritten=%d in the %s counts line at %s%s: a frame of 250 ms or more got no line, which the freeze "
            "logging must never allow%s" % (c["kv"]["over_250ms_unwritten"], h, clk(c["t"]), " (reason=%s)" % c["reason"] if c.get("reason") else "",
                                            "" if len(bad) == 1 else " (%d counts lines say so)" % len(bad)))
    elif keyed:
        add("UNWRITTEN FREEZES", "PASS", "over_250ms_unwritten=0 in every counts line that carries it (%d graphics, %d runtime)"
            % (sum(1 for h, _ in keyed if h == "graphics"), sum(1 for h, _ in keyed if h == "runtime")))
    else:
        add("UNWRITTEN FREEZES", "note", "no counts line carries over_250ms_unwritten, so there is nothing to judge (see INSTRUMENT)")

    # FREEZE LINES: every freeze has its LONG FRAME line, and every runtime cycle of 250 ms or more has its FREEZE line.
    only = [c for c, row in pairs if row is None]
    no_long = [r for r in rows if r["lf"] is None]
    problems = False
    if only:
        listing = "; ".join("sequence %d, %.1f ms, ended %s" % (c["seq"], c["ms"], clk(c["t"])) for c in only[:4]) + ("; ..." if len(only) > 4 else "")
        if gc:
            problems = True
            add("FREEZE LINES", "STOP", "%d runtime cycle(s) of 250 ms or more have no FREEZE line although the graphics log has a counts line, so the "
                "graphics half did not write a freeze: %s" % (len(only), listing))
        else:
            problems = True
            add("FREEZE LINES", "note", "%d runtime cycle(s) of 250 ms or more have no FREEZE line, but the graphics log has no counts line, so it may not "
                "have the freeze logging at all: %s" % (len(only), listing))
    if no_long:
        problems = True
        add("FREEZE LINES", "WARN", "%d FREEZE line(s) have no LONG FRAME line (the FREEZE line follows its LONG FRAME line, which from 250 ms up is "
            "not under the rate limit): %s" % (len(no_long), "; ".join("frame %d at %s" % (r["f"]["frame"], clk(r["f"]["t"])) for r in no_long[:4])
                                               + ("; ..." if len(no_long) > 4 else "")))
    if not problems and (rows or pairs):
        if rt_read:
            add("FREEZE LINES", "PASS", "each of the %d FREEZE line(s) has its LONG FRAME line, and each of the %d runtime cycle(s) of 250 ms or more has "
                "its FREEZE line" % (len(rows), len(pairs)))
        else:
            add("FREEZE LINES", "PASS", "each of the %d FREEZE line(s) has its LONG FRAME line (no runtime log was read, so no runtime cycle was checked)"
                % len(rows))

    # SAMPLER: named an owner for a freeze, or armed and never did. Off (or absent) says nothing here; a watchdog that could not start is its own WARN.
    st = fz["sampler_state"]
    armed = (st is not None and st["state"] == "armed") or (st is None and bool(fz["stalls"] or fz["stall_fails"] or fz["stall_counts"]))
    if st is not None and st["state"] == "failed":
        add("SAMPLER", "WARN", "the stall sampler could not start its watchdog thread, so no freeze in this log was sampled: %s" % st["text"][:120])
    if armed and rows:
        owned = [r for r in rows if any(s["kind"] == "stall" for s in r["stalls"])]
        if owned:
            owners = {}
            for r in owned:
                for s in r["stalls"]:
                    if s["kind"] == "stall":
                        owners[s["owner"]] = owners.get(s["owner"], 0) + 1
            add("SAMPLER", "PASS", "the stall sampler named an owner for %d of %d freeze(s): %s" % (
                len(owned), len(rows), ", ".join("%s (%d sample(s))" % kv for kv in sorted(owners.items(), key=lambda kv: (-kv[1], kv[0]))[:4])))
        else:
            failed = sum(1 for r in rows for s in r["stalls"] if s["kind"] == "fail")
            clauses = sorted({r["f"]["sampler"] for r in rows if r["f"]["sampler"]})
            add("SAMPLER", "WARN", "the stall sampler is armed and %d freeze(s) were logged, but no `stall:` sample with an owner falls inside any of them%s%s"
                % (len(rows), " (%d failed sample line(s) do)" % failed if failed else "",
                   "; the FREEZE line(s) say: %s" % "; ".join(clauses) if clauses else ""))

    # SUSPENSION: how long a sample held the render thread. Each is microseconds; the counts line keeps the longest of all.
    us = [(s["us"], "sample %d of %d at %s" % (s["k"], s["of"], clk(s["t"]))) for s in fz["stalls"]]
    us += [(c["kv"]["longest_suspend_us"], "longest_suspend_us in the counts line at %s" % clk(c["t"])) for c in fz["stall_counts"]
           if "longest_suspend_us" in c["kv"]]
    if us:
        top, where = max(us, key=lambda x: x[0])
        if top > FREEZE_SUSPEND_STOP_US:
            add("SUSPENSION", "STOP", "a stall sample held the render thread stopped for %d us (%s): far longer than a sample should take (the limit for a "
                "STOP is %d us)" % (top, where, FREEZE_SUSPEND_STOP_US))
        elif top > FREEZE_SUSPEND_WARN_US:
            add("SUSPENSION", "WARN", "a stall sample held the render thread stopped for %d us (%s), over the %d us a sample should stay under"
                % (top, where, FREEZE_SUSPEND_WARN_US))
        else:
            add("SUSPENSION", "PASS", "the longest a stall sample held the render thread stopped was %d us (%s), within %d us"
                % (top, where, FREEZE_SUSPEND_WARN_US))

    # FREEZE TEST: advanced.freeze_test_ms makes the render thread sleep once, 60 s in, so one flight shows the whole chain. The sleep is a frame of
    # 250 ms or more, so it must have a FREEZE line; its stall samples must name an owner, and since the thread sleeps inside EDVR's own DLL the
    # sampler should find EDVR's code on the stack.
    tests = fz["tests"]
    if tests:
        key = "advanced.freeze_test_ms = %d" % tests[-1]["ms"]
        slept = [t for t in tests if t["kind"] == "sleep"]
        if not slept:
            add("FREEZE TEST", "WARN", "%s was read, but the log has no `the render thread sleeps` line: the session ended before the sleep, 60 s in" % key)
        for sl in slept:
            holder = next((r for r in rows if r["test"] is sl), None)
            if sl["ms"] < FREEZE_MS:
                add("FREEZE TEST", "note", "the test slept %d ms at %s: under 250 ms, so no FREEZE line is due" % (sl["ms"], clk(sl["t"])))
            elif holder is None:
                add("FREEZE TEST", "STOP", "the test slept %d ms at %s, a frame of 250 ms or more, and no FREEZE line holds it: it must always get a line"
                    % (sl["ms"], clk(sl["t"])))
            else:
                got = [s for s in holder["stalls"] if s["kind"] == "stall"]
                missing = []
                if holder["lf"] is None:
                    missing.append("its LONG FRAME line")
                if rt_read and (holder["cycle"] is None or holder["cycle"]["ms"] < 0.8 * sl["ms"]):
                    missing.append("a runtime cycle of about that length")
                if armed and not got:
                    missing.append("a stall sample with an owner")
                elif armed and not any(s["edvr"] for s in got):
                    missing.append("EDVR code on the stack in any sample (the sleep is inside EDVR's own DLL)")
                head = "the deliberate %d ms sleep at %s is FREEZE #%d (frame %d, %.1f ms between Presents)" % (
                    sl["ms"], clk(sl["t"]), holder["i"], holder["f"]["frame"], holder["f"]["ms"])
                if missing:
                    add("FREEZE TEST", "WARN", "%s, but the chain lacks %s" % (head, "; ".join(missing)))
                else:
                    have = ["its LONG FRAME line"]
                    if holder["cycle"] is not None:
                        have.append("a runtime cycle of %.1f ms" % holder["cycle"]["ms"])
                    if armed:
                        have.append("%d stall sample(s) naming %s, EDVR code on the stack in %d of them"
                                    % (len(got), ", ".join(sorted({s["owner"] for s in got})), sum(1 for s in got if s["edvr"])))
                    else:
                        have.append("no stall samples (the sampler is not armed, so none were due)")
                    add("FREEZE TEST", "PASS", "%s, with %s" % (head, ", ".join(have)))

    # NO FREEZE: the plain answer for a flight without one.
    if not rows and not pairs:
        add("NO FREEZE", "PASS", "no frame of 250 ms or more (no FREEZE line%s)"
            % (", no runtime cycle of 250 ms or more" if rt_read else "; no runtime log was read to look for a cycle in"))

    if fz["unparsed"]["count"]:
        add("PARSE", "WARN", "%d line(s) open like a freeze-diagnostics line but match none of the formats this reads (the C++ wording changed?), and "
            "what is above leaves them out. First: %s" % (fz["unparsed"]["count"], " | ".join(fz["unparsed"]["samples"])))
    # The headset-lock arc's rows come first: a slow regime is the finding, and the freeze rows are the detail beside it.
    return _fz_arc_findings(fz, rt_read, clk) + out


def print_freezes(gfx_path, gfx_text, gfx_ver, want, args, native_dirs):
    """--freezes: one flight's freeze diagnostics in one report. Returns the exit code: 0 (PASS or WARN), 1 (STOP), 2 (the runtime log is of another
    build than --expect-build named; main() has checked the graphics log's), 3 (the log holds none of the freeze lines and no slow regime), 4 (SLOW: the
    log holds a slow regime, the frame rate under 40% of the display's for 5 s or more, that the vendor runtime, EDVR's copy or EDVR's own work holds, found
    by the build's own detector or put back together from the older lines; no STOP). A regime the game's own frame holds (INFO for a load, WARN for its
    scenes) or that has no named owner (WARN) does not set it."""
    rt_path, rt_text, rc = open_runtime_log(gfx_path, gfx_ver, want, args.runtime_file, native_dirs,
                                            lacking="native_long_cycle lines and the runtime's counts are unavailable")
    if rc is not None:
        return rc
    fz = parse_freezes(gfx_path, gfx_text, rt_path, rt_text)
    gscan, rscan, clock = fz["gscan"], fz["rscan"], fz["clock"]
    if gscan["stamped"] == 0:
        print("[edvr] freezes: the graphics log has no [HH:MM:SS.mmm] lines; --freezes reads edvr_gfx_*.log (a runtime log goes to --runtime-file).")
        return 3
    if rscan is not None and rscan["stamped"] == 0:
        print("[edvr] WARNING: the runtime log has no 'YYYY-MM-DD HH:MM:SS.mmm UTC pid= tid=' lines; it is not used.")
        rscan = None
    # A log from before the freeze logging can still hold a slow regime in its older lines: that is read, and is a finding, not an old build.
    if not _fz_has_lines(fz) and not _fz_slow_regimes(fz, rscan is not None):
        print("[edvr] freezes: none of the freeze-diagnostics lines is in this log (no FREEZE, long frame counts, worst long frame, stall sampler, "
              "native_long_cycle_worst or native_long_cycle_counts line), so it is from a build before the freeze logging.")
        return 3
    rt_read = rscan is not None
    rows, loose = freeze_rows(fz)
    pairs = _fz_cycle_pairs(rows, fz["cycles"] if rt_read else [])
    day0 = int(gscan["first"] // DAY)

    def clk(t):
        return "-" if t is None else fmt_clock(t, day0)

    print("[edvr] freezes: %s, runtime log %s; build %s; %d FREEZE line(s), %d runtime cycle(s) of 250 ms or more"
          % (os.path.basename(gfx_path), os.path.basename(rt_path) if rt_read else "none", gfx_ver or "(no version line)", len(rows), len(pairs)))
    if rt_read:
        print("[edvr] clocks: every time below is LOCAL; the runtime log's UTC prefix is converted at %s (%s)"
              % (_fmt_zone(clock["to_local"]), clock["how"]))
        if rscan["last"] < gscan["first"] or rscan["first"] > gscan["last"]:
            print("[edvr] WARNING: the two logs' time spans do not overlap: the clock conversion is wrong, or these are not one flight.")
        shared, median = clock_check(gscan["frames"] + rscan["frames"])
        if median is not None and abs(median) > 300000.0:
            print("[edvr] WARNING: %d LONG FRAME line(s) share a runtime sequence with a native_long_cycle line, %.0f s apart at the median: the UTC offset "
                  "is wrong, and a join across the two logs by time cannot be trusted" % (shared, median / 1000.0))
        elif median is not None:
            print("[edvr] clock check: %d LONG FRAME line(s) share a runtime sequence with a native_long_cycle line, %+.0f ms apart at the median"
                  % (shared, median))
    else:
        print("[edvr] clocks: every time below is LOCAL, the graphics log's own [HH:MM:SS.mmm] prefix")
    regimes = _fz_slow_regimes(fz, rt_read)
    _fz_print_slow(fz, regimes, clk)
    if rows:
        _fz_print_freezes(rows, rt_read, clk)
    if pairs:
        _fz_print_cycles(pairs, clk)
    _fz_print_counts(fz, rt_read, clk)
    _fz_print_worst(fz, rt_read, clk)
    _fz_print_sampler(fz, rows, loose, clk)
    _fz_print_gpu(fz, rows, clk)
    _fz_print_episodes(fz, clk)
    _fz_print_events(fz, clk)
    _fz_print_vram(fz, clk)
    _fz_print_slowtest(fz, clk)
    findings = freezes_judge(fz, rows, pairs, rt_read, clk)
    for key, status, text in findings:
        print("  %-4s  %s: %s" % (status, key, text))
    stops = sum(1 for f in findings if f[1] == "STOP")
    warns = sum(1 for f in findings if f[1] == "WARN")
    slows = sum(1 for f in findings if f[1] == "SLOW")
    # A log that holds a regime the vendor runtime, EDVR's copy or EDVR's own work held never gets PASS or WARN, and its verdict says when, how slow and who held
    # it. With a STOP the word stays STOP (the instrument failed), the regime still named. A regime the game's own frame holds (a load, or its scenes at a
    # low rate) or that has no named owner is a row of its own (INFO or WARN) and does not set SLOW or exit 4; the verdict still says how many there were.
    word = "STOP" if stops else "SLOW" if slows else "WARN" if warns else "PASS"
    slow_regs = [g for g in regimes if g["class"][0] == "SLOW"]
    other_regs = [g for g in regimes if g["class"][0] != "SLOW"]
    held = (": %s" % "; ".join(_fz_regime_sentence(g, clk, True) for g in slow_regs[:3]) + ("; and %d more" % (len(slow_regs) - 3) if len(slow_regs) > 3 else "")) if slows else ""
    if other_regs:
        held += " [+%d slow stretch(es) the game's own frame or no named owner held, listed above: not a SLOW]" % len(other_regs)
    print("freezes verdict: %s (%d STOP, %d WARN%s)%s. STOP: a frame of 250 ms or more that got no line, a runtime cycle of 250 ms or more the graphics half "
          "did not write, or a sample that held the render thread over %d us. WARN: the counts missing, a FREEZE line without its LONG FRAME line, an armed "
          "sampler that named no owner, a sample over %d us, a slow stretch the game's scenes or no named owner held, or a line this reader could not read.%s"
          % (word, stops, warns, ", %d SLOW" % slows if slows else "", held, FREEZE_SUSPEND_STOP_US, FREEZE_SUSPEND_WARN_US,
             " SLOW: the frame rate stayed under %.0f%% of the display's for %.0f s or more, and the vendor runtime, EDVR's copy or EDVR's own work held most of each frame."
             % (100.0 * FZ_SLOW_FRACTION, FZ_SLOW_MIN_S) if slows else ""))
    return 1 if stops else 4 if slows else 0


def self_test_freezes():
    """--freezes on logs built from the exact lines the C++ writes (perf_monitor.cpp freezeLine and writeFreezeSummary, freeze_book.h, long_cycle_line.h,
    gpu_frame_gap.h, and the stall sampler's own): a healthy flight with a 1858 ms freeze, then each way a log can differ from it, every case asserting the
    printed section and verdict keys and the exit code through main(). The pins at the top tie the reader's phrases to the sources that write them, and
    the run asserts that --freezes opens nothing for writing. Returns ok."""
    import builtins
    import contextlib
    import io
    import shutil
    ok = True

    def fail(msg):
        nonlocal ok
        print("freezes: %s" % msg)
        ok = False

    # The pins: each phrase the reader keys on must still be in the source that writes it, on one line there. A reworded line fails here with the file
    # named, instead of making a flight's report quietly wrong.
    root = repo_root()
    for rel, needles in (
            (("src", "d3d11", "perf_monitor.cpp"), (
                "monitor: FREEZE -- %.1f ms between Presents, ended now: frame %llu, runtime sequence %llu;",
                "ms from the pose wait's return to this Present, the previous cycle %.1f ms)",
                "freeze %llu of this session; stall sampler %s.",
                "monitor: long frame counts reason=%s %s;",
                "monitor: worst long frame %u of %u: %.1f ms between Presents, ended %s (frame %llu, runtime",
                "runtime cycle %s)%s%s",
                "freeze test: advanced.freeze_test_ms = %d.",
                "freeze test: the render thread sleeps %d ms now.")),
            (("src", "common", "stall_sampler.h"), (
                '"stall: the render thread stalled %u ms in "', '"; owner "', '"; stack "',
                '"; sample %u of %u, last Present returned in frame %llu, thread %lu, suspended %u us"',
                '" (the stack pointer was outside the thread\'s stack: one frame only)"',
                '"; EDVR code on the stack: yes (%u frame%s, innermost "', '"; EDVR code on the stack: no."',
                '"stall: sample %u of %u at %u ms failed: %s; last Present returned in frame %llu, thread %lu."')),
            (("src", "d3d11", "stall_watch.cpp"), (
                "stall sampler: off (advanced.freeze_location = off).", "stall sampler: armed. The render thread (thread %lu)",
                "stall sampler: could not start its watchdog thread", "stall sampler counts reason=%s episodes=%u samples=%u skipped_rate_limit=%u failures=%u")),
            (("src", "openxr", "long_cycle_line.h"), ('"native_long_cycle_worst,rank=%u,of=%u,utc=%s,%s"',
                                                      '"%s,reason=%s,count=%llu,logged=%llu,threshold=2x_period,%s"')),
            (("src", "common", "freeze_book.h"), ('add("over_250ms_unwritten"', '"long_%s"', '"blip_%s"', '"unwritten_%s"', 'return "lt50";', 'return "50_100";',
                                                  'return "100_250";', 'return "250_1000";', 'return "ge1000";')),
            (("src", "d3d11", "gpu_frame_gap.h"), ("EDVR GPU census, frame gap:", "pair%s over 1 s KEPT as stalls (not in the percentiles or the max; longest %.1f ms; each is the gap",
                                                    '"%s sequence %llu %.1f ms"'))):
        try:
            body = read_text(os.path.join(root, *rel))
        except OSError:
            fail("%s is not where the self-test looks for it" % "\\".join(rel))
            continue
        for needle in needles:
            if needle not in body:
                fail("%s no longer has %r, which --freezes reads; change this reader and its pin together" % ("\\".join(rel), needle))

    VERSION = "0.17.0-5-gabcdef1"
    ZONE_H = 8                           # local is UTC+8: the runtime log's file name (local) against its first line (UTC) says so
    T0 = 23 * 3600 + 20 * 60             # both logs open at 23:20:00 local
    END = T0 + 4 * 60 + 19.790           # the freeze under test ends at 23:24:19.790 local, 15:24:19.790 UTC
    CLOSE = T0 + 39 * 60                 # the session closes at 23:59:00
    GFX_NAME = "edvr_gfx_20261001_232000.log"
    RT_NAME = "edvr_openxr_20261001_232000_100_4242.log"
    BUCKETS = ("lt50", "50_100", "100_250", "250_1000", "ge1000")

    def hms(t):
        ms = int(round(t * 1000.0))
        h, rest = divmod(ms, 3600000)
        m, rest = divmod(rest, 60000)
        s, milli = divmod(rest, 1000)
        return "%02d:%02d:%02d.%03d" % (h, m, s, milli)

    def g(t, msg):
        """A graphics log line at local time t (seconds of the day)."""
        return "[%s] %s" % (hms(t), msg)

    def r(t, msg):
        """A runtime log line at local time t: its prefix is UTC."""
        return "2026-10-01 %s UTC pid=4242 tid=9 %s" % (hms(t - ZONE_H * 3600), msg)

    def utc(t):
        return "2026-10-01T%sZ" % hms(t - ZONE_H * 3600)

    # ---- the graphics log's lines, as the C++ formats them ----
    def long_frame(ms, seq, frame=47211):
        return ("monitor: LONG FRAME -- %.1f ms between Presents (runtime predicted period 11.1 ms), no WaitGetPoses, CPU busy, compositor, reprojection, "
                "or door samples; game creations: 0 textures, 0 buffers, 0 shaders (0.0 MB); EDVR events: none. This is frame %d; the flip timeline is not "
                "armed, so there are no table changes to order against it. %sgame work 4.92 ms."
                % (ms, frame, "runtime sequence %d, " % seq if seq else ""))

    sampler_3 = "took 3 samples, the last in nvwgf2umx.dll+0x1a2b3c4"

    def freeze(ms=1858.0, frame=47211, seq=44415, n=1, cycle=True, sampler=sampler_3):
        if cycle:
            c = "%.1f ms (%.1f ms from the pose wait's return to this Present, the previous cycle 11.2 ms)" % (ms, ms - 7.3)
        else:
            c = "unavailable (no two pose-wait returns: not the native path, or no open runtime session)"
        return ("monitor: FREEZE -- %.1f ms between Presents, ended now: frame %d, runtime sequence %d; runtime cycle %s; freeze %d of this session%s. "
                "A frame of 250 ms or more always gets this line and a LONG FRAME line, with no cap and no rate limit."
                % (ms, frame, seq, c, n, "; stall sampler %s" % sampler if sampler else ""))

    def counts_pairs(long_b=(40, 15, 4, 0, 1), blip_b=(330, 20, 1, 0, 0), unwritten_b=(1, 1, 0), over_unwritten=0, blips=True):
        """The counts freeze_book.h's formatCounts writes, in its order, with totals that add up (a case that breaks one does so on purpose)."""
        long_n, blip_n = sum(long_b), sum(blip_b)
        unwritten = sum(unwritten_b) + over_unwritten
        pairs = [("candidates", long_n + (blip_n if blips else 0)), ("long", long_n)]
        if blips:
            pairs.append(("blips", blip_n))
        pairs += [("written", long_n - unwritten), ("unwritten", unwritten), ("over_250ms", long_b[3] + long_b[4]), ("over_250ms_unwritten", over_unwritten)]
        pairs += [("long_" + k, v) for k, v in zip(BUCKETS, long_b)]
        if blips:
            pairs += [("blip_" + k, v) for k, v in zip(BUCKETS, blip_b)]
        pairs += [("unwritten_" + k, v) for k, v in zip(BUCKETS, unwritten_b)]
        return pairs

    def gcounts(reason="periodic", **kw):
        return ("monitor: long frame counts reason=%s %s; candidates are Present gaps over twice the runtime's predicted period; a candidate the runtime's "
                "cycle did not confirm is a blip, counted and not written; the others are long, written one every 5 s up to 60 a session; every frame of "
                "250 ms or more is written whatever the limit says, and over_250ms_unwritten counts any that was not."
                % (reason, " ".join("%s=%d" % kv for kv in counts_pairs(**kw))))

    def gworst(rank, of, ms, ended, frame, seq, note="; stalled in nvwgf2umx.dll+0x1a2b3c4 (3 samples, longest at 1003 ms, EDVR code on the stack)", cycle=None):
        return ("monitor: worst long frame %d of %d: %.1f ms between Presents, ended %s (frame %d, runtime sequence %d, runtime cycle %s)%s"
                % (rank, of, ms, ended, frame, seq, "%.1f ms" % ms if cycle is None else cycle, note))

    def stall(age, k, us=14, owner="nvwgf2umx.dll+0x1a2b3c4", tail="; EDVR code on the stack: no"):
        """(age in ms into the freeze, the line): one stall sampler sample."""
        return (age, "stall: the render thread stalled %d ms in ntdll.dll+0x9d5c4; owner %s; stack ntdll.dll+0x9d5c4 < KERNELBASE.dll+0x4f2 < %s < dxgi.dll+0x1234 "
                     "< EliteDangerous64.exe+0x99c0f4; sample %d of 3, last Present returned in frame 47210, thread 12345, suspended %d us%s."
                % (age, owner, owner, k, us, tail))

    def stall_fail(k, at, why="suspend_failed"):
        return (at, "stall: sample %d of 3 at %d ms failed: %s; last Present returned in frame 47210, thread 12345." % (k, at, why))

    def sampler_counts(reason="session_close", samples=3, failures=0, longest_us=41):
        return ("stall sampler counts reason=%s episodes=1 samples=%d skipped_rate_limit=0 failures=%d longest_suspend_us=%d longest_stall_ms=1858; an episode "
                "is a run of frames with no Present for 150 ms or more; skipped_rate_limit counts episodes whose first sample the rate limit refused; a stall "
                "of any length is also a FREEZE line when it reached 250 ms." % (reason, samples, failures, longest_us))

    def gpu_stall(pairs, more=0):
        listed = "".join("%s sequence %d %.1f ms" % ("," if i else "", s, ms) for i, (s, ms) in enumerate(pairs))
        n = len(pairs) + more
        return ("EDVR GPU census, frame gap: the time on the GPU clock from the end of one frame's last EDVR-timed span on the game's device (after the door "
                "work) to the start of the next frame's first, p50 0.42 ms, p95 1.10 ms, max 3.30 ms over 2699 pairs of 2700 valid frames, %d pair%s over 1 s "
                "KEPT as stalls (not in the percentiles or the max; longest %.1f ms; each is the gap before that runtime sequence):%s%s. This is an upper "
                "bound on GPU idle, not idle: SteamVR's compositor (another process on the same GPU), the runtime's transfers and the Present copy run in it "
                "too, so a gap of about 1 ms is not proof of idleness; a gap near 0 says the GPU never waited."
                % (n, "" if n == 1 else "s", max(ms for _, ms in pairs), listed, " (and %d more)" % more if more else ""))

    # ---- the runtime log's lines ----
    def cycle_fields(seq, ms, split="ok", **over):
        base = [("sequence", seq), ("cycle_ms", "%.4f" % ms), ("period_ms", "11.1111"), ("game_before_first_submit", "3.1000"),
                ("first_submit_roundtrip", "0.9000"), ("first_submit_owner_body", "0.2000"), ("between_eye_calls", "0.4000"),
                ("second_submit_roundtrip", "0.8000"), ("second_submit_owner_body", "0.2000"), ("first_submit_render_park", "0.1000"),
                ("second_submit_render_park", "0.1000"), ("post_second_submit_to_next_wait", "%.4f" % (ms - 7.3)), ("present_split", split)]
        if split == "ok":
            base += [("pre_present", "0.07"), ("present_hook", "%.1f" % (ms - 7.5)), ("hook_before_real", "0.01"), ("hook_real_present", "%.1f" % (ms - 7.6)),
                     ("hook_after_real", "0.01"), ("hook_render_callback", "0.03"), ("post_present", "0.1")]
        base += [("next_wait_roundtrip", "1.2000"), ("next_wait_owner_body", "0.3000"), ("units", "wall_ms")]
        return ",".join("%s=%s" % ((k, over.get(k, v))) for k, v in base)

    def native_cycle(seq, ms, **kw):
        return "native_long_cycle,%s" % cycle_fields(seq, ms, **kw)

    def native_worst(rank, of, end, seq, ms, **kw):
        return "native_long_cycle_worst,rank=%d,of=%d,utc=%s,%s" % (rank, of, utc(end), cycle_fields(seq, ms, **kw))

    def rcounts(head="native_long_cycle_counts", reason="periodic", count=None, **kw):
        """The runtime's counts line: no blips (it judges by its own cycle), `count` and `logged` the old fields."""
        pairs = counts_pairs(blips=False, **kw)
        count = dict(pairs)["long"] if count is None else count
        return "%s,%scount=%d,logged=%d,threshold=2x_period,%s" % (head, "reason=%s," % reason if reason else "", count, count,
                                                                 ",".join("%s=%d" % kv for kv in pairs))

    old_summary = "native_long_cycle_summary,count=88,logged=1,threshold=2x_period"

    # ---- the two logs of a flight ----
    gopen = [g(T0, "EDVR log -- unofficial VR fixes for Elite Dangerous: Odyssey"),
             g(T0 + 0.002, "version %s (build 68C0A1F2) -- this DLL was linked 2026-10-01 14:20:00 UTC" % VERSION)]
    ropen = [r(T0 + 0.3, "module_init,version=%s,durable_log=1" % VERSION)]
    # The sampler's state line as stall_watch.cpp writes it (the numbers are PolicyConfig's defaults), and the same line as the first design wrote it.
    armed = g(T0 + 1.5, "stall sampler: armed. The render thread (thread 12345) is watched from its first Present: a stall is no Present for 150 ms, and the "
                        "thread is stopped for a few tens of microseconds at 150, 500 and 1000 ms of it, its stack copied and the thread released, and the log then "
                        "names the modules on it (stall: lines). At most 200 episodes a session, 6 back to back and one more every 2 s. In this process only: no "
                        "other thread, no other process, nothing written to the thread. advanced.freeze_location = off turns it off.")
    armed_short = g(T0 + 1.5, "stall sampler: armed (the render thread is sampled at roughly 150, 500 and 1000 ms of a stall)")
    sampler_off = g(T0 + 1.5, "stall sampler: off (advanced.freeze_location = off). The log will not name where the render thread was during a freeze; the FREEZE "
                              "and LONG FRAME lines are unaffected.")
    sampler_off_short = g(T0 + 1.5, "stall sampler: off (advanced.freeze_location = off).")
    sampler_failed = g(T0 + 1.5, "stall sampler: could not start its watchdog thread; no stall will be sampled.")
    healthy_items = [stall(163, 1), stall(512, 2), stall(1003, 3)]
    counts_kw = dict(long_b=(40, 15, 4, 0, 1))                              # the graphics half's: 351 blips beside, two long frames unwritten
    rt_counts_kw = dict(long_b=(40, 2, 2, 0, 1), unwritten_b=(0, 0, 0))     # the runtime's own: the same freeze, every long cycle written

    def gfx_flight(items=None, sampler=None, long=True, fkw=None, gpu=((44415, 1851.2),), gpu_more=0, counts=None, worst=True, scounts=True, ms=1858.0,
                   end=END):
        """A graphics log with one freeze of `ms` ending at `end`: the stall sampler's `items` inside it, its LONG FRAME and FREEZE lines, then the GPU
        census, the counts and the worst list as the periodic pass writes them and again at the session's close."""
        sampler = armed if sampler is None else sampler
        items = healthy_items if items is None else items
        counts = counts_kw if counts is None else counts
        seq = (fkw or {}).get("seq", 44415)
        frame = (fkw or {}).get("frame", 47211)
        out = list(gopen)
        if sampler:
            out.append(sampler)
        out += [g(end - ms / 1000.0 + age / 1000.0, msg) for age, msg in items]
        if long:
            out.append(g(end, long_frame(ms, seq, frame)))
        out.append(g(end, freeze(ms, **(fkw or {}))))
        if gpu:
            out.append(g(end + 40.3, gpu_stall(list(gpu), gpu_more)))
        out.append(g(end + 40.4, gcounts("periodic", **counts)))
        if worst:
            out.append(g(end + 40.5, gworst(1, 1, ms, hms(end), frame, seq)))
        if scounts and sampler and "armed" in sampler:
            out.append(g(CLOSE, sampler_counts()))
        out.append(g(CLOSE + 0.001, gcounts("session_close", **counts)))
        if worst:
            out.append(g(CLOSE + 0.002, gworst(1, 1, ms, hms(end), frame, seq)))
        return out

    def rt_flight(cycles=None, counts=None, summary=True, worst=True):
        """A runtime log: `cycles` as (end, sequence, ms, field overrides), the counts every five minutes, the summary and the worst list at the close."""
        cycles = [(END, 44415, 1858.0, {})] if cycles is None else cycles
        counts = rt_counts_kw if counts is None else counts
        out = list(ropen)
        out += [r(t, native_cycle(seq, ms, **kw)) for t, seq, ms, kw in cycles]
        out.append(r(END + 40.5, rcounts(**counts)))
        if summary:
            out.append(r(CLOSE, rcounts("native_long_cycle_summary", None, **counts) if summary is True else summary))
        if worst:
            top = sorted(cycles, key=lambda c: -c[2])[:5]
            out += [r(CLOSE + 0.001, native_worst(i, len(top), t, seq, ms, **kw)) for i, (t, seq, ms, kw) in enumerate(top, 1)]
        return out

    old_gfx = list(gopen) + [g(END, long_frame(1858.0, 44415)), g(END + 1.0, "monitor: 4 dropped or long frames were logged this session (of 60 at most).")]
    old_rt = list(ropen) + [r(END - 0.001, native_cycle(44415, 1858.0)), r(CLOSE, old_summary)]

    tmp = tempfile.mkdtemp(prefix="edvr_freezes_")
    serial = [0]
    violations = []
    real_open = builtins.open

    @contextlib.contextmanager
    def read_only():
        """--freezes writes nothing: inside this, an open for writing or a call that creates or removes a file is a recorded failure."""
        names = ("makedirs", "mkdir", "remove", "unlink", "rename", "replace", "rmdir")
        saved = {n: getattr(os, n) for n in names}

        def guarded_open(file, mode="r", *a, **k):
            if any(c in str(mode) for c in "wax+"):
                violations.append("open(%r, %r)" % (file, mode))
            return real_open(file, mode, *a, **k)

        def tripwire(name):
            def hit(*a, **k):
                violations.append("os.%s%r" % (name, a))
                raise OSError("--freezes must not write")
            return hit
        builtins.open = guarded_open
        for n in names:
            setattr(os, n, tripwire(n))
        try:
            yield
        finally:
            builtins.open = real_open
            for n, fn in saved.items():
                setattr(os, n, fn)

    def put(path, lines, crlf=False):
        nl = "\r\n" if crlf else "\n"
        with real_open(path, "wb") as f:
            f.write((nl.join(lines) + nl).encode("utf-8"))

    def run(gfx, rt=None, *extra, crlf=False, rt_name=RT_NAME, explicit_rt=False):
        """Write a flight's logs to a directory of their own and run --freezes on the graphics log through main(); (exit code, output). explicit_rt
        names the runtime log with --runtime-file instead of leaving it to be paired by the time its file name says."""
        serial[0] += 1
        d = os.path.join(tmp, "f%02d" % serial[0])
        os.makedirs(d)
        gp = os.path.join(d, GFX_NAME)
        put(gp, gfx, crlf)
        argv = ["--file", gp, "--freezes"] + list(extra)
        if rt is not None:
            put(os.path.join(d, rt_name), rt, crlf)
            if explicit_rt:
                argv += ["--runtime-file", os.path.join(d, rt_name)]
        buf = io.StringIO()
        with read_only(), contextlib.redirect_stdout(buf):
            rc = main(argv)
        return rc, buf.getvalue()

    def rx(pattern):
        """A whitespace-flexible line pattern for the tables: lines anchored, runs of spaces matched by ` +`."""
        return re.compile(pattern, re.M)

    def seen(item, out):
        return item.search(out) is not None if hasattr(item, "search") else item in out

    def case(what, result, rc_want, verdict, *has, absent=()):
        rc, out = result
        problems = []
        if rc != rc_want:
            problems.append("exit %d, wanted %d" % (rc, rc_want))
        if verdict and ("freezes verdict: %s (" % verdict) not in out:
            problems.append("the verdict is not %s" % verdict)
        problems += ["lacks %r" % getattr(h, "pattern", h) for h in has if not seen(h, out)]
        problems += ["has %r" % getattr(a, "pattern", a) for a in absent if seen(a, out)]
        if problems:
            fail("%s: %s:\n%s" % (what, "; ".join(problems), out))

    no_stop_warn = ("  STOP  ", "  WARN  ")

    try:
        # ---- the parser, on the exact lines ----
        fz = parse_freezes(os.path.join(tmp, GFX_NAME), "\n".join(gfx_flight()) + "\n", os.path.join(tmp, RT_NAME), "\n".join(rt_flight()) + "\n")
        f0 = fz["freezes"][0] if len(fz["freezes"]) == 1 else None
        if (f0 is None or (f0["ms"], f0["frame"], f0["seq"], f0["n"], f0["cycle_ms"], f0["head_ms"], f0["prev_ms"], f0["sampler"])
                != (1858.0, 47211, 44415, 1, 1858.0, 1850.7, 11.2, sampler_3)):
            fail("the FREEZE line reads as %r" % (f0,))
        if (len(fz["stalls"]), [s["us"] for s in fz["stalls"]], fz["stalls"][0]["stack"][2], fz["stalls"][0]["owner"], fz["stalls"][0]["edvr"]) != (
                3, [14, 14, 14], "nvwgf2umx.dll+0x1a2b3c4", "nvwgf2umx.dll+0x1a2b3c4", False):
            fail("the stall lines read as %r" % fz["stalls"])
        if fz["gfx_counts"][0]["kv"]["over_250ms_unwritten"] != 0 or fz["gfx_counts"][0]["kv"]["blip_lt50"] != 330 or fz["gfx_counts"][-1]["reason"] != "session_close":
            fail("the graphics counts lines read as %r" % fz["gfx_counts"])
        if fz["rt_counts"][-1]["kind"] != "summary" or fz["rt_counts"][0]["reason"] != "periodic" or "blips" in fz["rt_counts"][0]["kv"]:
            fail("the runtime counts lines read as %r" % fz["rt_counts"])
        if fz["gpu"][0]["pairs"] != [{"seq": 44415, "ms": 1851.2}] or fz["gpu"][0]["n"] != 1:
            fail("the GPU census line reads as %r" % fz["gpu"])
        w = fz["rt_worst_sets"][-1][0]
        day = (datetime.date(2026, 10, 1) - EPOCH.date()).days * DAY
        if (w["rank"], w["of"], w["seq"], w["ms"]) != (1, 1, 44415, 1858.0) or w["t"] is None or abs(w["t"] - (day + END)) > 1e-6:
            fail("the runtime worst line reads as %r (its utc= should come out at local %s)" % (w, hms(END)))
        if fz["cycles"][0]["src"] != "line" or len(fz["cycles"]) != 1 or abs(fz["cycles"][0]["t"] - (day + END)) > 1e-6:
            fail("the runtime cycle and its worst-list line should be one cycle, the line's, at local %s: %r" % (hms(END), fz["cycles"]))
        if fz["unparsed"]["count"]:
            fail("exact lines were left unparsed: %r" % fz["unparsed"])

        # ---- the healthy flight: one freeze, every line there, PASS ----
        healthy = run(gfx_flight(), rt_flight(), "--expect-build", VERSION)
        case("a healthy flight with a 1858 ms freeze", healthy, 0, "PASS",
             "[edvr] freezes: %s, runtime log %s; build %s; 1 FREEZE line(s), 1 runtime cycle(s) of 250 ms or more" % (GFX_NAME, RT_NAME, VERSION),
             "runtime log's UTC prefix is converted at UTC+08:00",
             "[edvr] clock check: 1 LONG FRAME line(s) share a runtime sequence with a native_long_cycle line, +0 ms apart at the median",
             "FREEZES (1 FREEZE line(s)", "RUNTIME CYCLES of 250 ms or more (1,", "COUNTS (", "WORST (graphics log", "WORST (runtime log", "STALL SAMPLER: armed",
             "GPU CLOCK (",
             rx(r"^  1 +23:24:19\.790 +1858\.0 +47211 +44415 +1858\.0$"),
             "the graphics half's view: 1850.7 ms from the pose wait's return to this Present, the previous cycle 11.2 ms",
             "LONG FRAME line: 23:24:19.790, 1858.0 ms, runtime sequence 44415",
             "runtime cycle: native_long_cycle sequence 44415, 1858.0 ms, ended 23:24:19.790, main phase real_Present 1850.4 ms",
             "stall sample 1 of 3 at 23:24:18.095: stalled 163 ms in ntdll.dll+0x9d5c4, owner nvwgf2umx.dll+0x1a2b3c4, suspended 14 us, EDVR code on the stack: no",
             "GPU clock: the GPU census kept the gap before sequence 44415, 1851.2 ms, as a stall",
             "the FREEZE line's stall sampler clause: took 3 samples, the last in nvwgf2umx.dll+0x1a2b3c4",
             "samples by owner module (3 sample line(s), 3 inside a freeze, 0 outside any): nvwgf2umx.dll 3",
             "sequence 44415  1851.2 ms  FREEZE #1 (frame 47211, 1858.0 ms between Presents)",
             rx(r"^  23:24:19\.790 +44415 +1858\.0 +real_Present 1850\.4 ms +#1 \(frame 47211\)$"),
             rx(r"^  1 of 1 +1858\.0 ms +ended 23:24:19\.790 +frame 47211 +sequence 44415 +runtime cycle 1858\.0 ms +"
                r"stalled in nvwgf2umx\.dll\+0x1a2b3c4 \(3 samples, longest at 1003 ms, EDVR code on the stack\)$"),
             rx(r"^  1 of 1 +1858\.0 ms +ended 23:24:19\.790 +sequence 44415 +main phase real_Present 1850\.4 ms +\(utc=2026-10-01T15:24:19\.790Z\)$"),
             rx(r"^  under 50 ms +40 +330 +1 +40 +0$"), rx(r"^  250-1000 ms +0 +0 +- +0 +-$"), rx(r"^  1 s and over +1 +0 +- +1 +-$"),
             rx(r"^  total +60 +351 +2 +45 +0$"),
             "  graphics: candidates 411, written 58; over 250 ms 1, of them unwritten 0",
             "  runtime:  candidates 45, written 45; over 250 ms 1, of them unwritten 0; count 45, logged 45",
             "latest counts (reason=session_close, 23:59:00.000): episodes=1 samples=3 skipped_rate_limit=0 failures=0 longest_suspend_us=41 longest_stall_ms=1858",
             "  PASS  INSTRUMENT: a `monitor: long frame counts` line is in the graphics log (2 line(s), the last reason=session_close at 23:59:00.001): the freeze "
             "logging ran; the runtime log has 2 summary or counts line(s) with the counts too",
             "  PASS  UNWRITTEN FREEZES: over_250ms_unwritten=0 in every counts line that carries it (2 graphics, 2 runtime)",
             "  PASS  FREEZE LINES: each of the 1 FREEZE line(s) has its LONG FRAME line, and each of the 1 runtime cycle(s) of 250 ms or more has its FREEZE line",
             "  PASS  SAMPLER: the stall sampler named an owner for 1 of 1 freeze(s): nvwgf2umx.dll+0x1a2b3c4 (3 sample(s))",
             "  PASS  SUSPENSION: the longest a stall sample held the render thread stopped was 41 us", "freezes verdict: PASS (0 STOP, 0 WARN)",
             "[edvr] build matches: %s" % VERSION, "[edvr] runtime build matches: %s" % VERSION,
             absent=no_stop_warn + ("NO FREEZE", "runtime only", "NO LONG FRAME LINE", "do not add up"))
        case("the same flight with CRLF line ends", run(gfx_flight(), rt_flight(), crlf=True), 0, "PASS", "  PASS  SAMPLER:", "  PASS  FREEZE LINES:",
             absent=no_stop_warn)

        # ---- a log from before the freeze logging ----
        old = run(old_gfx, old_rt)
        case("an old-build log with none of the new lines", old, 3, None,
             "[edvr] freezes: none of the freeze-diagnostics lines is in this log", "so it is from a build before the freeze logging.",
             absent=("freezes verdict", "FREEZES (", "  PASS  ", "  STOP  "))
        sentence = [l for l in old[1].splitlines() if l.startswith("[edvr] freezes:")]
        if len(sentence) != 1 or ". " in sentence[0] or not sentence[0].endswith("."):
            fail("the old-build message should be one line and one sentence: %r" % sentence)
        case("an old-build graphics log alone", run(old_gfx), 3, None, "none of the freeze-diagnostics lines", "runtime log: NONE FOUND")
        case("a graphics log with no [HH:MM:SS.mmm] lines (a runtime log given as the log)", run(rt_flight(), None), 3, None,
             "the graphics log has no [HH:MM:SS.mmm] lines", "--runtime-file")

        # ---- the instrument's own proof: the counts ----
        case("over_250ms_unwritten=2 in the graphics counts line is a bug and a STOP",
             run(gfx_flight(counts=dict(long_b=(40, 15, 4, 0, 1), over_unwritten=2)), rt_flight()), 1, "STOP",
             "  STOP  UNWRITTEN FREEZES: over_250ms_unwritten=2 in the graphics counts line at", "a frame of 250 ms or more got no line",
             "freezes verdict: STOP (1 STOP, 0 WARN)")
        case("over_250ms_unwritten above 0 in the runtime's counts line is a STOP too",
             run(gfx_flight(), rt_flight(counts=dict(long_b=(40, 2, 2, 0, 1), over_unwritten=1))), 1, "STOP",
             "  STOP  UNWRITTEN FREEZES: over_250ms_unwritten=1 in the runtime counts line at")
        case("the counts lines that do not add up are named, and change no verdict",
             run([l.replace(" long=60 ", " long=61 ") for l in gfx_flight()], rt_flight()), 0, "PASS",
             "the graphics counts do not add up: long=61 but its size buckets add to 60")
        no_gfx_counts = [l for l in gfx_flight() if "long frame counts" not in l]
        case("no graphics counts line: a WARN that says why (an older DLL, or a session that never closed)", run(no_gfx_counts, rt_flight()), 0, "WARN",
             "  WARN  INSTRUMENT: no `monitor: long frame counts` line in the graphics log", "a session that ended without a clean runtime close in under five minutes",
             "  PASS  UNWRITTEN FREEZES: over_250ms_unwritten=0 in every counts line that carries it (0 graphics, 2 runtime)",
             "COUNTS (the latest counts line of each half: graphics none;", rx(r"^  under 50 ms +- +- +- +40 +0$"), absent=("  PASS  INSTRUMENT", "STOP  "))
        case("no counts line anywhere: UNWRITTEN FREEZES has nothing to judge, and says so", run(no_gfx_counts, [l for l in rt_flight(summary=old_summary)
             if "native_long_cycle_counts" not in l]), 0, "WARN", "  note  UNWRITTEN FREEZES: no counts line carries over_250ms_unwritten, so there is nothing to judge",
             absent=("  PASS  UNWRITTEN FREEZES", "  STOP  "))
        case("no runtime log: INSTRUMENT warns, the graphics half still reads", run(gfx_flight(), None), 0, "WARN",
             "runtime log: NONE FOUND", "[edvr] freezes: %s, runtime log none; build %s" % (GFX_NAME, VERSION),
             "  WARN  INSTRUMENT: no runtime log was read", "runtime cycle: - (no runtime log was read)", "  PASS  INSTRUMENT:", "  PASS  FREEZE LINES:",
             absent=("RUNTIME CYCLES",))
        case("a runtime log with no summary and no counts line",
             run(gfx_flight(), [l for l in rt_flight() if "native_long_cycle_summary" not in l and "native_long_cycle_counts" not in l]), 0, "WARN",
             "  WARN  INSTRUMENT: the runtime log has no native_long_cycle_summary or native_long_cycle_counts line", "COUNTS (")
        old_form = run(gfx_flight(), [l for l in rt_flight(summary=old_summary) if "native_long_cycle_counts" not in l])
        case("the runtime summary in its old three-field form: tolerated, every count it lacks prints `-`", old_form, 0, "WARN",
             "  WARN  INSTRUMENT: the runtime log's summary has only the three old fields",
             "runtime:  candidates -, written -; over 250 ms -, of them unwritten -; count 88, logged 1",
             "  PASS  INSTRUMENT: a `monitor: long frame counts` line", "  PASS  UNWRITTEN FREEZES: over_250ms_unwritten=0 in every counts line that carries it (2 graphics, 0 runtime)",
             absent=("  STOP  ",))
        if re.search(r"^  under 50 ms +40 +330 +1 +-", old_form[1], re.M) is None:
            fail("the old summary's cells should read `-` beside the graphics counts:\n%s" % old_form[1])

        # ---- FREEZE LINES: the freeze and the runtime cycle must each have the other ----
        game_held = dict(game_before_first_submit="330.2000", post_second_submit_to_next_wait="10.0000", present_hook="5.0", hook_real_present="4.0")
        runtime_only = [(END, 44415, 1858.0, {}), (T0 + 600.0, 51000, 349.0, game_held)]
        case("a runtime-only 349 ms cycle beside a graphics counts line is a STOP", run(gfx_flight(), rt_flight(cycles=runtime_only)), 1, "STOP",
             "  STOP  FREEZE LINES: 1 runtime cycle(s) of 250 ms or more have no FREEZE line although the graphics log has a counts line",
             "sequence 51000, 349.0 ms, ended 23:30:00.000", "RUNTIME CYCLES of 250 ms or more (2,",
             rx(r"^  23:30:00\.000 +51000 +349\.0 +game_before_first_submit 330\.2 ms +runtime only$"), "#1 (frame 47211)",
             "freezes verdict: STOP (1 STOP, 0 WARN)")
        no_counts = [l for l in gfx_flight(items=[], long=True) if "long frame counts" not in l and "worst long frame" not in l]
        case("the same cycle with no graphics counts line is a note, not a STOP", run(no_counts, rt_flight(cycles=runtime_only)), 0, "WARN",
             "  note  FREEZE LINES: 1 runtime cycle(s) of 250 ms or more have no FREEZE line, but the graphics log has no counts line", absent=("  STOP  ",))
        case("a cycle only in the worst list is still a cycle of 250 ms or more",
             run(gfx_flight(), [l for l in rt_flight(cycles=runtime_only) if "native_long_cycle,sequence=51000," not in l]), 1,
             "STOP", "[worst list only]", "runtime only", "  STOP  FREEZE LINES:")
        case("one cycle in the log and in the worst list is one row",
             run(gfx_flight(), rt_flight()), 0, "PASS", "RUNTIME CYCLES of 250 ms or more (1,", absent=("[worst list only]",))
        case("a runtime cycle whose sequence is within one of the freeze's goes with it",
             run(gfx_flight(), rt_flight(cycles=[(END + 0.200, 44416, 1858.0, {})])), 0, "PASS", "runtime cycle: native_long_cycle sequence 44416",
             "#1 (frame 47211)", absent=("runtime only",))
        case("a runtime cycle two sequences away and 1 s away is not the freeze's",
             run(gfx_flight(), rt_flight(cycles=[(END + 1.0, 44417, 1858.0, {})])), 1, "STOP", "runtime only",
             "runtime cycle: none with sequence 44415 or within one of it")
        case("a runtime log whose cycle times are two hours off the graphics log's: the clock check says so, the sequence still joins",
             run(gfx_flight(), rt_flight(cycles=[(END + 7200.0, 44415, 1858.0, {})])), 0, "PASS",
             "[edvr] WARNING: 1 LONG FRAME line(s) share a runtime sequence with a native_long_cycle line, -7200 s apart at the median: the UTC offset is wrong",
             "runtime cycle: native_long_cycle sequence 44415", "#1 (frame 47211)")
        case("a runtime log of the day before does not overlap the graphics log",
             run(gfx_flight(), ["2026-09-30 04:00:00.300 UTC pid=4242 tid=1 module_init,version=%s,durable_log=1" % VERSION],
                 rt_name="edvr_openxr_20260930_120000_100_4242.log", explicit_rt=True), 0, "WARN",
             "WARNING: the two logs' time spans do not overlap", "runtime log's UTC prefix is converted at UTC+08:00")
        case("a runtime cycle with no matching sequence but within 150 ms of the freeze's end goes with it",
             run(gfx_flight(fkw=dict(seq=0)), rt_flight(cycles=[(END + 0.140, 44417, 1858.0, {})])), 0, "PASS", "#1 (frame 47211)", absent=("runtime only",))
        case("a FREEZE line with no LONG FRAME line is a WARN", run(gfx_flight(long=False), rt_flight()), 0, "WARN",
             "LONG FRAME line: NO LONG FRAME LINE", "  WARN  FREEZE LINES: 1 FREEZE line(s) have no LONG FRAME line", "frame 47211 at 23:24:19.790",
             "freezes verdict: WARN (0 STOP, 1 WARN)", absent=("  STOP  ", "  PASS  FREEZE LINES"))
        near = gfx_flight(long=False) + [g(END + 0.030, long_frame(41.0, 44418, 47212))]
        case("a LONG FRAME line within 50 ms of the freeze's end is its own, whatever its sequence", run(near, rt_flight()), 0, "PASS",
             "LONG FRAME line: 23:24:19.820, 41.0 ms, runtime sequence 44418", absent=("NO LONG FRAME LINE",))
        far = gfx_flight(long=False) + [g(END + 0.060, long_frame(41.0, 44418, 47212))]
        case("a LONG FRAME line 60 ms away with another sequence is not", run(far, rt_flight()), 0, "WARN", "NO LONG FRAME LINE")
        case("a LONG FRAME line of the same sequence is its own, however far its time",
             run(gfx_flight(long=False) + [g(END + 5.0, long_frame(1858.0, 44415))], rt_flight()), 0, "PASS", "LONG FRAME line: 23:24:24.790, 1858.0 ms, runtime sequence 44415")
        case("a FREEZE line with no runtime cycle and no sequence: unavailable, and the time joins it",
             run(gfx_flight(fkw=dict(cycle=False, seq=0, sampler=None), long=False), rt_flight()), 0, "WARN",
             "unavailable", "runtime cycle: native_long_cycle sequence 44415", "NO LONG FRAME LINE", absent=("the graphics half's view", "stall sampler clause"))

        # ---- the stall sampler ----
        owners = [stall(163, 1, owner="nvwgf2umx.dll+0x1a2b3c4"), stall(512, 2, owner="dxgi.dll+0x1234"), stall(1003, 3, owner="nvwgf2umx.dll+0x1a2b3c4")]
        case("the owner census across three samples", run(gfx_flight(items=owners), rt_flight()), 0, "PASS",
             "samples by owner module (3 sample line(s), 3 inside a freeze, 0 outside any): nvwgf2umx.dll 2, dxgi.dll 1",
             "  PASS  SAMPLER: the stall sampler named an owner for 1 of 1 freeze(s): nvwgf2umx.dll+0x1a2b3c4 (2 sample(s)), dxgi.dll+0x1234 (1 sample(s))")
        failed = [stall(163, 1), stall_fail(2, 512), stall(1003, 3)]
        case("a failed sample is listed and counted by reason", run(gfx_flight(items=failed), rt_flight()), 0, "PASS",
             "stall sample 2 of 3 at 23:24:18.444: FAILED at 512 ms (suspend_failed)", "failed samples (1) by reason: suspend_failed 1",
             "samples by owner module (2 sample line(s)", "  PASS  SAMPLER:")
        case("a freeze whose samples all failed named no owner: a WARN", run(gfx_flight(items=[stall_fail(1, 150, "suspend_failed"), stall_fail(2, 512, "no_thread")]), rt_flight()),
             0, "WARN", "  WARN  SAMPLER: the stall sampler is armed and 1 freeze(s) were logged, but no `stall:` sample with an owner falls inside any of them (2 failed sample line(s) do)",
             "failed samples (2) by reason: no_thread 1, suspend_failed 1", absent=("  PASS  SAMPLER", "STOP  "))
        no_sample = "took no sample (the rate limit, a failed suspend, or the stall began before the sampler was armed)"
        case("armed, a freeze, and no stall line at all: a WARN that quotes the FREEZE line's clause",
             run(gfx_flight(items=[], fkw=dict(sampler=no_sample)), rt_flight()), 0, "WARN",
             "  WARN  SAMPLER: the stall sampler is armed and 1 freeze(s) were logged, but no `stall:` sample with an owner falls inside any of them; the FREEZE line(s) "
             "say: %s" % no_sample, "the FREEZE line's stall sampler clause: %s" % no_sample)
        case("the first design's armed line (`armed (...)`) reads as armed too", run(gfx_flight(sampler=armed_short), rt_flight()), 0, "PASS",
             "STALL SAMPLER: armed (the render thread is sampled at roughly 150, 500 and 1000 ms of a stall)", "  PASS  SAMPLER:", absent=("  WARN  PARSE",))
        case("a sampler that could not start its watchdog thread is a WARN of its own",
             run(gfx_flight(items=[], sampler=sampler_failed, fkw=dict(sampler="not running (the watchdog thread never started)"), scounts=False), rt_flight()),
             0, "WARN", "STALL SAMPLER: could not start its watchdog thread; no stall will be sampled.",
             "  WARN  SAMPLER: the stall sampler could not start its watchdog thread, so no freeze in this log was sampled",
             "the FREEZE line's stall sampler clause: not running (the watchdog thread never started)", absent=("  PASS  SAMPLER", "  WARN  PARSE", "  STOP  "))
        case("a stall line a minute before the freeze belongs to no freeze",
             run(gfx_flight(items=[(-60000, stall(150, 1)[1]), stall(163, 1)]), rt_flight()), 0, "PASS",
             "samples by owner module (2 sample line(s), 1 inside a freeze, 1 outside any)", "  PASS  SAMPLER:")
        for off_line in (sampler_off, sampler_off_short):
            case("the sampler off: its state is shown and no SAMPLER verdict is made", run(gfx_flight(items=[], sampler=off_line,
                 fkw=dict(sampler="off (advanced.freeze_location = off)")), rt_flight()), 0, "PASS",
                 "STALL SAMPLER: off (advanced.freeze_location = off).", "the FREEZE line's stall sampler clause: off (advanced.freeze_location = off)",
                 absent=("  SAMPLER:", "  WARN  ", "  STOP  ", "  PASS  SAMPLER", "  PASS  SUSPENSION"))
        case("no sampler lines at all (a build between the two commits): no STALL SAMPLER section, no SAMPLER verdict",
             run(gfx_flight(items=[], sampler="", fkw=dict(sampler=None), scounts=False), rt_flight()), 0, "PASS", absent=("STALL SAMPLER", "SAMPLER:", "SUSPENSION"))
        case("EDVR code on the stack is reported with its innermost frame",
             run(gfx_flight(items=[stall(163, 1), stall(512, 2, tail="; EDVR code on the stack: yes (2 frames, innermost d3d11.dll+0x1c4a3)"),
                                   stall(1003, 3, tail="; EDVR code on the stack: yes (1 frame, innermost openvr_api.dll+0x77)")]), rt_flight()), 0, "PASS",
             "EDVR code on the stack: yes (2 frames, innermost d3d11.dll+0x1c4a3)", "EDVR code on the stack: yes (1 frame, innermost openvr_api.dll+0x77)",
             "  EDVR code on the stack: 2 of 3 sample(s); EDVR's own code was in the freeze:", "    sample 2 of 3 at 23:24:18.444: yes (2 frames, innermost d3d11.dll+0x1c4a3)",
             "    sample 3 of 3 at 23:24:18.935: yes (1 frame, innermost openvr_api.dll+0x77)")
        case("a sample taken with the stack pointer outside the thread's stack",
             run(gfx_flight(items=[stall(163, 1, tail=" (the stack pointer was outside the thread's stack: one frame only); EDVR code on the stack: no"),
                                   stall(512, 2, tail=" (the stack pointer was outside the thread's stack: one frame only); EDVR code on the stack: yes (1 frame, innermost "
                                                      "d3d11.dll+0x1c4a3)")]), rt_flight()), 0, "PASS", "samples by owner module (2 sample line(s), 2 inside a freeze",
             "suspended 14 us, EDVR code on the stack: no; the stack pointer was outside the thread's stack: one frame only",
             "suspended 14 us, EDVR code on the stack: yes (1 frame, innermost d3d11.dll+0x1c4a3); the stack pointer was outside the thread's stack: one frame only",
             "  samples taken with the stack pointer outside the thread's stack (one frame only): 2 of 2",
             "  EDVR code on the stack: 1 of 2 sample(s); EDVR's own code was in the freeze:", absent=("do not add up", "UNPARSED", "  WARN  PARSE"))
        case("a sample line of the older form (no EDVR clause) says `-`", run(gfx_flight(items=[stall(163, 1, tail=""), stall(512, 2, tail="")]), rt_flight()), 0, "PASS",
             "suspended 14 us, EDVR code on the stack: -", "  EDVR code on the stack: - (no sample line says)")
        def suspend(us):
            return run(gfx_flight(items=[stall(163, 1, us=us)]), rt_flight())
        case("suspended 60000 us is a STOP", suspend(60000), 1, "STOP", "  STOP  SUSPENSION: a stall sample held the render thread stopped for 60000 us (sample 1 of 3 at 23:24:18.095)")
        case("suspended 50000 us is a WARN, not a STOP", suspend(50000), 0, "WARN", "  WARN  SUSPENSION: a stall sample held the render thread stopped for 50000 us")
        case("suspended 5001 us is a WARN", suspend(5001), 0, "WARN", "  WARN  SUSPENSION:")
        case("suspended 5000 us is within the limit", suspend(5000), 0, "PASS", "  PASS  SUSPENSION: the longest a stall sample held the render thread stopped was 5000 us")
        case("a longer suspension in the counts line than in any sample line", run([l.replace("longest_suspend_us=41", "longest_suspend_us=70000") for l in gfx_flight()], rt_flight()),
             1, "STOP", "  STOP  SUSPENSION: a stall sample held the render thread stopped for 70000 us (longest_suspend_us in the counts line at 23:59:00.000)")

        # ---- GPU CLOCK ----
        case("two GPU stalls, one with a freeze's sequence and one without, and the pairs the line did not list",
             run(gfx_flight(gpu=((44415, 1851.2), (44500, 1200.0))), rt_flight()), 0, "PASS",
             "GPU CLOCK (the GPU census kept 2 frame-gap pair(s) over 1 s as stalls, in 1 line(s)", "sequence 44415  1851.2 ms  FREEZE #1 (frame 47211, 1858.0 ms between Presents)",
             "sequence 44500  1200.0 ms  no FREEZE line has this sequence")
        case("a GPU census line that lists fewer pairs than it kept", run(gfx_flight(gpu=((44415, 1851.2),), gpu_more=2), rt_flight()), 0, "PASS",
             "kept 3 frame-gap pair(s)", "(and 2 more the lines did not list)")
        case("no GPU stall: no GPU CLOCK section", run(gfx_flight(gpu=()), rt_flight()), 0, "PASS", absent=("GPU CLOCK", "GPU clock:"))

        # ---- the main phase of a cycle ----
        def phase_of(**kw):
            return run(gfx_flight(), rt_flight(cycles=[(END, 44415, 1858.0, kw)]))
        case("a hook whose real Present is under half of it keeps its own name", phase_of(hook_real_present="900.0"), 0, "PASS", "main phase present_hook 1850.5 ms")
        case("a hook whose real Present is over half of it is real_Present, with that call's ms", phase_of(hook_real_present="1000.0"), 0, "PASS",
             "main phase real_Present 1000.0 ms")
        case("a cycle held in the game's own work", phase_of(game_before_first_submit="1800.0", post_second_submit_to_next_wait="50.0", present_hook="40.0",
                                                            hook_real_present="10.0"), 0, "PASS", "main phase game_before_first_submit 1800.0 ms")
        case("a cycle held in the next wait", phase_of(next_wait_roundtrip="1900.0"), 0, "PASS", "main phase next_wait_roundtrip 1900.0 ms")
        unsplit = [(END, 44415, 1858.0, dict(split="lost_or_inflight_present_history"))]
        case("a cycle with no Present split uses the one phase after the second Submit", run(gfx_flight(), rt_flight(cycles=unsplit)), 0, "PASS",
             "main phase post_second_submit_to_next_wait 1850.7 ms (Present split unavailable: lost_or_inflight_present_history)")

        # ---- the worst lists: a later set replaces an earlier one ----
        two_sets = gfx_flight() + [g(CLOSE + 0.003, gworst(1, 2, 2400.0, "23:41:00.000", 51000, 51000, note="")),
                                   g(CLOSE + 0.004, gworst(2, 2, 1858.0, "23:24:19.790", 47211, 44415, note=""))]
        case("the latest worst set is the one printed, and only it", run(two_sets, rt_flight()), 0, "PASS",
             "WORST (graphics log: the latest of 3 set(s), 2 line(s), written 23:59:00.003):",
             rx(r"^  1 of 2 +2400\.0 ms +ended 23:41:00\.000 +frame 51000 +sequence 51000 +runtime cycle 2400\.0 ms$"),
             rx(r"^  2 of 2 +1858\.0 ms +ended 23:24:19\.790 +frame 47211 +sequence 44415 +runtime cycle 1858\.0 ms$"),
             absent=("stalled in nvwgf2umx.dll+0x1a2b3c4",))

        # ---- many freezes: every one gets a row, the longest FREEZE_DETAIL_MAX their full joins ----
        many_g = list(gopen) + [sampler_off]
        many_cycles = []
        for i in range(14):
            end_i, ms_i = T0 + 120.0 + 20.0 * i, 300.0 + 100.0 * i          # 300 ms up to 1600 ms
            many_g += [g(end_i, long_frame(ms_i, 44000 + i, 47000 + i)), g(end_i, freeze(ms_i, 47000 + i, 44000 + i, n=i + 1, sampler=None))]
            many_cycles.append((end_i, 44000 + i, ms_i, {}))
        many_g.append(g(CLOSE + 0.001, gcounts("session_close", long_b=(40, 15, 4, 12, 2))))
        many = run(many_g, rt_flight(cycles=many_cycles, counts=dict(long_b=(40, 2, 2, 12, 2), unwritten_b=(0, 0, 0))))
        case("fourteen freezes: a row each, the joins in full for the twelve longest", many, 0, "PASS",
             "FREEZES (14 FREEZE line(s); each covers [end - gap, end]; the joins are printed in full for the 12 longest):",
             rx(r"^  1 +23:22:00\.000 +300\.0 +47000 +44000 +300\.0$"), rx(r"^  14 +23:26:20\.000 +1600\.0 +47013 +44013 +1600\.0$"),
             "      joins: LONG FRAME line yes; runtime cycle 300.0 ms, main phase real_Present 292.4 ms",
             "      joins: LONG FRAME line yes; runtime cycle 400.0 ms, main phase real_Present 392.4 ms",
             "RUNTIME CYCLES of 250 ms or more (14,", absent=("runtime only", "joins: LONG FRAME line yes; runtime cycle 500.0 ms"))
        if many[1].count("\n      LONG FRAME line:") != 12 or many[1].count("\n      joins: ") != 2:
            fail("fourteen freezes should print 12 full joins and 2 summaries, not %d and %d:\n%s"
                 % (many[1].count("\n      LONG FRAME line:"), many[1].count("\n      joins: "), many[1]))

        # ---- no freeze at all ----
        quiet_counts = dict(long_b=(40, 15, 4, 0, 0), blip_b=(330, 20, 1, 0, 0))
        quiet_g = list(gopen) + [armed, g(T0 + 300.0, gcounts("periodic", **quiet_counts)), g(CLOSE, sampler_counts("session_close", 0, 0, 0)),
                                 g(CLOSE + 0.001, gcounts("session_close", **quiet_counts))]
        quiet_r = list(ropen) + [r(T0 + 300.0, rcounts(**quiet_counts)), r(CLOSE, rcounts("native_long_cycle_summary", None, **quiet_counts))]
        case("a flight with no frame of 250 ms or more", run(quiet_g, quiet_r), 0, "PASS", "  PASS  NO FREEZE: no frame of 250 ms or more (no FREEZE line, no runtime cycle of 250 ms or more)",
             "  PASS  INSTRUMENT:", "  PASS  UNWRITTEN FREEZES:", rx(r"^  1 s and over +0 +0 +- +0 +-$"),
             "0 FREEZE line(s), 0 runtime cycle(s)", absent=("FREEZES (", "RUNTIME CYCLES", "  WARN  ", "  STOP  ", "  PASS  FREEZE LINES", "PASS  SAMPLER"))
        case("no freeze and no runtime log", run(quiet_g, None), 0, "WARN", "  PASS  NO FREEZE: no frame of 250 ms or more (no FREEZE line; no runtime log was read",
             "  WARN  INSTRUMENT: no runtime log was read")

        # ---- the test trigger, advanced.freeze_test_ms: the sleep must be a FREEZE with its whole chain ----
        test_armed = "freeze test: advanced.freeze_test_ms = %d. Sixty seconds from now the render thread sleeps %d ms, once, to test the freeze lines and the " \
                     "stall sampler. Set it back to 0."
        test_sleep = "freeze test: the render thread sleeps %d ms now."
        edvr_yes = "; EDVR code on the stack: yes (3 frames, innermost d3d11.dll+0x52a1)"
        own = "d3d11.dll+0x52a1"

        def test_items(tail=edvr_yes, ms=1200):
            return [(-59000, test_armed % (ms, ms)), (4, test_sleep % ms), stall(154, 1, owner=own, tail=tail), stall(508, 2, owner=own, tail=tail),
                    stall(1015, 3, owner=own, tail=tail)]

        t_cycles = [(END, 44415, 1200.0, {})]
        t_head = "the deliberate 1200 ms sleep at 23:24:18.594 is FREEZE #1 (frame 47211, 1200.0 ms between Presents)"
        case("the test trigger's sleep is a FREEZE with its whole chain", run(gfx_flight(items=test_items(), ms=1200.0, gpu=((44415, 1190.4),)),
                                                                              rt_flight(cycles=t_cycles)), 0, "PASS",
             "freeze test: this freeze holds the deliberate sleep of 1200 ms (advanced.freeze_test_ms), logged at 23:24:18.594",
             "  PASS  FREEZE TEST: %s, with its LONG FRAME line, a runtime cycle of 1200.0 ms, 3 stall sample(s) naming %s, EDVR code on the stack in 3 of them" % (t_head, own),
             "samples by owner module (3 sample line(s), 3 inside a freeze, 0 outside any): d3d11.dll 3", "EDVR code on the stack: 3 of 3 sample(s); EDVR's own code was in the freeze:",
             absent=no_stop_warn + ("UNPARSED",))
        case("a test sleep whose samples show no EDVR code on the stack is a WARN", run(gfx_flight(items=test_items(tail="; EDVR code on the stack: no"), ms=1200.0),
                                                                                         rt_flight(cycles=t_cycles)), 0, "WARN",
             "  WARN  FREEZE TEST: %s, but the chain lacks EDVR code on the stack in any sample (the sleep is inside EDVR's own DLL)" % t_head)
        case("a test sleep with no runtime cycle of its length is a WARN", run(gfx_flight(items=test_items(), ms=1200.0), rt_flight(cycles=[(END, 44415, 800.0, {})])), 0, "WARN",
             "  WARN  FREEZE TEST: %s, but the chain lacks a runtime cycle of about that length" % t_head)
        case("a test sleep whose freeze has no LONG FRAME line and no stall samples lacks both", run(gfx_flight(items=test_items()[:2], ms=1200.0, long=False),
                                                                                                     rt_flight(cycles=t_cycles)), 0, "WARN",
             "  WARN  FREEZE TEST: %s, but the chain lacks its LONG FRAME line; a stall sample with an owner" % t_head)
        case("the test with the sampler off needs no samples", run(gfx_flight(items=test_items()[:2], ms=1200.0, sampler=sampler_off,
                                                                              fkw=dict(sampler="off (advanced.freeze_location = off)")), rt_flight(cycles=t_cycles)),
             0, "PASS", "  PASS  FREEZE TEST: %s, with its LONG FRAME line, a runtime cycle of 1200.0 ms, no stall samples (the sampler is not armed, so none were due)" % t_head)
        lonely = list(gopen) + [armed, g(T0 + 2.0, test_armed % (1200, 1200)), g(T0 + 62.0, test_sleep % 1200), g(CLOSE, gcounts("session_close", long_b=(40, 15, 4, 0, 0)))]
        case("a test sleep that no FREEZE line holds is a STOP", run(lonely, quiet_r), 1, "STOP",
             "  STOP  FREEZE TEST: the test slept 1200 ms at 23:21:02.000, a frame of 250 ms or more, and no FREEZE line holds it: it must always get a line",
             "freezes verdict: STOP (1 STOP, 0 WARN)")
        case("the key read and no sleep line: the session ended first", run(lonely[:-2] + lonely[-1:], quiet_r), 0, "WARN",
             "  WARN  FREEZE TEST: advanced.freeze_test_ms = 1200 was read, but the log has no `the render thread sleeps` line: the session ended before the sleep, 60 s in")
        short_sleep = list(gopen) + [armed, g(T0 + 62.0, test_sleep % 200), g(CLOSE, gcounts("session_close", long_b=(40, 15, 4, 0, 0)))]
        case("a test sleep under 250 ms owes no FREEZE line", run(short_sleep, quiet_r), 0, "PASS",
             "  note  FREEZE TEST: the test slept 200 ms at 23:21:02.000: under 250 ms, so no FREEZE line is due", absent=no_stop_warn)

        # ---- a line the reader cannot read ----
        bad = gfx_flight() + [g(CLOSE + 1.0, "monitor: FREEZE -- a format this reader never saw")]
        case("a line that opens like a FREEZE line and matches nothing", run(bad, rt_flight()), 0, "WARN",
             "  WARN  PARSE: 1 line(s) open like a freeze-diagnostics line but match none of the formats this reads", "a format this reader never saw")
        case("a log whose only freeze-diagnostics line cannot be read is not an old build", run(list(gopen) + [g(T0 + 5.0, "stall: something the sampler never wrote")], None), 0, "WARN",
             "  WARN  PARSE:", absent=("before the freeze logging",))

        # ---- the command line: the flag, the build checks, the exit codes ----
        case("--expect-build that does not match the graphics log", run(gfx_flight(), rt_flight(), "--expect-build", "0.17.0-9-g1234567"), 2, None,
             "BUILD MISMATCH", "log says   %s" % VERSION, absent=("freezes verdict",))
        other_rt = [l.replace(VERSION, "0.17.0-9-g1234567") for l in rt_flight()]
        case("--expect-build that the runtime log does not match", run(gfx_flight(), other_rt, "--expect-build", VERSION), 2, None,
             "BUILD MISMATCH (runtime log)", absent=("freezes verdict",))
        case("a runtime log of another build, with no --expect-build, is a printed warning", run(gfx_flight(), other_rt), 0, "PASS",
             "WARNING: the runtime log is from build 0.17.0-9-g1234567 and the graphics log from %s" % VERSION)
        serial[0] += 1
        d = os.path.join(tmp, "f%02d" % serial[0])
        os.makedirs(d)
        put(os.path.join(d, GFX_NAME), gfx_flight())
        put(os.path.join(d, "somewhere_else.log"), rt_flight())
        buf = io.StringIO()
        with read_only(), contextlib.redirect_stdout(buf):
            rc = main(["--file", os.path.join(d, GFX_NAME), "--freezes", "--runtime-file", os.path.join(d, "somewhere_else.log")])
        case("--runtime-file names the runtime log", (rc, buf.getvalue()), 0, "PASS", "runtime log somewhere_else.log")
        buf = io.StringIO()
        with read_only(), contextlib.redirect_stdout(buf):
            rc = main(["--file", os.path.join(d, GFX_NAME), "--freezes", "--runtime-file", os.path.join(d, "nope.log")])
        case("--runtime-file that is not there", (rc, buf.getvalue()), 1, None, "no such runtime log", absent=("freezes verdict",))
        buf = io.StringIO()
        with read_only(), contextlib.redirect_stdout(buf):
            rc = main(["--dir", d, "--tag", "openxr", "--freezes"])
        case("--freezes reads the graphics log, not a runtime --tag", (rc, buf.getvalue()), 1, None, "--freezes reads the graphics log (--tag gfx)")
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            try:
                main(["--help"])
            except SystemExit as e:
                if e.code not in (0, None):
                    fail("--help exited %r" % (e.code,))
        helped = buf.getvalue()
        if "--freezes" not in helped or "with --tally periodic or --freezes" not in " ".join(helped.split()):
            fail("--help should list --freezes and say --runtime-file serves it:\n%s" % helped)
        if "--freezes" not in (__doc__ or ""):
            fail("the module docstring should list --freezes")
        if violations:
            fail("--freezes wrote, or tried to: %s" % "; ".join(violations[:5]))
    except Exception:
        import traceback
        traceback.print_exc()
        print("freezes: the checks stopped at an exception (above)")
        ok = False
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    return ok


def self_test_slow_regime():
    """--freezes on the headset-lock arc's lines (docs/headset-lock-vdxr-2026-10-02.md): logs built from the exact lines the C++ writes (vram_watch.h,
    vendor_events.h, end_frame_episodes.h, slow_regime.h, perf_monitor.cpp's slow test, native_runtime_host.h's hold) and from a v0.18.0 flight's older
    lines, every case asserting the printed sections, the finding rows, the verdict and the exit code through main(). A log that holds a slow regime the vendor
    runtime, EDVR's copy or EDVR's own work holds never gets a PASS or WARN verdict, whether the build detected it itself or this reader put it back together
    from LONG FRAME, native_long_cycle, FREEZE and vScreen totals lines; a regime the game's own frame holds (INFO in a load, WARN in its scenes) or with no
    named owner sets neither SLOW nor exit 4, and the cases pin both sides, with a game-owned fixture. The pins tie each phrase the reader keys on to the
    source that writes it, and the run asserts that --freezes opens nothing for writing. Returns ok."""
    import builtins
    import contextlib
    import io
    import shutil
    ok = True

    def fail(msg):
        nonlocal ok
        print("slow regime: %s" % msg)
        ok = False

    root = repo_root()
    for rel, needles in (
            (("src", "openxr", "slow_regime.h"), (
                '"native_slow_regime,event=%s,%sregime=%u,duration_s=%.1f,window_s=%.1f,frames=%llu,fps=%.2f,display_hz=%.2f,fraction=%.3f,threshold=%.2f,frame_ms=%.2f,"',
                '"context=%s,loading_frames=%llu,empty_frames=%llu,"', 'enum class SlowContext { None, Scene, Loading, NoLayers };',
                'case SlowContext::Scene: return "scene";', 'case SlowContext::Loading: return "loading";', 'case SlowContext::NoLayers: return "no_layers";',
                'r.context = s.loading * 2 >= s.frames ? SlowContext::Loading : s.empty * 2 >= s.frames ? SlowContext::NoLayers : SlowContext::Scene;',
                '"held_by=%s,held_share=%.3f,vendor_share=%.3f,vendor_end_frame_ms=%.2f,vendor_wait_frame_ms=%.2f,vendor_swapchain_ms=%.2f,edvr_copy_ms=%.2f,"',
                '"edvr_work_ms=%.2f,game_ms=%.2f,end_frame_max_ms=%.2f,%s,units=wall_ms,summary=%s"',
                '"native_slow_regime,armed=1,threshold_fraction=%.2f,of=display_rate,hold_s=%u,end_s=%u,still_slow_s=%u,owner_share=%.2f,frames=xrEndFrame_returns,"',
                '"native_slow_regime_summary,%s%s%sregimes=%u,open=%u,frames=%llu,seconds=%llu,slow_seconds=%llu,threshold=%.2f"',
                'formatVramFigures(figures, sizeof(figures), *vram, \',\', "vram_")', 'std::snprintf(figures, sizeof(figures), "vram=unavailable")',
                '"held by %s: %.1f ms of every %.1f ms frame (%.0f%%)"', '"no single owner; the largest is %s at %.0f%% of a %.1f ms frame"',
                'constexpr double kSlowFraction = 0.40;', 'constexpr unsigned kSlowHoldSeconds = 5;', 'constexpr unsigned kSlowStillSlowSeconds = 30;',
                'constexpr double kSlowOwnerShare = 0.35;', '"vendor_end_frame"', '"vendor_wait_frame"', '"vendor_swapchain"', '"edvr_copy"', '"edvr_work"',
                '"the vendor runtime\'s xrEndFrame"', '"the game\'s own frame"', '"recovered"', '"session_close"', '"still_slow"')),
            (("src", "openxr", "end_frame_episodes.h"), (
                '"native_end_frame_episode,episode=%llu,sequence=%llu,ms=%.4f,periods=%.2f,period_ms=%.4f,should_render=%u,layers=%u,path=%s,pacing=%s,"',
                '"result=%d,xr_acquire_ms=%.4f,xr_wait_ms=%.4f,xr_draw_ms=%.4f,xr_release_ms=%.4f,copy_ms=%.4f,session_state=%s,state_age_ms=%.1f,"',
                '"native_end_frame_episode_end,episode=%llu,reason=%s,start_line=%u,calls=%llu,slow_calls=%llu,duration_ms=%.1f,p50_ms=%.2f,max_ms=%.4f,"',
                '"mean_ms=%.4f,period_ms=%.4f,max_periods=%.2f,first_sequence=%llu,last_sequence=%llu,units=wall_ms"',
                '"native_end_frame_episodes_summary,%s%s%sthreshold_periods=%.1f,episodes=%llu,start_lines=%llu,end_lines=%llu,calls=%llu,slow_calls=%llu,"',
                '"native_end_frame_episodes,armed=1,threshold_periods=%.1f,period=last_real_predicted_display_period,end_after_normal_calls=%u,"',
                'constexpr double kEndFramePeriods = 3.0;', '"loading"', '"overlapped"', '"synchronous"', '"deferred"', '"runtime"')),
            (("src", "openxr", "vendor_events.h"), (
                '"native_xr_event,n=%llu,type=%s%s"', '",from=%s,to=%s,event_time=%lld,lag_ms=%s"', '",lost_count=%u"',
                '"native_xr_event,n=%llu,type=unknown,number=%d,first_of_type=1"', '"native_xr_event,n=%llu,suppressed=1,limit=%u,',
                '"native_xr_events_summary,%s%s%sreceived=%llu,logged=%llu,suppressed=%llu,session_state_changed=%llu,events_lost=%llu,lost_events=%llu,"',
                '"instance_loss_pending=%llu,reference_space_change_pending=%llu,interaction_profile_changed=%llu,visibility_mask_changed=%llu,"',
                '"display_refresh_rate_changed=%llu,perf_settings=%llu,unknown_events=%llu,unknown_types=%llu,last_state=%s"',
                '"native_xr_events,armed=1,source=xrPollEvent,line_cap=%u,unknown_types_named=%u,decoded=session_state_changed|events_lost|"',
                '",loss_time=%lld"', '"events_lost"', '"instance_loss_pending"', '"session_state_changed"')),
            (("src", "common", "vram_watch.h"), (
                '"%slocal_used_mb=%s%c%slocal_budget_mb=%s%c%slocal_pct=%s%c%snonlocal_used_mb=%s%c%snonlocal_budget_mb=%s%c%snonlocal_pct=%s"',
                '"vram: reason=%s %s"', '"vram: reason=armed %s; adapter %s; this process\'s graphics memory against the budget the OS gives it "',
                '"vram: unavailable -- %s. No vram: line will be written this session."',
                '"vram: QueryVideoMemoryInfo failed for the local segment group (hr 0x%08lX); no line is written until "',
                '"over_budget"', '"back_under_budget"', '"pressure"', '"periodic"')),
            (("src", "d3d11", "perf_monitor.cpp"), (
                '"slow test: advanced.slow_test_ms = %d. %llu s from now the runtime holds every xrEndFrame it makes %d ms longer, "',
                '"for %llu s, to test the end-frame episode and slow-regime lines. Set it back to 0."',
                '"slow test: the hold begins now: every xrEndFrame is held %u ms longer for %llu s."', '"slow test: the hold ended. The runtime\'s frames are as they were."')),
            (("src", "openxr", "native_runtime_host.h"), (
                '"native_end_frame_hold,test=1,state=began,ms=%u,source=advanced.slow_test_ms,inside=timed_xrEndFrame_region\\n"',
                '"native_end_frame_hold,test=1,state=ended,held_calls=%llu\\n"')),
            (("src", "d3d11", "vscreen.cpp"), ('"vScreen totals: panel distance applied %llu time(s), void cleared to black "',)),
            (("src", "d3d11", "perf_monitor.cpp"), ('This is frame %llu', 'runtime sequence %llu')),
            (("src", "openxr", "producer_gpu_timing.cpp"), ('"native_producer_gpu,window=%llu,samples=%u,pending_dropped=%u,copy=%.4f/%.4f/%.4f/%.4f,units=gpu_ms\\n"',))):
        try:
            body = read_text(os.path.join(root, *rel))
        except OSError:
            fail("%s is not where the self-test looks for it" % "\\".join(rel))
            continue
        for needle in needles:
            if needle not in body:
                fail("%s no longer has %r, which --freezes reads; change this reader and its pin together" % ("\\".join(rel), needle))

    VERSION = "0.18.1-4-gabcdef1"
    ZONE_H = 1                                  # local is UTC+1: the runtime log's file name (local) against its first line (UTC) says so
    T0 = 10 * 3600 + 50 * 60 + 12.0             # both logs open at 10:50:12 local
    RS = 11 * 3600 + 14 * 60 + 38.1             # the regime begins at 11:14:38.1
    CLOSE = 11 * 3600 + 20 * 60                 # the session closes at 11:20:00
    GFX_NAME = "edvr_gfx_20261002_105012.log"
    RT_NAME = "edvr_openxr_20261002_105014_044_12912.log"

    def hms(t):
        ms = int(round(t * 1000.0))
        h, rest = divmod(ms, 3600000)
        m, rest = divmod(rest, 60000)
        s, milli = divmod(rest, 1000)
        return "%02d:%02d:%02d.%03d" % (h, m, s, milli)

    def g(t, msg):
        return "[%s] %s" % (hms(t), msg)

    def r(t, msg):
        return "2026-10-02 %s UTC pid=12912 tid=6364 %s" % (hms(t - ZONE_H * 3600), msg)

    # ---- the graphics log's lines ----
    def vram(reason, lu=7421, lb=10863, nu=316, nb=16311, tail=""):
        return ("vram: reason=%s local_used_mb=%d local_budget_mb=%d local_pct=%.1f nonlocal_used_mb=%d nonlocal_budget_mb=%d nonlocal_pct=%.1f%s"
                % (reason, lu, lb, 100.0 * lu / lb, nu, nb, 100.0 * nu / nb, tail))

    vram_armed = vram("armed", tail="; adapter NVIDIA GeForce RTX 4090; this process's graphics memory against the budget the OS gives it (DXGI QueryVideoMemoryInfo, local = "
                                    "the card's own memory, non-local = system memory it uses), sampled once a second; a line every 30 s, every 5 s from 90% of the local "
                                    "budget, and one at each crossing of the budget.")
    vram_none = "vram: unavailable -- this device has no IDXGIAdapter3 (DXVK or Wine without it). No vram: line will be written this session."

    def long_frame(ms, seq, frame, period=13.9):
        return ("monitor: LONG FRAME -- %.1f ms between Presents (runtime predicted period %.1f ms), no WaitGetPoses, CPU busy, compositor, reprojection, or door samples; "
                "game creations: 0 textures, 0 buffers, 0 shaders (0.0 MB); EDVR events: none. This is frame %d; the flip timeline is not armed, so there are no table "
                "changes to order against it. runtime sequence %d, game work 4.92 ms." % (ms, period, frame, seq))

    def freeze(ms, frame, seq, n):
        if seq:
            c = "%.1f ms (%.1f ms from the pose wait's return to this Present, the previous cycle 330.6 ms)" % (ms, ms - 240.2)
        else:
            c = "unavailable (no two pose-wait returns: not the native path, or no open runtime session)"
        return ("monitor: FREEZE -- %.1f ms between Presents, ended now: frame %d, runtime sequence %d; runtime cycle %s; freeze %d of this session; stall sampler took 1 "
                "sample, the last in EliteDangerous64.exe+0x5d6d7f. A frame of 250 ms or more always gets this line and a LONG FRAME line, with no cap and no rate limit."
                % (ms, frame, seq, c, n))

    def vscreen(frames, ms):
        return ("vScreen totals: panel distance applied 51877 time(s), void cleared to black 51884 time(s) (2-2 per frame over the last %d frames), largest eye-draw count 2 "
                "this window and 7041 this session. %d frames in %d ms is %d fps. Two eyes a frame, so these should climb steadily." % (frames, frames, ms, round(frames * 1000.0 / ms)))

    def counts_line():
        return ("monitor: long frame counts reason=session_close candidates=3 long=3 blips=0 written=3 unwritten=0 over_250ms=0 over_250ms_unwritten=0 long_lt50=0 "
                "long_50_100=0 long_100_250=3 long_250_1000=0 long_ge1000=0 blip_lt50=0 blip_50_100=0 blip_100_250=0 blip_250_1000=0 blip_ge1000=0 unwritten_lt50=0 "
                "unwritten_50_100=0 unwritten_100_250=0; candidates are Present gaps over twice the runtime's predicted period; a candidate the runtime's cycle did not "
                "confirm is a blip, counted and not written.")

    gopen = [g(T0, "EDVR log -- unofficial VR fixes for Elite Dangerous: Odyssey"),
             g(T0 + 0.002, "version %s (build 68C0A1F2) -- this DLL was linked 2026-10-02 08:00:00 UTC" % VERSION)]
    # The runtime's own counts at its close, so a fixture's INSTRUMENT row has what a closed session writes and the verdict is not a WARN for it.
    rt_counts = ("native_long_cycle_summary,count=3,logged=3,threshold=2x_period,candidates=3,long=3,written=3,unwritten=0,over_250ms=0,over_250ms_unwritten=0,long_lt50=0,"
                 "long_50_100=0,long_100_250=3,long_250_1000=0,long_ge1000=0,unwritten_lt50=0,unwritten_50_100=0,unwritten_100_250=0")

    # ---- the runtime log's lines ----
    def cycle_line(seq, ms, period=13.8889):
        return ("native_long_cycle,sequence=%d,cycle_ms=%.4f,period_ms=%.4f,game_before_first_submit=2.4360,first_submit_roundtrip=0.8010,first_submit_owner_body=0.7730,"
                "between_eye_calls=0.0010,second_submit_roundtrip=%.4f,second_submit_owner_body=%.4f,first_submit_render_park=0.7908,second_submit_render_park=%.4f,"
                "post_second_submit_to_next_wait=1.1120,present_split=ok,pre_present=0.1140,present_hook=0.1670,hook_before_real=0.0010,hook_real_present=0.0820,"
                "hook_after_real=0.0750,hook_render_callback=0.0090,post_present=0.5760,next_wait_roundtrip=0.0760,next_wait_owner_body=0.0450,units=wall_ms"
                % (seq, ms, period, ms - 5.0, ms - 5.1, ms - 5.1))

    def phases_line(window, first, last, end_p50, pacer_p50=0.0001, wait_p50=0.0, dispatch_p50=0.158):
        return ("native_submit_phases,window=%d,first=%d,last=%d,output=2325x2392/2325x2392,treatments=6/6,feature_epoch=8,trim_stage=3,trim_factors=0.90000/0.95000,pacing=1,"
                "separate=1,producer_dispatch=%.4f/0.2701/0.3418/0.3961,producer_acquire=0.0765/0.1489/0.1674/0.1714,producer_flush=0.0330/0.1032/0.1267/0.1953,"
                "consumer_acquire=0.0636/0.1304/0.1787/0.2020,consumer_flush=0.0281/0.0999/0.1065/0.1209,receive=0.1266/0.1833/0.2516/0.2953,xr_acquire=0.0014/0.0018/0.0021/0.0131,"
                "xr_wait=0.0003/0.0004/0.0006/0.0006,xr_draw_submit=0.0794/0.1203/0.1579/0.1726,xr_release=0.0007/0.0010/0.0012/0.0014,"
                "xr_end_frame=%.4f/%.4f/%.4f/%.4f,wait_frame=%.4f/0.0000/0.0000/0.0001,pacer_block=%.4f/0.0002/0.0005/0.0006,percentiles=50/95/99/max,units=wall_ms,nested=1,gpu=0,"
                "frame_end_overlap=0" % (window, first, last, dispatch_p50, end_p50, end_p50 + 1.2, end_p50 + 1.7, end_p50 + 2.0, wait_p50, pacer_p50))

    def producer_gpu(window, p50, p95, p99, mx):
        return "native_producer_gpu,window=%d,samples=512,pending_dropped=0,copy=%.4f/%.4f/%.4f/%.4f,units=gpu_ms" % (window, p50, p95, p99, mx)

    rt_head = [r(T0 + 2.4, "module_init,version=%s,durable_log=1" % VERSION), r(T0 + 2.4, "display_frequency,hz=72,source=runtime,extension_enabled=1,result=0,reason=session_created"),
               r(T0 + 2.6, "native_perf_settings,extension=absent,cpu=n/a,gpu=n/a")]

    # ---- the arc's runtime lines ----
    xr_armed = ("native_xr_events,armed=1,source=xrPollEvent,line_cap=200,unknown_types_named=24,decoded=session_state_changed|events_lost|instance_loss_pending|"
                "reference_space_change_pending|interaction_profile_changed|visibility_mask_changed|display_refresh_rate_changed|perf_settings,undecoded=named_once_by_number")
    ep_armed = ("native_end_frame_episodes,armed=1,threshold_periods=3.0,period=last_real_predicted_display_period,end_after_normal_calls=8,start_lines=1_per_2000_ms_max_100,"
                "long_episode_end_line_from_slow_calls=20,paths=synchronous|overlapped|loading,units=wall_ms")

    def slow_armed(vram_state="available"):
        return ("native_slow_regime,armed=1,threshold_fraction=0.40,of=display_rate,hold_s=5,end_s=2,still_slow_s=30,owner_share=0.35,frames=xrEndFrame_returns,"
                "owners=vendor_end_frame|vendor_wait_frame|vendor_swapchain|edvr_copy|edvr_work|game,vram=%s" % vram_state)

    def xr_event(n, kind, body=""):
        return "native_xr_event,n=%d,type=%s%s" % (n, kind, body)

    def state_event(n, frm, to, lag="2.500"):
        return xr_event(n, "session_state_changed", ",from=%s,to=%s,event_time=1234567890123,lag_ms=%s" % (frm, to, lag))

    def xr_summary(reason, received=5, logged=5, last="FOCUSED", **extra):
        v = dict(suppressed=0, session_state_changed=received, events_lost=0, lost_events=0, instance_loss_pending=0, reference_space_change_pending=0,
                 interaction_profile_changed=0, visibility_mask_changed=0, display_refresh_rate_changed=0, perf_settings=0, unknown_events=0, unknown_types=0)
        v.update(extra)
        return ("native_xr_events_summary,reason=%s,received=%d,logged=%d,suppressed=%d,session_state_changed=%d,events_lost=%d,lost_events=%d,instance_loss_pending=%d,"
                "reference_space_change_pending=%d,interaction_profile_changed=%d,visibility_mask_changed=%d,display_refresh_rate_changed=%d,perf_settings=%d,unknown_events=%d,"
                "unknown_types=%d,last_state=%s" % (reason, received, logged, v["suppressed"], v["session_state_changed"], v["events_lost"], v["lost_events"],
                                                    v["instance_loss_pending"], v["reference_space_change_pending"], v["interaction_profile_changed"],
                                                    v["visibility_mask_changed"], v["display_refresh_rate_changed"], v["perf_settings"], v["unknown_events"], v["unknown_types"], last))

    def episode(n, seq, ms, path="synchronous", pacing="runtime", state="FOCUSED", period=13.8889):
        return ("native_end_frame_episode,episode=%d,sequence=%d,ms=%.4f,periods=%.2f,period_ms=%.4f,should_render=1,layers=1,path=%s,pacing=%s,result=0,xr_acquire_ms=0.0016,"
                "xr_wait_ms=0.0002,xr_draw_ms=0.0385,xr_release_ms=0.0005,copy_ms=0.3120,session_state=%s,state_age_ms=1204.0,units=wall_ms"
                % (n, seq, ms, ms / period, period, path, pacing, state))

    def episode_end(n, calls, slow, duration_ms, p50, mx, first, last, reason="normal_calls", start_line=1, period=13.8889):
        return ("native_end_frame_episode_end,episode=%d,reason=%s,start_line=%d,calls=%d,slow_calls=%d,duration_ms=%.1f,p50_ms=%.2f,max_ms=%.4f,mean_ms=%.4f,period_ms=%.4f,"
                "max_periods=%.2f,first_sequence=%d,last_sequence=%d,units=wall_ms" % (n, reason, start_line, calls, slow, duration_ms, p50, mx, p50 + 0.4, period, mx / period, first, last))

    def ep_summary(reason, episodes=1, start_lines=1, end_lines=1, calls=900, slow_calls=567, longest=84.7388, open_=0):
        return ("native_end_frame_episodes_summary,reason=%s,threshold_periods=3.0,episodes=%d,start_lines=%d,end_lines=%d,calls=%d,slow_calls=%d,longest_ms=%.4f,open=%d,units=wall_ms"
                % (reason, episodes, start_lines, end_lines, calls, slow_calls, longest, open_))

    VRAM_FIG = "vram_local_used_mb=7421,vram_local_budget_mb=10863,vram_local_pct=68.3,vram_nonlocal_used_mb=316,vram_nonlocal_budget_mb=16311,vram_nonlocal_pct=1.9"

    def slow(event, regime, duration, window, frames, owner="vendor_end_frame", share=0.831, fps=9.97, hz=72.0, frame_ms=100.3, end_ms=83.10, wait_ms=0.0, swap_ms=0.04,
             copy_ms=0.40, work_ms=0.30, game_ms=16.16, mx=84.74, figures=VRAM_FIG, summary=None, reason=None, context="scene", loading=0, empty=0, with_context=True):
        """One native_slow_regime line. context/loading/empty are the `context=`, `loading_frames=` and `empty_frames=` fields (slow_regime.h), which a build from
        before them does not write: with_context=False leaves them out."""
        if summary is None:
            summary = {"vendor_end_frame": "held by the vendor runtime's xrEndFrame: %.1f ms of every %.1f ms frame (%.0f%%)" % (end_ms, frame_ms, 100 * share),
                       "game": "held by the game's own frame: %.1f ms of every %.1f ms frame (%.0f%%)" % (game_ms, frame_ms, 100 * share),
                       "edvr_copy": "held by EDVR's copy of the eyes: %.1f ms of every %.1f ms frame (%.0f%%)" % (copy_ms, frame_ms, 100 * share),
                       "none": "no single owner; the largest is the game's own frame at 31%% of a %.1f ms frame" % frame_ms,
                       "no_frames": "held by nothing: no frame reached xrEndFrame in this window"}[owner]
        ctx = "context=%s,loading_frames=%d,empty_frames=%d," % (context, loading, empty) if with_context else ""
        return ("native_slow_regime,event=%s,%sregime=%d,duration_s=%.1f,window_s=%.1f,frames=%d,fps=%.2f,display_hz=%.2f,fraction=%.3f,threshold=0.40,frame_ms=%.2f,%sheld_by=%s,"
                "held_share=%.3f,vendor_share=%.3f,vendor_end_frame_ms=%.2f,vendor_wait_frame_ms=%.2f,vendor_swapchain_ms=%.2f,edvr_copy_ms=%.2f,edvr_work_ms=%.2f,game_ms=%.2f,"
                "end_frame_max_ms=%.2f,%s,units=wall_ms,summary=%s" % (event, "reason=%s," % reason if reason else "", regime, duration, window, frames, fps, hz, fps / hz, frame_ms, ctx,
                                                                       owner, share, (end_ms + wait_ms + swap_ms) / frame_ms, end_ms, wait_ms, swap_ms, copy_ms, work_ms, game_ms, mx,
                                                                       figures, summary))

    def slow_summary(reason, regimes=1, open_=0, frames=82000, seconds=330, slow_seconds=57):
        return ("native_slow_regime_summary,reason=%s,regimes=%d,open=%d,frames=%d,seconds=%d,slow_seconds=%d,threshold=0.40" % (reason, regimes, open_, frames, seconds, slow_seconds))

    def native_flight(owner="vendor_end_frame", end=True, vram_mode="available", arms=("xr", "ep", "slow"), regimes=1, events=True, episodes=True, summary_regimes=None,
                      extra_rt=(), extra_gfx=(), still=True, share=0.831, crash=False, end_ms=83.10, context="scene", loading=0, empty=0, with_context=True, summary=None):
        """A new-build flight: the three armed lines, the vendor's session events, one regime from RS for 57.1 s with its SLOW, still_slow and end lines (and the
        episode instrument's start and end line), the VRAM watch's lines, the summaries at the close. crash=True is a session that stopped inside the regime:
        no end lines and no close. end_ms is the mean xrEndFrame the regime's lines carry; context, loading and empty the `context=`, `loading_frames=` and
        `empty_frames=` fields of its lines (with_context=False: a build from before them). Returns (graphics lines, runtime lines)."""
        ctx = dict(context=context, loading=loading, empty=empty, with_context=with_context, summary=summary)
        fig = VRAM_FIG if vram_mode == "available" else "vram=unavailable"
        end = end and not crash
        gfx = list(gopen)
        gfx.append(g(T0 + 3.0, vram_armed if vram_mode == "available" else vram_none))
        if vram_mode == "available":
            gfx += [g(T0 + 33.0 + 30.0 * i, vram("periodic", lu=7400 + 3 * i)) for i in range(8)]
            gfx.append(g(RS + 5.0, vram("periodic", lu=7421)))
        gfx += list(extra_gfx)
        if not crash:
            gfx.append(g(CLOSE, counts_line()))
        rt = list(rt_head)
        if "xr" in arms:
            rt.append(r(T0 + 2.7, xr_armed))
        if "ep" in arms:
            rt.append(r(T0 + 2.7, ep_armed))
        if "slow" in arms:
            rt.append(r(T0 + 2.7, slow_armed("available" if vram_mode == "available" else "unavailable: this device has no IDXGIAdapter3")))
        if events:
            rt += [r(T0 + 3.1, state_event(1, "UNKNOWN", "IDLE")), r(T0 + 3.2, state_event(2, "IDLE", "READY")), r(T0 + 3.3, state_event(3, "READY", "SYNCHRONIZED")),
                   r(T0 + 3.4, state_event(4, "SYNCHRONIZED", "VISIBLE")), r(T0 + 3.5, state_event(5, "VISIBLE", "FOCUSED"))]
        if regimes:
            if events:
                rt.append(r(RS + 1.0, state_event(6, "FOCUSED", "VISIBLE", "1.250")))
            if episodes:
                rt.append(r(RS + 0.2, episode(1, 96600, 83.1)))
            rt.append(r(RS + 5.0, slow("SLOW", 1, 5.0, 5.0, 50, owner=owner, share=share, figures=fig, end_ms=end_ms, **ctx)))
            if still:
                rt.append(r(RS + 35.0, slow("still_slow", 1, 35.0, 30.0, 298, owner=owner, share=share, figures=fig, end_ms=end_ms, **ctx)))
            if end:
                rt.append(r(RS + 59.1, slow("end", 1, 57.1, 57.1, 567, owner=owner, share=share, figures=fig, reason="recovered", end_ms=end_ms, **ctx)))
                if episodes:
                    rt.append(r(RS + 57.3, episode_end(1, 575, 567, 57000.0, 83.5, 84.7388, 96600, 97175)))
        rt += list(extra_rt)
        if not crash:
            rt.append(r(CLOSE, rt_counts))
            if episodes or "ep" in arms:
                rt.append(r(CLOSE, ep_summary("session_close", episodes=1 if regimes else 0, start_lines=1 if (regimes and episodes) else 0,
                                              end_lines=1 if (regimes and episodes and end) else 0)))
            if events or "xr" in arms:
                rt.append(r(CLOSE, xr_summary("session_close", received=6 if (regimes and events) else 5 if events else 0, logged=6 if (regimes and events) else 5 if events else 0)))
            if "slow" in arms:
                n = regimes if summary_regimes is None else summary_regimes
                rt.append(r(CLOSE, slow_summary("session_close", regimes=n, open_=0, slow_seconds=57 if regimes else 0)))
        return gfx, rt

    # ---- the v0.18.0 form: the older lines only, modelled on the user's boarding-freeze flight ----
    def seq_at(t):
        return 96600 + int(round((t - RS) * 9.93))

    def legacy_flight(runtime=True, hitches=False, marks_end=57.138, gfx_fps=None):
        """A flight from before the arc: LONG FRAME lines of 100 ms every 5 s (the limiter), the runtime's native_long_cycle lines four a second until its cap, the
        vScreen windows, a FREEZE at the end of the regime and an unrelated one 6.2 s later; or, with hitches, long frames now and then at a steady 72 fps.
        gfx_fps makes the graphics frame counter rise that fast instead of with the runtime's sequence (a game Presenting at hundreds of frames a second in a
        menu while the runtime's frames crawl)."""
        gfx = list(gopen)
        rt = list(rt_head)

        def frame_at(t):
            return seq_at(t) + 5832 if gfx_fps is None else 102431 + int(round((t - RS) * gfx_fps))
        if hitches:
            for k in range(13):
                t = RS + 5.0 * k
                seq = 96600 + int(round((t - RS) * 72.0))
                gfx.append(g(t, long_frame(61.0, seq, seq + 5832)))
                rt.append(r(t, cycle_line(seq, 61.0)))
            for k in range(3):
                t = RS - 20.0 + 20.0 * k
                gfx.append(g(t + 0.1, vscreen(1440, 20000)))
        else:
            for k in range(11):
                t = RS + 5.0 * k
                gfx.append(g(t, long_frame(100.2, seq_at(t), frame_at(t))))
            t = 0.0
            while RS + t < RS + 35.5:
                for j in range(4):
                    s = seq_at(RS + t) + j
                    rt.append(r(RS + t + 0.3 * j, cycle_line(s, 100.2)))
                t += 1.0
            gfx += [g(RS + 14.552, vscreen(519, 20094)), g(RS + 34.614, vscreen(200, 20062)), g(RS + 54.677, vscreen(200, 20063))]
            end_t = RS + marks_end
            end_frame = 102999 if gfx_fps is None else frame_at(end_t)
            gfx += [g(end_t, long_frame(254.6, 97167, end_frame)), g(end_t, freeze(254.6, end_frame, 97167, 14)),
                    g(end_t + 6.2, long_frame(539.8, 0, 103057)), g(end_t + 6.2, freeze(539.8, 103057, 0, 15))]
            rt += [r(RS + 38.7, phases_line(49, 95547, 95802, 1.6682 / 100.0)), r(RS + 38.7, phases_line(50, 96730, 96985, 82.7130)),
                   r(RS - 18.1, producer_gpu(48, 0.0367, 0.0408, 0.3625, 0.5921)), r(RS + 41.9, producer_gpu(49, 0.0362, 0.0409, 0.2263, 0.3707)),
                   r(RS + 42.0, producer_gpu(50, 0.1741, 75.6131, 78.1940, 79.4299))]
        gfx.append(g(CLOSE, counts_line()))
        rt.append(r(CLOSE, rt_counts))
        return gfx, (rt if runtime else None)

    tmp = tempfile.mkdtemp(prefix="edvr_slow_")
    serial = [0]
    violations = []
    real_open = builtins.open

    @contextlib.contextmanager
    def read_only():
        names = ("makedirs", "mkdir", "remove", "unlink", "rename", "replace", "rmdir")
        saved = {n: getattr(os, n) for n in names}

        def guarded_open(file, mode="r", *a, **k):
            if any(c in str(mode) for c in "wax+"):
                violations.append("open(%r, %r)" % (file, mode))
            return real_open(file, mode, *a, **k)

        def tripwire(name):
            def hit(*a, **k):
                violations.append("os.%s%r" % (name, a))
                raise OSError("--freezes must not write")
            return hit
        builtins.open = guarded_open
        for n in names:
            setattr(os, n, tripwire(n))
        try:
            yield
        finally:
            builtins.open = real_open
            for n, fn in saved.items():
                setattr(os, n, fn)

    def put(path, lines):
        with real_open(path, "wb") as f:
            f.write(("\n".join(lines) + "\n").encode("utf-8"))

    def run(flight, *extra):
        """Write the flight's logs (a graphics log and, when it has one, a runtime log) to a directory of their own and run --freezes through main(); (exit code, output)."""
        gfx, rt = flight
        serial[0] += 1
        d = os.path.join(tmp, "f%02d" % serial[0])
        os.makedirs(d)
        gp = os.path.join(d, GFX_NAME)
        put(gp, gfx)
        if rt is not None:
            put(os.path.join(d, RT_NAME), rt)
        buf = io.StringIO()
        with read_only(), contextlib.redirect_stdout(buf):
            rc = main(["--file", gp, "--freezes"] + list(extra))
        return rc, buf.getvalue()

    def rx(pattern):
        return re.compile(pattern, re.M)

    def seen(item, out):
        return item.search(out) is not None if hasattr(item, "search") else item in out

    def case(what, result, rc_want, verdict, *has, absent=()):
        rc, out = result
        problems = []
        if rc != rc_want:
            problems.append("exit %d, wanted %d" % (rc, rc_want))
        if verdict and ("freezes verdict: %s (" % verdict) not in out:
            problems.append("the verdict is not %s" % verdict)
        problems += ["lacks %r" % getattr(h, "pattern", h) for h in has if not seen(h, out)]
        problems += ["has %r" % getattr(a, "pattern", a) for a in absent if seen(a, out)]
        if problems:
            fail("%s: %s:\n%s" % (what, "; ".join(problems), out))

    never_pass = ("freezes verdict: PASS", "freezes verdict: WARN")

    try:
        # ---- the parser, on the exact lines ----
        gfx, rt = native_flight()
        fz = parse_freezes(os.path.join(tmp, GFX_NAME), "\n".join(gfx) + "\n", os.path.join(tmp, RT_NAME), "\n".join(rt) + "\n")
        if fz["unparsed"]["count"]:
            fail("exact lines were left unparsed: %r" % fz["unparsed"])
        if (fz["xr_armed"] is None or fz["ep_armed"] is None or fz["slow_armed"] is None or fz["vram_state"] is None or fz["vram_state"]["state"] != "armed"
                or fz["vram_state"]["adapter"] != "NVIDIA GeForce RTX 4090"):
            fail("the armed lines read as %r" % ({k: fz[k] for k in ("xr_armed", "ep_armed", "slow_armed", "vram_state")},))
        if [e["type"] for e in fz["xr_events"]] != ["session_state_changed"] * 6 or fz["xr_events"][5]["f"]["to"] != "VISIBLE":
            fail("the vendor events read as %r" % fz["xr_events"])
        if len(fz["episodes"]) != 1 or fz["episodes"][0]["ms"] != 83.1 or fz["episodes"][0]["seq"] != 96600 or len(fz["episode_ends"]) != 1 or fz["episode_ends"][0]["slow"] != 567:
            fail("the episode lines read as %r / %r" % (fz["episodes"], fz["episode_ends"]))
        evs = [l["event"] for l in fz["slow_lines"]]
        if evs != ["SLOW", "still_slow", "end"] or fz["slow_lines"][0]["vram"]["local_used_mb"] != 7421.0 or fz["slow_lines"][0]["owner"] != "vendor_end_frame" \
                or fz["slow_lines"][2]["summary"] != "held by the vendor runtime's xrEndFrame: 83.1 ms of every 100.3 ms frame (83%)":
            fail("the slow-regime lines read as %r" % fz["slow_lines"])
        if len(fz["vram"]) != 10 or fz["vram"][0]["reason"] != "armed" or fz["vram"][1]["kv"]["local_pct"] is None:
            fail("the vram lines read as %r" % fz["vram"][:2])
        if fz["display_hz"] != 72.0:
            fail("display_frequency reads as %r" % fz["display_hz"])
        regimes = _fz_slow_regimes(fz, True)
        day = (datetime.date(2026, 10, 2) - EPOCH.date()).days * DAY
        if len(regimes) != 1 or abs(regimes[0]["start"] - (day + RS)) > 1e-3 or abs(regimes[0]["end"] - (day + RS + 57.1)) > 1e-3 or regimes[0]["owner"] != "vendor_end_frame":
            fail("the native regime reads as %r" % (regimes,))

        # ---- a new-build flight with a slow regime: SLOW, never PASS, the owner named ----
        n1 = run(native_flight(), "--expect-build", VERSION)
        case("a new-build flight with a regime held by the vendor's xrEndFrame", n1, 4, "SLOW",
             "SLOW REGIME (1 found: the frame rate under 40% of the display's for 5 s or more; a state the session was in, not one frame; SLOW is set only by a regime "
             "the vendor runtime or EDVR holds, a load the game's frame holds is INFO, a slow stretch of its scenes or no named owner a WARN):",
             "  #1  11:14:38.100 to 11:15:35.200 local  (57.1 s, ended: recovered)  [SLOW] the build's own detector",
             "rate: 9.97 fps against the display's 72.00 Hz (13.8% of it; a regime is under 40%), frames of 100.3 ms",
             "owner: held by the vendor runtime's xrEndFrame: 83.1 ms of every 100.3 ms frame (83%)",
             "context: scene (0 loading frame(s) and 0 end(s) with no layers of 567 frames)",
             "a frame went (mean ms over the whole regime): xrEndFrame 83.10 (longest 84.74), pose wait 0.00, swapchain calls 0.04, EDVR copy 0.40, EDVR work 0.30, the game 16.16",
             "graphics memory at the SLOW line (11:14:43.100): local 7421 of 10863 MB (68.3%), non-local 316 of 16311 MB (1.9%)",
             "lines: SLOW 11:14:43.100; still_slow 11:15:13.100; end 11:15:37.200",
             "vendor events within 10 s of it (1): 11:14:39.100 session_state_changed FOCUSED -> VISIBLE (read 1.250 ms after the runtime stamped it)",
             "END-FRAME EPISODES (a call into the vendor's xrEndFrame of 3 display periods or more",
             rx(r"^  1   11:14:38\.300 +96600 +83\.1 +5\.98  synchronous runtime  FOCUSED +0\.0016/0\.0002/0\.0385/0\.0005, 0\.3120$"),
             "  episode 1 over at 11:15:35.400 (normal_calls): 575 call(s), 567 of them slow, 57.0 s, p50 83.5 ms, max 84.7 ms (6.10 periods), sequences 96600-97175",
             "VENDOR EVENTS (xrPollEvent: what the vendor's runtime told the session):",
             "last session state FOCUSED", "11:14:39.100  #6  session_state_changed FOCUSED -> VISIBLE",
             "VRAM (this process's graphics memory against the budget the OS gives it",
             "  armed at 10:50:15.000 on adapter NVIDIA GeForce RTX 4090",
             "  SLOW  SLOW REGIME: 11:14:38.100 to 11:15:35.200 local (57.1 s), 9.97 fps against the display's 72 Hz (14%), held by the vendor runtime's xrEndFrame: 83.1 ms of every 100.3 ms frame (83%)",
             "  note  INSTRUMENT: the slow-regime detector (native_slow_regime) armed at 10:50:14.700",
             "  note  INSTRUMENT: the graphics-memory watch armed at 10:50:15.000 on adapter NVIDIA GeForce RTX 4090",
             "  note  END-FRAME EPISODES: 1 start line(s) and 1 end line(s); the longest xrEndFrame call 83.1 ms (6.0 display periods, sequence 96600, at 11:14:38.300)",
             "  PASS  INSTRUMENT: a `monitor: long frame counts` line is in the graphics log",
             "freezes verdict: SLOW (0 STOP, 0 WARN, 1 SLOW): 11:14:38.100 to 11:15:35.200 local (57 s) at 10.0 fps of the display's 72 Hz, held by the vendor runtime's xrEndFrame: 83.1 ms of every 100.3 ms frame (83%). STOP:",
             absent=never_pass + ("  PASS  SLOW REGIME", "reconstructed", "RECONSTRUCTED", "DELIBERATE"))
        # The slow-regime finding is the first of the finding rows, and the SLOW rows are counted in the verdict.
        rows = [l for l in n1[1].splitlines() if l[:6] in ("  SLOW", "  note", "  PASS", "  WARN", "  STOP")]
        if not rows or not rows[0].startswith("  SLOW  SLOW REGIME:"):
            fail("the SLOW REGIME row should come first among the finding rows: %r" % rows[:2])
        case("the same flight, a session that stopped inside the regime", run(native_flight(crash=True)), 4, "SLOW",
             "  #1  11:14:38.100 to 11:15:13.100 local  (35.0 s, still going when the log stopped)  [SLOW] the build's own detector",
             "(still going when the log stopped)", "  note  END-FRAME EPISODES: episode 1 started and has no end line: the session ended inside it",
             absent=never_pass + ("ended: recovered",))
        case("no still_slow line: the regime's own end line carries the whole of it", run(native_flight(still=False)), 4, "SLOW", "lines: SLOW 11:14:43.100; end 11:15:37.200")
        # ---- who holds it decides whether it is a SLOW: the vendor runtime and EDVR set it, the game's own frame and no named owner do not ----
        for owner, sentence in (("edvr_copy", "held by EDVR's copy of the eyes"),
                                ("edvr_work", "held by EDVR's own work in the runtime"), ("vendor_wait_frame", "held by the vendor runtime's xrWaitFrame and xrBeginFrame (the pose wait)"),
                                ("vendor_swapchain", "held by the vendor runtime's swapchain calls (xrAcquire, xrWait, xrReleaseSwapchainImage)")):
            case("a regime EDVR's or the vendor's own calls hold is a SLOW: %s" % owner,
                 run(native_flight(owner=owner, share=0.60, episodes=False, summary="%s: 60.0 ms of every 100.3 ms frame (60%%)" % sentence)), 4, "SLOW",
                 "  #1  11:14:38.100 to 11:15:35.200 local  (57.1 s, ended: recovered)  [SLOW] the build's own detector", "  SLOW  SLOW REGIME: 11:14:38.100 to 11:15:35.200 local (57.1 s)",
                 "%s: 60.0 ms of every 100.3 ms frame (60%%)" % sentence, absent=never_pass + ("not a SLOW",))
        case("an owner that holds under 35% of the frame is not named, and is a WARN, not a SLOW", run(native_flight(owner="none", share=0.31)), 0, "WARN",
             "owner: no single owner; the largest is the game's own frame at 31% of a 100.3 ms frame",
             "  #1  11:14:38.100 to 11:15:35.200 local  (57.1 s, ended: recovered)  [WARN] the build's own detector", "      not a SLOW: no single owner holds 35% of a frame",
             "  WARN  SLOW REGIME: 11:14:38.100 to 11:15:35.200 local (57.1 s), 9.97 fps against the display's 72 Hz (14%), no single owner; the largest is the game's own frame at "
             "31% of a 100.3 ms frame; not a SLOW: no single owner holds 35% of a frame",
             "freezes verdict: WARN (0 STOP, 1 WARN) [+1 slow stretch(es) the game's own frame or no named owner held, listed above: not a SLOW]",
             absent=("  SLOW  ", "freezes verdict: SLOW", "freezes verdict: PASS"))
        case("a regime the game's own frame holds while it submits scenes is a WARN: a GPU- or CPU-bound stretch, not a SLOW",
             run(native_flight(owner="game", share=0.72, episodes=False, context="scene")), 0, "WARN",
             "  #1  11:14:38.100 to 11:15:35.200 local  (57.1 s, ended: recovered)  [WARN] the build's own detector", "context: scene (0 loading frame(s) and 0 end(s) with no layers of 567 frames)",
             "      not a SLOW: the game's own frame while it submitted scenes: a GPU- or CPU-bound stretch of the game",
             "  WARN  SLOW REGIME: 11:14:38.100 to 11:15:35.200 local (57.1 s), 9.97 fps against the display's 72 Hz (14%), held by the game's own frame: 16.2 ms of every 100.3 ms "
             "frame (72%); not a SLOW: the game's own frame while it submitted scenes: a GPU- or CPU-bound stretch of the game",
             "freezes verdict: WARN (0 STOP, 1 WARN) [+1 slow stretch(es) the game's own frame or no named owner held, listed above: not a SLOW]",
             absent=("  SLOW  ", "freezes verdict: SLOW", "freezes verdict: PASS"))
        case("a regime the game's own frame holds during a load is INFO: it sets neither SLOW nor WARN, and the verdict says it was there",
             run(native_flight(owner="game", share=0.72, episodes=False, context="loading", loading=567)), 0, "PASS",
             "  #1  11:14:38.100 to 11:15:35.200 local  (57.1 s, ended: recovered)  [INFO] the build's own detector",
             "context: loading (567 loading frame(s) and 0 end(s) with no layers of 567 frames)",
             "      not a SLOW: the game's own frame during a load (the runtime's frames were its own loading frames)",
             "  note  SLOW REGIME: 11:14:38.100 to 11:15:35.200 local (57.1 s), 9.97 fps against the display's 72 Hz (14%), held by the game's own frame: 16.2 ms of every 100.3 ms "
             "frame (72%); not a SLOW: the game's own frame during a load (the runtime's frames were its own loading frames)",
             "freezes verdict: PASS (0 STOP, 0 WARN) [+1 slow stretch(es) the game's own frame or no named owner held, listed above: not a SLOW]",
             absent=("  SLOW  ", "  WARN  ", "freezes verdict: SLOW", "freezes verdict: WARN"))
        case("the same during ends with no layers: a load too", run(native_flight(owner="game", share=0.72, episodes=False, context="no_layers", empty=567)), 0, "PASS",
             "[INFO] the build's own detector", "context: no_layers (0 loading frame(s) and 567 end(s) with no layers of 567 frames)",
             "      not a SLOW: the game's own frame during a load (the runtime's frames were ends with no layers)", absent=("  SLOW  ", "  WARN  ", "freezes verdict: SLOW"))
        case("a game-owned regime from a build that writes no context is a WARN: nothing says it was a load",
             run(native_flight(owner="game", share=0.72, episodes=False, with_context=False)), 0, "WARN",
             "[WARN] the build's own detector", "context: the line has none (a build from before it)",
             "      not a SLOW: the game's own frame (the line does not say whether it was a load)", absent=("  SLOW  ", "freezes verdict: SLOW"))
        case("no frame reached xrEndFrame: the owner is nothing, and a WARN", run(native_flight(owner="no_frames")), 0, "WARN",
             "owner: held by nothing: no frame reached xrEndFrame in this window", "[WARN] the build's own detector", "      not a SLOW: no frame reached xrEndFrame",
             absent=("  SLOW  ", "freezes verdict: SLOW"))
        # A vendor regime and a load in one session: SLOW, naming the vendor's only, with the load a row and a count of its own.
        both = native_flight(extra_rt=[r(RS + 125.0, slow("SLOW", 2, 5.0, 5.0, 50, owner="game", share=0.72, context="loading", loading=50)),
                                       r(RS + 143.0, slow("end", 2, 15.0, 15.0, 150, owner="game", share=0.72, context="loading", loading=150, reason="recovered"))], summary_regimes=2)
        case("a vendor regime and a load in one session: SLOW names the vendor's regime only, and says the other was there", run(both), 4, "SLOW",
             "SLOW REGIMES (2 found:", "  #1  11:14:38.100 to 11:15:35.200 local  (57.1 s, ended: recovered)  [SLOW] the build's own detector",
             rx(r"^  #2  11:16:38\.100 to 11:16:53\.100 local  \(15\.0 s, ended: recovered\)  \[INFO\] the build's own detector$"),
             "  SLOW  SLOW REGIME: 11:14:38.100 to 11:15:35.200 local (57.1 s), 9.97 fps against the display's 72 Hz (14%), held by the vendor runtime's xrEndFrame",
             "  note  SLOW REGIME: 11:16:38.100 to 11:16:53.100 local (15.0 s), 9.97 fps against the display's 72 Hz (14%), held by the game's own frame",
             "freezes verdict: SLOW (0 STOP, 0 WARN, 1 SLOW): 11:14:38.100 to 11:15:35.200 local (57 s) at 10.0 fps of the display's 72 Hz, held by the vendor runtime's xrEndFrame: "
             "83.1 ms of every 100.3 ms frame (83%) [+1 slow stretch(es) the game's own frame or no named owner held, listed above: not a SLOW]. STOP:",
             absent=never_pass)

        # ---- a new-build flight with no regime: the detector's own PASS ----
        quiet = native_flight(regimes=0, episodes=False)
        case("a new-build flight with no regime", run(quiet), 0, "PASS",
             "  PASS  SLOW REGIME: the slow-regime detector was armed at 10:50:14.700 and wrote no SLOW line; its last summary (11:20:00.000) saw 82000 frame(s) in 330 s, 0 s of them slow",
             "  note  END-FRAME EPISODES: no xrEndFrame call of 3 display periods or more (no start line, 0 end line(s))", "  note  VENDOR EVENTS: 5 event line(s) in the log",
             absent=("SLOW REGIME (", "  SLOW  ", "  WARN  ", "  STOP  "))
        case("a detector that wrote no summary says why", run((quiet[0], [l for l in quiet[1] if "native_slow_regime_summary" not in l])), 0, "PASS",
             "wrote no SLOW line; it wrote no summary, so the session did not close cleanly or ran under five minutes")
        case("a summary that counts two regimes beside one regime's lines is a WARN", run(native_flight(summary_regimes=2)), 4, "SLOW",
             "  WARN  SLOW REGIME: the detector's summary at 11:20:00.000 counts 2 regime(s) but the log has native_slow_regime lines for 1")

        # ---- the instruments' own armed lines ----
        case("a new-build graphics log read alone: the runtime's lines are not looked for, and the report says so", run((native_flight()[0], None)), 0, "WARN",
             "  note  SLOW REGIME: the slow-regime detector writes to the runtime log, which was not read, so its SLOW lines were not looked for",
             "  note  INSTRUMENT: the graphics-memory watch armed at 10:50:15.000 on adapter NVIDIA GeForce RTX 4090", "  WARN  INSTRUMENT: no runtime log was read",
             "VRAM (this process's graphics memory", absent=("  SLOW  ", "END-FRAME EPISODES (", "VENDOR EVENTS (", "this build has no slow-regime detector"))
        case("a runtime log with the slow-regime detector's line but not the episodes'", run(native_flight(arms=("xr", "slow"))), 4, "SLOW",
             "  WARN  INSTRUMENT: the runtime log has no armed line for the end-frame episodes (native_end_frame_episodes) although it has others of this arc")
        case("a graphics log with no vram armed line beside a runtime log with the arc's", run((native_flight()[0][:2] + native_flight()[0][3:], native_flight()[1])), 4, "SLOW",
             "  WARN  INSTRUMENT: the graphics log has no `vram:` armed or unavailable line")
        case("the graphics-memory watch unavailable: a note, and the SLOW lines say so", run(native_flight(vram_mode="unavailable")), 4, "SLOW",
             "  note  INSTRUMENT: the graphics-memory watch is UNAVAILABLE on this machine (this device has no IDXGIAdapter3 (DXVK or Wine without it))",
             "graphics memory: the lines say vram=unavailable", "UNAVAILABLE: this device has no IDXGIAdapter3", absent=("  WARN  INSTRUMENT",))

        # ---- the end-frame episodes against the regime ----
        case("a regime held by xrEndFrame with no episode line in it is a WARN: one instrument contradicts the other", run(native_flight(episodes=False)), 4, "SLOW",
             "  WARN  END-FRAME EPISODES: regime #1 is held by the vendor's xrEndFrame but the episode instrument wrote no start or end line in it")
        case("a regime whose mean xrEndFrame is under 3 periods owes no episode line", run(native_flight(episodes=False, end_ms=25.0, share=0.80)), 4, "SLOW",
             "held by the vendor runtime's xrEndFrame: 25.0 ms of every 100.3 ms frame (80%)", absent=("  WARN  END-FRAME EPISODES",))
        case("a periodic summary older than a later regime is not a mismatch",
             run(native_flight(crash=True, extra_rt=[r(RS - 5.0, slow_summary("periodic", regimes=0, slow_seconds=0))])), 4, "SLOW",
             absent=("  WARN  SLOW REGIME: the detector's summary",))

        # ---- the vendor's events and the graphics memory ----
        lost = native_flight(extra_rt=[r(RS + 2.0, xr_event(7, "events_lost", ",lost_count=3")), r(RS + 3.0, xr_event(8, "instance_loss_pending", ",loss_time=1234567990123"))])
        case("events_lost and instance_loss_pending are WARNs, listed beside the regime", run(lost), 4, "SLOW",
             "  WARN  VENDOR EVENTS: the vendor runtime reported events_lost 1 time(s) (first at 11:14:40.100, lost_count=3)",
             "  WARN  VENDOR EVENTS: the vendor runtime reported instance_loss_pending at 11:14:41.100", "vendor events within 10 s of it (3):", "events_lost (lost_count=3)")
        over = native_flight(extra_gfx=[g(RS + 6.0, vram("over_budget", lu=11000)), g(RS + 20.0, vram("back_under_budget", lu=10100)), g(RS + 21.0, vram("pressure", lu=9900))])
        case("graphics memory over the OS's budget is a WARN, listed with its crossings", run(over), 4, "SLOW",
             "  WARN  VRAM: graphics memory went over the OS's budget 1 time(s), first at 11:14:44.100 (local 11000 of 10863 MB (101.3%), non-local 316 of 16311 MB (1.9%))",
             "  note  VRAM: 1 pressure line(s) (local use at or above 90% of the budget), the highest 91.1% at 11:14:59.100",
             rx(r"^  11:14:44\.100  over_budget +local 11000 of 10863 MB \(101\.3%\)"), rx(r"^  11:14:58\.100  back_under_budget +local 10100 of 10863 MB \(93\.0%\)"))
        case("a quiet VRAM watch", run(quiet), 0, "PASS",
             rx(r"^  note  VRAM: 10 vram line\(s\), none over the budget or at 90% of it; the highest local use 68\.3% at "))
        case("an undecoded event type: the line the runtime writes once by number", run(native_flight(extra_rt=[r(RS + 2.0, "native_xr_event,n=7,type=unknown,number=1000047001,first_of_type=1")])),
             4, "SLOW", "an event of type number 1000047001 that the runtime does not decode (the first of its type)")
        case("the event cap's notice line is read, not counted as unparsed",
             run(native_flight(extra_rt=[r(RS + 2.0, "native_xr_event,n=300,suppressed=1,limit=200,the rest of the decoded events are counted in native_xr_events_summary")])),
             4, "SLOW", "the cap of 200 decoded event lines was reached", absent=("PARSE:",))

        # ---- a line the reader cannot read ----
        case("an arc line in a format this reader never saw", run(native_flight(extra_rt=[r(RS + 2.0, "native_slow_regime,event=BOGUS,regime=1")])), 4, "SLOW",
             "  WARN  PARSE: 1 line(s) open like a freeze-diagnostics line but match none of the formats this reads")

        # ---- the test trigger: advanced.slow_test_ms ----
        TH = T0 + 90.0
        t_armed = ("slow test: advanced.slow_test_ms = 80. 90 s from now the runtime holds every xrEndFrame it makes 80 ms longer, for 40 s, to test the end-frame episode and "
                   "slow-regime lines. Set it back to 0.")
        t_began = "slow test: the hold begins now: every xrEndFrame is held 80 ms longer for 40 s."
        t_ended = "slow test: the hold ended. The runtime's frames are as they were."

        def test_flight(hold=True, episodes=True, regime=True, still=True, end=True, fig=True, ended=True):
            gfx, rt = native_flight(regimes=0, episodes=False)
            gfx = gfx[:-1] + [g(T0 + 2.0, t_armed), g(TH, t_began)] + ([g(TH + 40.0, t_ended)] if ended else []) + gfx[-1:]
            rt = [l for l in rt if "native_xr_events_summary" not in l and "native_slow_regime_summary" not in l and "native_end_frame_episodes_summary" not in l]
            if hold:
                rt.append(r(TH, "native_end_frame_hold,test=1,state=began,ms=80,source=advanced.slow_test_ms,inside=timed_xrEndFrame_region"))
                rt.append(r(TH + 40.0, "native_end_frame_hold,test=1,state=ended,held_calls=392"))
            if episodes:
                rt.append(r(TH + 0.1, episode(1, 51000, 95.1)))
                rt.append(r(TH + 40.2, episode_end(1, 400, 392, 40000.0, 94.8, 96.0, 51000, 51400)))
            if regime:
                f = VRAM_FIG if fig else "vram=unavailable"
                rt.append(r(TH + 5.0, slow("SLOW", 1, 5.0, 5.0, 53, end_ms=94.9, frame_ms=106.4, share=0.892, fps=9.4, figures=f)))
                if still:
                    rt.append(r(TH + 35.0, slow("still_slow", 1, 35.0, 30.0, 282, end_ms=94.9, frame_ms=106.4, share=0.892, fps=9.4, figures=f)))
                if end:
                    rt.append(r(TH + 42.0, slow("end", 1, 40.0, 40.0, 376, end_ms=94.9, frame_ms=106.4, share=0.892, fps=9.4, figures=f, reason="recovered")))
            rt.append(r(CLOSE, ep_summary("session_close", episodes=1 if episodes else 0, start_lines=1 if episodes else 0, end_lines=1 if episodes else 0)))
            rt.append(r(CLOSE, xr_summary("session_close")))
            rt.append(r(CLOSE, slow_summary("session_close", regimes=1 if regime else 0, slow_seconds=40 if regime else 0)))
            return gfx, rt

        case("the test hold is a SLOW regime with its whole chain: SLOW verdict, and the regime is named deliberate", run(test_flight()), 4, "SLOW",
             "  PASS  SLOW TEST: the deliberate 80 ms hold at 10:51:42.000 (40 s): an episode start line of 95.1 ms, 1 episode end line(s), SLOW regime #1 held by the vendor "
             "runtime's xrEndFrame, with a still_slow and an end line",
             "(deliberate: advanced.slow_test_ms)", "; DELIBERATE: the slow test's hold (advanced.slow_test_ms)", "SLOW TEST (advanced.slow_test_ms:",
             "10:51:42.000  graphics log: the hold begins now (80 ms for 40 s)", "10:51:42.000  runtime log: native_end_frame_hold state=began ms=80",
             "10:52:22.000  runtime log: native_end_frame_hold state=ended held_calls=392", "10:50:14.000  graphics log: the key was read (80 ms, the hold begins 90 s in)",
             rx(r"^freezes verdict: SLOW \(0 STOP, 0 WARN, 1 SLOW\): 10:51:42\.000 to 10:52:22\.000 local \(40 s\) at 9\.4 fps of the display's 72 Hz, held by the vendor runtime's "
                r"xrEndFrame: 94\.9 ms of every 106\.4 ms frame \(89%\) \(deliberate: advanced\.slow_test_ms\)"), absent=never_pass + ("  WARN  ", "  STOP  "))
        case("the hold never reached the runtime: the chain lacks it, the episode and the regime", run(test_flight(hold=False, episodes=False, regime=False)), 0, "WARN",
             "  WARN  SLOW TEST: the deliberate 80 ms hold at 10:51:42.000 (40 s), but the chain lacks the runtime's native_end_frame_hold state=began line (the hold never reached the "
             "runtime); the runtime's native_end_frame_hold state=ended line; an end-frame episode start line of at least 72 ms; an end-frame episode end line; a SLOW regime that "
             "begins within 15 s of the hold")
        case("a test with no still_slow and no end line", run(test_flight(still=False, end=False)), 4, "SLOW",
             "  WARN  SLOW TEST: the deliberate 80 ms hold at 10:51:42.000 (40 s), but the chain lacks a still_slow line (the hold lasted 40 s; one is written 30 s after the SLOW line); "
             "the regime's end line")
        case("a test whose SLOW line has no graphics-memory figures beside an armed watch", run(test_flight(fig=False)), 4, "SLOW",
             "the chain lacks the graphics-memory figures on the SLOW line")
        case("the key read and no hold line: the session ended first", run((quiet[0][:-1] + [g(T0 + 2.0, t_armed), quiet[0][-1]], quiet[1])), 0, "WARN",
             "  WARN  SLOW TEST: advanced.slow_test_ms = 80 was read, but the log has no `the hold begins now` line: the session ended before the hold began")
        case("a hold the graphics log never said ended, long after it should have", run(test_flight(ended=False)), 4, "SLOW",
             "the chain lacks the graphics log's `the hold ended` line")

        # ---- a v0.18.0 flight: the older lines only, the regime put back together ----
        legacy = run(legacy_flight(), "--expect-build", VERSION)
        case("a v0.18.0 flight: the regime is rebuilt from LONG FRAME, native_long_cycle, FREEZE and vScreen totals lines, held by the vendor's xrEndFrame", legacy, 4, "SLOW",
             "SLOW REGIME (1 found:",
             "  #1  11:14:38.100 to 11:15:35.238 local  (57.1 s)  [SLOW] RECONSTRUCTED from the older lines: this build has no slow-regime detector",
             rx(r"^      rate: 9\.9[0-9] fps against the display's 72 Hz \(13\.[0-9]% of it; a regime is under 40%\), frames of 100\.[0-9] ms$"),
             rx(r"^      owner: held by the vendor runtime's xrEndFrame: 82\.7 ms of every 100\.[0-9] ms frame \(82%\)$"),
             "(p50 of each phase in native_submit_phases window 50: sequences 96730-96985, ended 11:15:16.800; 256 of its 256 frames are inside the run): xrEndFrame 82.7 ms = 6.0 display "
             "periods of 13.889 ms",
             "GPU clock: native_producer_gpu window 50 (ended 11:15:20.100): EDVR's copy of the eyes took p95 75.6 ms, max 79.4 ms on the GPU; p95 0.04 ms in window 48, before it",
             "evidence: 12 LONG FRAME line(s)", "native_long_cycle line(s)", "1 FREEZE line(s)", "3 vScreen totals window(s) under 40% of the display's rate (26, 10, 10 fps)",
             rx(r"^      frame rate from: runtime sequence 96600 to 97167 over 57\.1 s = 9\.9[0-9] fps; graphics frames 102432 to 102999 over 57\.1 s = 9\.9[0-9] fps$"),
             "  SLOW  SLOW REGIME: 11:14:38.100 to 11:15:35.238 local (57.1 s), 9.9", "(reconstructed from the older lines)",
             "  note  INSTRUMENT: none of the headset-lock arc's lines (vendor events, end-frame episodes, slow-regime detector, graphics memory) is in this log: a build before them",
             "freezes verdict: SLOW (0 STOP, 0 WARN, 1 SLOW): 11:14:38.100 to 11:15:35.238 local (57 s) at 9.9 fps of the display's 72 Hz, held by the vendor runtime's xrEndFrame",
             "FREEZES (2 FREEZE line(s);", absent=never_pass + ("  PASS  SLOW REGIME", "DELIBERATE", "SLOW TEST", "VENDOR EVENTS (", "END-FRAME EPISODES ("))
        if "11:15:41" in legacy[1].split("freezes verdict")[1].split(". STOP")[0]:
            fail("the FREEZE 6.2 s after the regime's end is its own freeze, not the regime's:\n%s" % legacy[1])
        case("the same flight with the graphics log alone: the regime stands, the owner is unknown, so it is a WARN and not a SLOW", run((legacy_flight()[0], None)), 0, "WARN",
             "  #1  11:14:38.100 to 11:15:35.238 local  (57.1 s)  [WARN] RECONSTRUCTED from the older lines",
             "owner: owner unknown: no runtime log was read, so no native_submit_phases window says which call held the frames", "      not a SLOW: no owner could be named",
             "  WARN  SLOW REGIME: 11:14:38.100 to 11:15:35.238 local (57.1 s)", "; not a SLOW: no owner could be named", "  WARN  INSTRUMENT: no runtime log was read",
             "freezes verdict: WARN (0 STOP, 2 WARN) [+1 slow stretch(es) the game's own frame or no named owner held, listed above: not a SLOW]",
             absent=("  SLOW  ", "freezes verdict: SLOW"))
        case("a runtime log with no native_submit_phases window over the run: the owner is unknown too, a WARN",
             run((legacy_flight()[0], [l for l in legacy_flight()[1] if "native_submit_phases" not in l])), 0, "WARN",
             "owner: owner unknown: the runtime log has no native_submit_phases window covering the run", "[WARN] RECONSTRUCTED from the older lines", absent=("  SLOW  ", "freezes verdict: SLOW"))
        case("a menu Presenting at 300 frames a second while the runtime's frames crawl is still a regime: the runtime's sequence is the rate, not the Presents",
             run(legacy_flight(gfx_fps=300.0)), 4, "SLOW",
             rx(r"^      rate: 9\.9[0-9] fps against the display's 72 Hz"),
             rx(r"^      frame rate from: runtime sequence 96600 to 97167 over 57\.1 s = 9\.9[0-9] fps; graphics frames 102431 to 119572 over 57\.1 s = (299\.99|300\.00) fps$"),
             "[SLOW] RECONSTRUCTED from the older lines")
        case("long frames now and then at a steady 72 fps are hitches, not a regime", run(legacy_flight(hitches=True)), 0, None,
             "  note  SLOW REGIME: this build has no slow-regime detector, and its older lines (LONG FRAME, native_long_cycle, FREEZE, vScreen totals) hold no run of 5 s or more with "
             "the frame rate under 40% of the display's; that cannot prove there was none", absent=("SLOW REGIME (", "  SLOW  ", "freezes verdict: SLOW"))
        windows_only = (list(gopen) + [g(RS + 20.0 * k, vscreen(160, 20000)) for k in range(3)] + [g(CLOSE, counts_line())] + [g(RS - 100.0, long_frame(61.0, 5, 6))], None)
        case("three vScreen windows at 8 fps and nothing else: a regime from the windows alone, with no owner to name, so a WARN", run(windows_only), 0, "WARN",
             "[WARN] RECONSTRUCTED from the older lines", "evidence: 3 vScreen totals window(s) under 40% of the display's rate (8, 8, 8 fps)",
             "frame rate from: the median of the vScreen windows' own rates", "bounds: no LONG FRAME, native_long_cycle or FREEZE line is in the run",
             "good to about 20 s")
        case("a v0.18.0 flight and a wrong --expect-build is still the build mismatch", run(legacy_flight(), "--expect-build", "0.18.1-9-g1234567"), 2, None, "BUILD MISMATCH",
             absent=("freezes verdict",))

        # ---- the command line ----
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            try:
                main(["--help"])
            except SystemExit as e:
                if e.code not in (0, None):
                    fail("--help exited %r" % (e.code,))
        helped = " ".join(buf.getvalue().split())
        for phrase in ("SLOW REGIME", "4 for a slow regime", "END-FRAME EPISODES", "VENDOR EVENTS", "VRAM", "SLOW TEST", "advanced.slow_test_ms"):
            if phrase not in helped:
                fail("--help should say %r:\n%s" % (phrase, buf.getvalue()))
            if phrase not in " ".join((__doc__ or "").split()):
                fail("the module docstring should say %r" % phrase)
        if violations:
            fail("--freezes wrote, or tried to: %s" % "; ".join(violations[:5]))
    except Exception:
        import traceback
        traceback.print_exc()
        print("slow regime: the checks stopped at an exception (above)")
        ok = False
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    return ok


def main(argv=None):
    ap = argparse.ArgumentParser(
        description="Locate and read EDVR flight logs.")
    ap.add_argument("--target", default="steam",
                    help="steam, frontier, or a path to the game directory")
    ap.add_argument("--dir", default=None,
                    help="read this log directory directly, ignoring --target")
    ap.add_argument("--file", default=None, help="read exactly this log file")
    ap.add_argument("--tag", default=None,
                    help="gfx (d3d11), vr (legacy OpenVR proxy, retired 2026-09-16), "
                         "openxr (native OpenXR), or all. Omitted, it is gfx, except "
                         "for --tally pose, whose lines are in the runtime log (openxr); "
                         "a tag you name is always the one read")
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
    ap.add_argument("--tally", choices=["vh", "periodic", "pose"], default=None,
                    help="aggregate instead of dumping: pose tables the `pose gap:` "
                         "lines of the runtime log by caller; "
                         "vh counts eye-texture "
                         "DC lines per vh= hash, split by r= render-target "
                         "token, with the DC frame summaries as totals; "
                         "periodic lays the `periodic work:` timing against "
                         "the flight's long frames (graphics log plus its "
                         "runtime log)")
    ap.add_argument("--frame", type=int, default=None,
                    help="with --tally vh, restrict to this census frame "
                         "ordinal; frames past the census line cap have no "
                         "per-draw lines and are reported as summaries")
    ap.add_argument("--camera-census", action="store_true",
                    help="report a VR camera census flight (advanced.vr_camera_census "
                         "= on): the 5 s lines, the cameras and their roles read "
                         "from the kind of each call, each logged frame's "
                         "call sequence reduced to runs, the eye draws' rows, the "
                         "offline join of those rows to the calls' -- which "
                         "camera is an eye's, and which of its caller, signature, "
                         "place in the frame, tangents and view tell it from the "
                         "world's -- the episodes (a sampled frame per trigger, "
                         "aboard or on foot: its calls, its join to the b1 rows, "
                         "the pass's rows), the on-foot naming runs and the "
                         "detour's CPU, and the stage 2 verdict (PASS / WARN / "
                         "STOP) on the world route's injected phase")
    ap.add_argument("--maps-sharp", action="store_true",
                    help="report an on-foot maps gate flight (experimental.on_foot_maps_sharp "
                         "= on): every map or menu the layer took and handed back "
                         "(when, how long, how many eyes skipped the upscaler), the 5 s "
                         "counters summed, whether the VR world route let go and "
                         "re-owned with the gate, and a PASS / WARN / STOP verdict")
    ap.add_argument("--vscreen-fit", action="store_true",
                    help="report a fix.vscreen_res_width = auto flight: the `vScreen "
                         "resolution:` rule line (fitted or legacy, and why) and the "
                         "width applied, the footprint instrument's arming line and "
                         "its 30 s lines (the on-foot screen's width in eye pixels, "
                         "its stability, shape and 1/d law, and the session's stored "
                         "head-on floor against the 5006 px behind Sean's 3504) and what "
                         "is stored for the next launch; PASS / WARN / STOP lines")
    ap.add_argument("--flat-upscale", action="store_true",
                    help="report a flat-profile flight (design doc section 83): the final copy admitted by "
                         "structure, so DLSS, FSR and TAA resolve below the output whatever the post chain; the "
                         "key, routes, first admission, declines, 5 s windows, stand-down and F8 warning lines, "
                         "and KEY / ADMISSION / TREATED / UPSCALE / TONE REFUSALS / STAND-DOWN / F8 WARNING / "
                         "CHAIN / ADVICE PASS / WARN / STOP lines")
    ap.add_argument("--vr-supersampling", action="store_true",
                    help="report a VR flight's Elite-supersampling-below-1 notice (design doc section 83): "
                         "the `vr supersampling:` line from the measured render size, vScreen's adoption line "
                         "and the headset notice; NOTICE / CONSISTENT / HEADSET / FLAT lines")
    ap.add_argument("--ui-composites", action="store_true",
                    help="report the interface composites the UI layer left in the scene (fix.ui_quality on; "
                         "docs/ui-layer-2026-09-23.md, 2026-10-01): the layer's 30 s `composites left in the "
                         "scene` lines (zeros printed, so a 0 is the count having run), the pairs left by vertex "
                         "and pixel shader and family, and INSTRUMENT / DETECTOR / UNCLAIMED / NAMED / OVERFLOW / "
                         "TAKEN lines (a composite no family names, left in the scene, is a STOP); exit 0 for "
                         "PASS or WARN, 1 for STOP, 3 when the log has no such line")
    ap.add_argument("--terrain-checkerboard", action="store_true",
                    help="report a VR flight's Elite-terrain-checkerboard-rendering notice (design doc section 84): the worker's "
                         "start line, one line for the first read and each change (ON, OFF or unknown with its reason, read from "
                         "the game's active graphics preset on a worker thread) and the menu's queued-toast line; READER / NOTICE / "
                         "CHANGES / LIMIT / HEADSET / FLAT lines. No line at all means the reader never started, which is not "
                         "\"read, off\"")
    ap.add_argument("--route-curve", action="store_true",
                    help="report a curved VR world route flight (fix.panel_curvature above 0 "
                         "with experimental.temporal_aa_on_foot_world = auto): the route's 5 s "
                         "lines by token (curve=, curve-reissues=), its OWNS lines, the `panel "
                         "curvature:` notes and the layer's refusals, and a PASS / WARN / STOP "
                         "verdict (strips drawn against eye takes, pending, stood-down, a stale "
                         "build, the OWNS sentence, a fault); exit 0 for PASS or WARN, 1 for "
                         "STOP, 3 when the log has no route line")
    ap.add_argument("--map-bounce", action="store_true",
                    help="report issue 65's flat constant-buffer Map bounce: 5 s summaries, adaptive decision, verification and fail-safe trips; PASS / WARN / STOP, exit 3 when the instrument never ran")
    ap.add_argument("--freezes", action="store_true",
                    help="report a flight's freeze diagnostics (issue 63): the graphics log's "
                         "FREEZE lines (a frame of 250 ms or more, never rate limited) joined "
                         "to their LONG FRAME lines, the runtime log's native_long_cycle lines "
                         "(cycle and main phase), the stall sampler's samples (age and owner), "
                         "and the GPU census's kept stalls by runtime sequence; the runtime "
                         "cycles of 250 ms or more with no FREEZE line; the counts by size and "
                         "the worst few of each half; the sampler's owner census; and a PASS / "
                         "WARN / STOP verdict (INSTRUMENT, UNWRITTEN FREEZES, FREEZE LINES, "
                         "SAMPLER, SUSPENSION, FREEZE TEST, NO FREEZE). Also the headset-lock "
                         "arc: SLOW REGIME (the frame rate under 40%% of the display's for 5 s or "
                         "more, who held it, and the graphics memory then: from the build's own "
                         "native_slow_regime lines, or put back together from a v0.18.0 log's "
                         "LONG FRAME, native_long_cycle, FREEZE and vScreen totals lines), "
                         "END-FRAME EPISODES, VENDOR EVENTS, VRAM and SLOW TEST "
                         "(advanced.slow_test_ms). A log that holds a slow regime the vendor "
                         "runtime, EDVR's copy or EDVR's own work holds gets a SLOW verdict, never "
                         "PASS or WARN; one the game's own frame holds is INFO (a load) or WARN "
                         "(its scenes) and never sets SLOW. Pairs the runtime log like --tally "
                         "periodic (--runtime-file names one). Exit 0 for PASS or WARN, 1 for "
                         "STOP, 2 for a wrong build, 3 when the log holds none of the freeze "
                         "lines (a build from before the freeze logging), 4 for a slow regime "
                         "the vendor runtime or EDVR holds (and no STOP)")
    ap.add_argument("--window-ms", type=float, default=100.0,
                    help="with --tally periodic, a long frame coincides with a "
                         "periodic event when the event's end time is inside "
                         "the frame or within this many ms of it (default 100)")
    ap.add_argument("--runtime-file", default=None,
                    help="with --tally periodic or --freezes, read exactly this "
                         "runtime (edvr_openxr_*.log) log instead of pairing the "
                         "one that opened nearest the graphics log")
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

    # The pose-gap lines are written by the runtime, not the graphics half: --tally pose reads its log when no tag was named. The default is
    # None rather than "gfx" so that a tag named on the command line, gfx included, is never mistaken for the default and overridden.
    if args.tag is None:
        args.tag = "openxr" if args.tally == "pose" else "gfx"

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

    if args.freezes and not args.file and args.tag.lower() != "gfx":
        print("[edvr] --freezes reads the graphics log (--tag gfx) and pairs "
              "its runtime log itself; drop --tag %s." % args.tag)
        return 1

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

    if args.vscreen_fit:
        return print_vscreen_fit(text)
    if args.flat_upscale:
        return print_flat_upscale(text)
    if args.vr_supersampling:
        return print_vr_supersampling(text)
    if args.ui_composites:
        return print_ui_composites(text)
    if args.terrain_checkerboard:
        return print_terrain_checkerboard(text)
    if args.camera_census:
        return print_camera_census(text)
    if args.maps_sharp:
        return print_maps_sharp(text)
    if args.route_curve:
        return print_route_curve(text, path)
    if args.map_bounce:
        return print_map_bounce(text, path)
    if args.freezes:
        return print_freezes(path, text, ver, want, args, native_dirs)
    if args.tally == "periodic":
        return print_periodic_report(path, text, ver, want, args, native_dirs)
    if args.tally == "pose":
        return print_pose_tally(text)
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


def self_test_map_bounce():
    """Exercise map-bounce verdicts and malformed records through the CLI."""
    import contextlib
    import io
    import shutil

    ok = True
    version = "0.18.0-1-g0123456"
    header = "[10:00:00.000] version %s (build 01234567)\n" % version
    def summary(state="off", key="auto", maps=10, bounced=0, flushes=0,
                trips=0, samples=0, mismatches=0, bank=900, sample_bytes=8192):
        # Same token order and spelling as reportMapBounce in flat_runtime.cpp.
        return ("[10:00:05.000] flat map bounce 5s: state=%s key=%s maps=%s tracked=8 other-buffer=1 texture=1 bounced=%s "
                "declined-not-discard=0 declined-foreign-context=0 declined-foreign-thread=0 declined-internal=0 "
                "declined-paused=0 declined-untracked=0 declined-open-full=0 declined-width=0 failed=0 flushes=%s "
                "flush-bytes=4096 flush-ns-per-kb=200.0 bank-bytes=8192 bank-sample-bytes=%s bank-ns-per-kb=%s abandoned=0 "
                "open-at-present=0 trips=%s verify-samples=%s verify-mismatches=%s unchanged-rows=0 rows-checked=0 "
                "width-le64=0 width-le256=0 width-le1k=0 width-le4k=0 width-le8k=0 width-le64k=0 "
                "tracked-read=0 tracked-write=0 tracked-read-write=0 tracked-discard=0 tracked-no-overwrite=0 cb-first-nonzero=0\n" %
                (state, key, maps, bounced, flushes, sample_bytes, bank, trips, samples, mismatches))

    fixtures = (
        ("fast adaptive OFF", summary() + "[10:00:01.000] flat map bounce: decision at frame 10, batch rates 2.000/2.000/2.000/2.000 B/ns, threshold=1.0 -> OFF, key=auto\n", 0, "map-bounce verdict: PASS", "PASS  OFF"),
        ("adaptive OFF after sampling window", summary() + summary(sample_bytes=0, bank=0) + "[10:00:01.000] flat map bounce: decision at frame 10, batch rates 2.000/2.000/2.000/2.000 B/ns, threshold=1.0 -> OFF, key=auto\n", 0, "map-bounce verdict: PASS", "PASS  OFF"),
        ("adaptive OFF without sample bytes", summary(sample_bytes=0, bank=0) + "[10:00:01.000] flat map bounce: decision at frame 10, batch rates 2.000/2.000/2.000/2.000 B/ns, threshold=1.0 -> OFF, key=auto\n", 0, "map-bounce verdict: WARN", "WARN  OFF"),
        ("active bounce", summary("on", bounced=8, flushes=8, samples=8, bank=1800), 0, "map-bounce verdict: PASS", "PASS  ACTIVE"),
        ("pending decision", summary("pending", bank=1800), 0, "map-bounce verdict: WARN", "WARN  PENDING"),
        ("fail-safe trip", summary("tripped", trips=1, bank=1800), 1, "map-bounce verdict: STOP", "STOP  TRIPS"),
        ("verification mismatch", summary("on", bounced=3, flushes=3, trips=0, samples=3, mismatches=1, bank=1800), 1, "map-bounce verdict: STOP", "STOP  VERIFY"),
        ("slow bank reads while forced off", summary("off", key="off", bank=2048), 0, "map-bounce verdict: WARN", "WARN  SLOW OFF"),
        ("missing instrument", "[10:00:00.000] nothing to report\n", 3, "map-bounce verdict: NEVER RAN", "NEVER RAN"),
        ("missing safety fields", header + "[10:00:05.000] flat map bounce 5s: state=off key=auto maps=10\n", 1, "map-bounce verdict: STOP", "STOP  TRIPS"),
        ("fractional counter is malformed", header + summary(maps="1.5"), 0, "map-bounce verdict: WARN", "WARN  FORMAT"),
        ("oversized integer is malformed", header + summary(maps="9" * 5000), 0, "map-bounce verdict: WARN", "WARN  FORMAT"),
        ("negative bank time is malformed", header + summary(bank="-1"), 0, "map-bounce verdict: WARN", "WARN  FORMAT"),
        ("verification missing after 16 flushes", header + summary("on", key="on", bounced=16, flushes=16, bank=1800), 0, "map-bounce verdict: WARN", "WARN  VERIFY COVERAGE"),
        ("malformed tail with trip", header + summary() + summary(maps="bad", trips=1), 1, "map-bounce verdict: STOP", "STOP  TRIPS"),
        ("malformed tail with mismatch", header + summary() + summary(maps="bad", mismatches=1), 1, "map-bounce verdict: STOP", "STOP  VERIFY"),
        ("auto off without measured decision", header + summary(), 0, "map-bounce verdict: WARN", "WARN  OFF"),
        ("on with no bounce count", header + summary("on", bounced=0, flushes=0, bank=1800), 0, "map-bounce verdict: WARN", "WARN  ACTIVE"),
    )
    tmp = tempfile.mkdtemp(prefix="edvr_map_bounce_test_")
    try:
        for i, (name, body, wanted_rc, verdict, detail) in enumerate(fixtures):
            path = os.path.join(tmp, "%02d.log" % i)
            with open(path, "w", encoding="utf-8") as f:
                f.write(header + body if "version " not in body else body)
            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                rc = main(["--file", path, "--map-bounce", "--expect-build", version])
            got = output.getvalue()
            if rc != wanted_rc or verdict not in got or detail not in got:
                print("map-bounce %s: rc=%d wanted %d, expected %r and %r:\n%s" %
                      (name, rc, wanted_rc, verdict, detail, got))
                ok = False

        mismatch = os.path.join(tmp, "mismatch.log")
        with open(mismatch, "w", encoding="utf-8") as f:
            f.write(header + summary())
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            rc = main(["--file", mismatch, "--map-bounce", "--expect-build",
                       "0.18.0-2-g7654321"])
        if rc != 2 or "BUILD MISMATCH" not in output.getvalue() or "map-bounce verdict" in output.getvalue():
            print("map-bounce expected-build mismatch did not stop before report (rc=%d):\n%s" % (rc, output.getvalue()))
            ok = False

        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            try:
                main(["--help"])
            except SystemExit as e:
                if e.code not in (0, None):
                    print("map-bounce --help exited %r" % (e.code,)); ok = False
        help_text = " ".join(output.getvalue().split())
        if "--map-bounce" not in help_text or "adaptive decision" not in help_text:
            print("--help does not describe --map-bounce")
            ok = False
        if "flat map bounce 5s:" not in (__doc__ or ""):
            print("module help does not describe the map-bounce log prefix")
            ok = False
    except Exception:
        import traceback
        traceback.print_exc()
        print("map-bounce: self-test stopped at an exception")
        ok = False
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    return ok


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
    if not self_test_camera_census():
        ok = False
    if not self_test_maps_sharp():
        ok = False
    if not self_test_ui_composites():
        ok = False
    if not self_test_vscreen_fit():
        ok = False
    if not self_test_flat_upscale():
        ok = False
    if not self_test_route_curve():
        ok = False
    if not self_test_map_bounce():
        ok = False
    if not self_test_slow_regime():
        ok = False
    if not self_test_freezes():
        ok = False
    if not self_test_terrain_checkerboard():
        ok = False
    if not self_test_pose():
        ok = False

    print("self-test: %s" % ("ok" if ok else "FAILED"))
    return 0 if ok else 1


FLATU_FIXTURE = "flat_upscale_fixture.log"


POSE_FIXTURE = "pose_gap_fixture.log"


def self_test_pose():
    """--tally pose on tools\\pose_gap_fixture.log (which tools\\openxr_pose_test holds to exactly what src/openxr/pose_gap.h writes for the scripted
    flight: 24 lines -- twelve 60-second windows of Elite's own "now" request (thread 24212, answered at the display time: gap 0, a small flat
    angle, one fallback call in window 2, three failed in window 5) and of another caller (thread 7001, outside the image, a real prediction: gap 9.5
    ms, an angle of 0.02 degrees per deg/s of head speed)), then on lines altered to break each thing the report depends on. Returns ok."""
    ok = True

    def fail(message):
        nonlocal ok
        print("self_test_pose: %s" % message)
        ok = False

    import contextlib
    import io
    import shutil
    fixture = os.path.join(os.path.dirname(os.path.abspath(__file__)), POSE_FIXTURE)
    if not os.path.isfile(fixture):
        fail("the fixture %s is missing beside this script" % POSE_FIXTURE)
        return False
    text = read_text(fixture)
    windows, notes = parse_pose_gap(text)
    if len(windows) != 24 or notes:
        fail("the fixture parsed to %d windows and %d notes, want 24 and none" % (len(windows), len(notes)))
        return False
    w0, w1 = windows[0], windows[1]
    if (w0["tid"], w0["calls"], w0["frm"], w0["waits"], w0["failed"], w0["fb"]) != (24212, 120, "exe+0x4E3881", 5400, 0, 0) \
            or w0["pred"] != 0.0 or w0["gm"] != 0.0 or w0["gmin"] != 0.0 or w0["gmax"] != 0.0 or w0["am"] != 0.05 or w0["amax"] != 0.05 or w0["head"] != 10.0:
        fail("window 1 parsed to %r" % (w0,))
    if (w1["tid"], w1["calls"], w1["frm"]) != (7001, 60, "outside") or w1["pred"] != 11.0 or w1["gm"] != 9.5 or w1["gmin"] != 9.0 or w1["gmax"] != 10.0:
        fail("window 2 (the other caller, outside the image, prediction 11 ms) parsed to %r" % (w1,))

    def close(a, b, eps=0.005):
        return a is not None and abs(a - b) <= eps

    rows = {r["tid"]: r for r in tally_pose(windows)}
    if sorted(rows) != [7001, 24212]:
        fail("the rows -> %r" % (sorted(rows),))
        return False
    elite = rows[24212]
    if elite["frm"] != "exe+0x4E3881" or elite["n"] != 12 or elite["calls"] != 1440 or elite["failed"] != 3 or elite["fallbacks"] != 1 \
            or not close(elite["gap"][0], 0.0) or not close(elite["gap"][1], 0.0) or not close(elite["angle"][0], 0.055) \
            or not close(elite["head"][0], 65.0) or elite["angle_range"] != (0.05, 0.06) or elite["r"] is None or abs(elite["r"]) > 0.4:
        fail("Elite's own request -> %r (a gap of 0 and an angle that does not follow head speed)" % (elite,))
    other = rows[7001]
    if other["frm"] != "outside" or other["n"] != 12 or other["calls"] != 720 or not close(other["gap"][0], 9.5) or not close(other["angle"][0], 1.3) \
            or not close(other["angle"][1], 0.7, 0.05) or not close(other["head"][0], 65.0) or other["r"] is None or not close(other["r"], 1.0, 1e-9):
        fail("the other caller -> %r (the true gap, an angle that follows head speed)" % (other,))
    # The correlation needs three windows and a spread on both sides.
    two = [w for w in windows if w["tid"] == 7001][:2]
    if tally_pose(two)[0]["r"] is not None:
        fail("two windows gave a correlation")
    flat = [dict(w, am=0.05) for w in windows if w["tid"] == 7001]
    if tally_pose(flat)[0]["r"] is not None:
        fail("a flat angle gave a correlation")
    if _pearson([(1.0, 1.0), (2.0, 4.0), (3.0, 9.0)]) is None or abs(_pearson([(1.0, 3.0), (2.0, 2.0), (3.0, 1.0)]) + 1.0) > 1e-9:
        fail("_pearson of a rising and a falling line")
    # A line altered: n/a figures parse as None and stay out of every mean.
    altered = text.replace("target-minus-display mean 0.00 ms (min 0.00 max 0.00)", "target-minus-display n/a", 1)
    aw, _ = parse_pose_gap(altered)
    if aw[0]["gm"] is not None or aw[0]["gmin"] is not None or aw[0]["gmax"] is not None:
        fail("an n/a gap parsed as a number: %r" % (aw[0],))
    ar = {r["tid"]: r for r in tally_pose(aw)}[24212]
    if ar["n"] != 12 or not close(ar["gap"][0], 0.0):
        fail("an n/a window changed the gap mean -> %r" % (ar,))
    # The over-the-limit line is a note, not a window.
    more = text + "[00:00:00.000] pose gap: more than 16 callers in a window; 4 calls not counted\n"
    mw, mn = parse_pose_gap(more)
    if len(mw) != 24 or mn != ["more than 16 callers in a window; 4 calls not counted"]:
        fail("the over-the-limit line -> %d windows, notes %r" % (len(mw), mn))
    # Through main(), on a directory the tool discovers on its own: the runtime log is read by default.
    tmp = tempfile.mkdtemp(prefix="edvr_pose_selftest_")
    try:
        logs = os.path.join(tmp, "edvr_logs")
        os.makedirs(logs)
        with open(os.path.join(logs, "edvr_openxr_20261009_120000_123_77.log"), "wb") as f:
            f.write(("[00:00:00.001] version 0.18.3-1-gabcdef0 (build 68C0A1F2) -- this DLL was linked 2026-10-09 12:00:00 UTC\n" + text).encode("utf-8"))
        with open(os.path.join(logs, "edvr_gfx_20261009_120001.log"), "wb") as f:
            f.write(b"[00:00:00.001] version 0.18.3-1-gabcdef0 (build 68C0A1F2) -- x\n[00:00:01.000] nothing here\n")

        def run(argv):
            buf = io.StringIO()
            with contextlib.redirect_stdout(buf):
                rc = main(argv)
            return rc, buf.getvalue()

        rc, out = run(["--dir", logs, "--tally", "pose"])
        for want in ("tally pose: 24 window line(s) over 2 caller row(s)",
                     "   7001 outside          12     720      0  +9.50 +/- 0.00",
                     " 24212 exe+0x4E3881     12    1440      3  +0.00 +/- 0.00",
                     "1.300 +/- 0.7", "+1.00",
                     "exe+0x4E3881, tid 24212: its pose is located +0.00 ms from the drawn frame's display time, and is turned 0.055 deg from the drawn pose "
                     "(0.050 to 0.060 across windows)",
                     "outside, tid 7001: its pose is located +9.50 ms from the drawn frame's display time, and is turned 1.300 deg from the drawn pose "
                     "(0.200 to 2.400 across windows); angle against head speed r = +1.00 over 12 window(s).",
                     "tid 24212: 1 call(s) could not be located at the display time", "before the fix Elite's was 41-44 ms"):
            if want not in out:
                fail("--tally pose output lacks %r:\n%s" % (want, out))
        if rc != 0:
            fail("--tally pose exited %d" % rc)
        with open(os.path.join(logs, "edvr_openxr_20261009_130000_123_77.log"), "wb") as f:
            f.write(b"[00:00:00.001] version 0.18.3-1-gabcdef0 (build 68C0A1F2) -- x\n[00:00:01.000] nothing here\n")
        rc, out = run(["--dir", logs, "--tally", "pose", "--nth", "0"])
        if rc != 1 or "no `pose gap:` lines" not in out:
            fail("a log with no pose gap line -> rc %d:\n%s" % (rc, out))
        rc, out = run(["--dir", logs, "--tally", "pose", "--tag", "gfx"])
        if rc != 1 or "no `pose gap:` lines" not in out or "edvr_gfx_20261009_120001.log" not in out or "edvr_openxr_" in out:
            fail("an explicit --tag gfx reads the graphics log, which has none -> rc %d:\n%s" % (rc, out))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    # Which log is read, from what each carries: an older runtime log and a NEWER graphics log, each with a pose row of its own (+0.00 ms and
    # +123.00 ms), so the file that was read shows in the output twice over, by name and by content. The tag defaults to the runtime log for
    # --tally pose only when it was omitted; a tag that was named, gfx included, is honoured; --file controls whatever the tag says.
    tmp = tempfile.mkdtemp(prefix="edvr_pose_selftest_tag_")
    try:
        logs = os.path.join(tmp, "edvr_logs")
        os.makedirs(logs)
        head = "[00:00:00.001] version 0.18.3-1-gabcdef0 (build 68C0A1F2) -- this DLL was linked 2026-10-09 12:00:00 UTC\n"

        def row(gap):
            return ("[00:01:00.000] pose gap: tid 24212 calls 120 from exe+0x4E3881 prediction 0.0 ms target-minus-display mean %.2f ms "
                    "(min %.2f max %.2f) angle-to-drawn mean 0.050 max 0.050 deg head 10.0 deg/s waitgetposes 5400 failed 0\n" % (gap, gap, gap))

        runtime_name, gfx_name = "edvr_openxr_20261009_120000_123_77.log", "edvr_gfx_20261009_120001.log"
        runtime_path, gfx_path = os.path.join(logs, runtime_name), os.path.join(logs, gfx_name)
        with open(runtime_path, "wb") as f:
            f.write((head + row(0.0)).encode("utf-8"))
        with open(gfx_path, "wb") as f:
            f.write((head + row(123.0)).encode("utf-8"))

        def run_tag(argv):
            buf = io.StringIO()
            with contextlib.redirect_stdout(buf):
                rc = main(argv)
            return rc, buf.getvalue()

        def reads(label, argv, name, gap, other_name, other_gap):
            rc, out = run_tag(argv)
            read_line = [l for l in out.splitlines() if l.startswith("[edvr] ") and ".log" in l and "lines" in l]
            if (rc != 0 or len(read_line) != 1 or name not in read_line[0] or other_name in out
                    or ("is located %+.2f ms from the drawn frame's display time" % gap) not in out
                    or ("is located %+.2f ms" % other_gap) in out):
                fail("%s: expected %s at %+.2f ms, not %s at %+.2f ms -> rc %d:\n%s" % (label, name, gap, other_name, other_gap, rc, out))

        reads("--tally pose with no tag reads the runtime log", ["--dir", logs, "--tally", "pose"], runtime_name, 0.0, gfx_name, 123.0)
        reads("--tally pose --tag gfx reads the graphics log it was told to", ["--dir", logs, "--tally", "pose", "--tag", "gfx"], gfx_name, 123.0, runtime_name, 0.0)
        reads("--tally pose --tag openxr reads the runtime log", ["--dir", logs, "--tally", "pose", "--tag", "openxr"], runtime_name, 0.0, gfx_name, 123.0)
        reads("--tally pose --tag GFX is the same tag in any case", ["--dir", logs, "--tally", "pose", "--tag", "GFX"], gfx_name, 123.0, runtime_name, 0.0)
        reads("--tally pose --tag all reads the newest log of any tag", ["--dir", logs, "--tally", "pose", "--tag", "all"], gfx_name, 123.0, runtime_name, 0.0)
        reads("--tally pose --nth 0 with no tag still reads the runtime log", ["--dir", logs, "--tally", "pose", "--nth", "0"], runtime_name, 0.0, gfx_name, 123.0)
        reads("--file controls with no tag", ["--file", gfx_path, "--tally", "pose"], gfx_name, 123.0, runtime_name, 0.0)
        reads("--file controls over a tag that disagrees", ["--file", runtime_path, "--tally", "pose", "--tag", "gfx"], runtime_name, 0.0, gfx_name, 123.0)
        # The other tallies keep their default: with no tag the graphics log is read (here, the tally finds nothing in it, which says which one it was).
        rc, out = run_tag(["--dir", logs, "--tally", "vh"])
        if gfx_name not in out or runtime_name in out:
            fail("--tally vh with no tag no longer defaults to the graphics log -> rc %d:\n%s" % (rc, out))
        rc, out = run_tag(["--dir", logs, "--list"])
        if gfx_name not in out or runtime_name in out:
            fail("--list with no tag no longer defaults to the graphics logs -> rc %d:\n%s" % (rc, out))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    return ok

def self_test_flat_upscale():
    """--flat-upscale on the checked-in synthetic flight (tools\\flat_upscale_fixture.log, which tools\\flat_temporal_test holds to exactly what the DLL's
    formatters write): a good flight below the output, then each episode appended to it, then logs altered to take away each thing the report
    depends on and to break each thing the verdict judges; and --vr-supersampling on lines built from the header's own text. Returns ok."""
    import contextlib
    import io
    ok = True

    def fail(msg):
        nonlocal ok
        print("flat upscale: %s" % msg)
        ok = False

    def report(text, fn=None):
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            rc = (fn or print_flat_upscale)(text)
        return rc, buf.getvalue()

    def statuses(text, fn=None):
        """{tag: status} of the verdict lines of a log, and the whole report."""
        _, out = report(text, fn)
        found = {}
        for line in out.splitlines():
            m = re.match(r"^(PASS|WARN|STOP|n/a) \(([A-Z0-9 -]+)\) ", line)
            if m:
                found[m.group(2)] = m.group(1)
        return found, out

    def sub(text, old, new, count=-1):
        """text with `old` replaced by `new`; fails the test (and returns text) when `old` is not there: a mutation that changes nothing would pass
        every check for the wrong reason."""
        if old not in text:
            fail("the mutation %r -> %r found nothing to change" % (old, new))
            return text
        return text.replace(old, new, count)

    def want_statuses(text, want, label):
        got, out = statuses(text)
        for tag, status in want.items():
            if got.get(tag) != status:
                fail("%s: %s is %r, wanted %r:\n%s" % (label, tag, got.get(tag), status, out))

    here = os.path.dirname(os.path.abspath(__file__))
    fixture = os.path.join(here, FLATU_FIXTURE)
    if not os.path.isfile(fixture):
        fail("the fixture %s is missing beside this script" % FLATU_FIXTURE)
        return False
    whole = read_text(fixture)
    segments, current = {"base": []}, "base"
    for line in whole.splitlines():
        m = re.match(r"^# episode: (.+)$", line)
        if m:
            current = m.group(1)
            segments[current] = []
        else:
            segments[current].append(line)
    base = "\n".join(segments["base"]) + "\n"
    if sorted(segments) != ["base", "game-aa", "old-advice", "render-size"]:
        fail("the fixture's segments are %r" % sorted(segments))
        return False

    def with_episode(name):
        return base + "\n".join(segments[name]) + "\n"

    # ---- the parser, on the good flight ----
    f = parse_flat_upscale(base)
    if len(f["keys"]) != 1 or f["keys"][0]["key"] != "auto" or f["keys"][0]["when"] != "read at startup" or len(f["windows"]) != 5 or len(f["routes"]) != 1 or \
            f["routes"][0]["r"] != (2880, 1620) or f["routes"][0]["d"] != (3840, 2160) or f["routes"][0]["name"] != "trained-upscale" or len(f["runtime"]) != 5:
        fail("the fixture's key, windows, route and runtime lines parsed as %r" % ({k: (v if k in ("keys", "routes") else len(v) if isinstance(v, list) else v) for k, v in f.items()},))
    fi = f["first"]
    if not fi or (fi["frame"], fi["src"], fi["scene"], fi["output"], fi["whitelist"], fi["route"], fi["menu"]) != \
            (1919, (2880, 1620), (2880, 1620), (3840, 2160), "no-known-tone-pass", "trained-upscale", False):
        fail("the first admission parsed as %r" % (fi,))
    stand = [(s["what"], s["reason"]) for s in f["stand"]]
    if stand != [("entered", "no-3d-scene"), ("resumed", None)] or f["warns"] or f["refusals"] != {"no-3d-scene": 1482} or f["old_advice"]:
        fail("the stand-down, warning and refusal lines parsed as %r %r %r" % (stand, f["warns"], f["refusals"]))
    w = flat_upscale_windows(f)
    if len(w) != 5 or (w[0]["copies"], w[0]["no_scene"], w[0]["admitted"], w[0]["last"], w[0]["scene"], w[0]["output"]) != (741, 741, 0, "no-scene", None, (3840, 2160)) or \
            (w[2]["admitted"], w[2]["copies"], w[2]["scene"], w[2]["source"], w[2]["ldr_before_max"], w[2]["last"]) != (331, 331, (2880, 1620), (2880, 1620), 0, "admitted"):
        fail("the fixture's windows parsed as %r" % (w,))
    # A line cut short or garbled is skipped, never fatal.
    g = parse_flat_upscale("flat copy structure 5s: key=auto copies=x\nflat stand-down: entered at frame=zz\nflat route: trained-upscale R=\n"
                           "flat settings warning: shown (mode=\nflat runtime: treated=\nnothing at all\n")
    if g["first"] is not None or g["routes"] or g["runtime"] or len(g["windows"]) != 1:
        fail("a cut-short line was mis-parsed: %r" % (g,))

    # ---- the report on the good flight: every question PASSes ----
    rc, out = report(base)
    flat = re.sub(r"[ ]+", " ", out)
    for want in (
            "[edvr] flat upscale: 1 key line(s), 5 copy-structure window(s), 1 route line(s), 2 stand-down line(s), 0 warning line(s), 0 decline line(s)",
            "key 15:12:02.151: experimental.temporal_aa_before_post=auto (read at startup)",
            "route 15:12:18.913: trained-upscale R=2880x1620 E=3840x2160 D=3840x2160",
            "first admission 15:12:18.914 at frame 1919: a 2880x1620 image, scene 2880x1620 on a 3840x2160 output, route trained-upscale; the whitelist said no-known-tone-pass",
            "window 15:12:21.149: key=auto copies 331 (whitelist 0, admitted 331, declined 0, selector-refused 0, no-3d-scene 0, render-size 0, route-serves 0, key-off 0); last admitted; "
            "scene 2880x1620 output 3840x2160 source 2880x1620; longest chain 0",
            "stand-down 15:12:08.368: entered for no-3d-scene",
            "stand-down 15:12:18.903: resumed",
            "PASS (KEY) experimental.temporal_aa_before_post=auto (read at startup)",
            "PASS (ADMISSION) 5 window(s): copies 1753, whitelist 0, admitted 1009, declined 0, selector-refused 0, no-3d-scene 744, render-size 0, route-serves 0, key-off 0; first admission at frame 1919",
            "PASS (TREATED) 1009 frame(s) treated over the log (the counter went 0 -> 1009)",
            "PASS (UPSCALE) below the output (trained-upscale R=2880x1620 D=3840x2160): 1009 frame(s) admitted by structure, treated",
            "PASS (TONE REFUSALS) no frame was refused for a tone pass",
            "PASS (STAND-DOWN) 1 stand-down(s): 15:12:08.368: no-3d-scene",
            "PASS (F8 WARNING) the panel showed no warning",
            "PASS (CHAIN) no R-sized image pass between the scene HDR's first consumer and the copy (the longest chain seen: 0)",
            "PASS (ADVICE) the supersampling advice is gone (below 1.0 is supported)",
            "flat upscale verdict: PASS (9 PASS, 0 WARN, 0 STOP, 0 n/a)"):
        if want not in flat:
            fail("the good flight's report lacks %r:\n%s" % (want, out))
    if rc != 0:
        fail("the good flight reported exit %d" % rc)

    # ---- the episodes ----
    want_statuses(with_episode("render-size"), {"KEY": "PASS", "ADMISSION": "PASS", "TREATED": "PASS", "STAND-DOWN": "WARN", "F8 WARNING": "WARN", "CHAIN": "PASS", "ADVICE": "PASS"},
                  "a render size that does not fit")
    _, out = statuses(with_episode("render-size"))
    flat = re.sub(r"[ ]+", " ", out)
    for want in ("stand-down 15:14:28.335: entered for render-size-does-not-fit-output (Elite renders 2176x1224 on a 2560x1600 screen)",
                 "F8 warning 15:14:28.335: shown for render-size-does-not-fit-output: DLSS is not active: Elite renders 2176x1224 on a 2560x1600 screen. | Set Elite's resolution to your screen's",
                 "WARN (F8 WARNING) 1 warning(s): 15:14:28.335 render-size-does-not-fit-output (the render size)",
                 "flat upscale verdict: WARN"):
        if want not in flat:
            fail("the render-size episode's report lacks %r:\n%s" % (want, out))
    want_statuses(with_episode("game-aa"), {"TONE REFUSALS": "WARN", "STAND-DOWN": "WARN", "F8 WARNING": "WARN", "CHAIN": "WARN", "ADMISSION": "PASS", "ADVICE": "PASS"}, "a game AA chain")
    _, out = statuses(with_episode("game-aa"))
    flat = re.sub(r"[ ]+", " ", out)
    for want in ("declined 15:16:40.100: r-sized-image-passes-follow-the-first-consumer-of-the-scene-hdr (the whitelist said no-known-tone-pass; a 2880x1620 image, 2 R-sized pass(es) between)",
                 "WARN (CHAIN) the structure declined 600 frame(s) with R-sized image passes", "(the longest chain: 2)"):
        if want not in flat:
            fail("the game-AA episode's report lacks %r:\n%s" % (want, out))
    # The CHAIN count is the windows' frames, not the decline lines (one line a cause a session); with no window that counted them it says lines.
    no_counts = re.sub(r"(declines=)r-sized-image-passes-follow-the-first-consumer-of-the-scene-hdr:\d+", r"\1none", with_episode("game-aa"))
    _, out = statuses(no_counts)
    if "frames (1 decline line(s); no window counted them)" not in re.sub(r"[ ]+", " ", out):
        fail("a game-AA log whose windows counted no declines should name the decline lines, not call them frames:\n%s" % out)
    want_statuses(with_episode("old-advice"), {"ADVICE": "STOP", "F8 WARNING": "STOP"}, "a build from before section 83")

    # ---- take away what the report depends on, and break what the verdict judges ----
    no_windows = "\n".join(l for l in base.splitlines() if "flat copy structure 5s:" not in l) + "\n"
    rc, out = report(no_windows)
    got, _ = statuses(no_windows)
    if got.get("ADMISSION") != "STOP" or rc != 0 or got.get("UPSCALE") != "STOP":
        fail("a flight with no admission window should say ADMISSION STOP and (nothing admitted below the output) UPSCALE STOP, exit 0: %r rc=%d" % (got, rc))
    want_statuses(sub(base, "flat hdr route: auto (read at startup)", "flat hdr route: off (read at startup)"), {"KEY": "WARN"}, "the key off")
    no_key = "\n".join(l for l in base.splitlines() if "flat hdr route:" not in l) + "\n"
    want_statuses(no_key, {"KEY": "n/a", "ADMISSION": "PASS"}, "no key line")
    # Windows that never ruled on a final copy: the admission ran and had nothing to say, which is not a PASS.
    no_copies = "\n".join(re.sub(r"(copies|whitelist|admitted|no-scene)=\d+", r"\1=0", l) if "flat copy structure 5s:" in l else l for l in base.splitlines()) + "\n"
    want_statuses(no_copies, {"ADMISSION": "WARN"}, "windows with no final copy")
    _, out = statuses(no_copies)
    if "NO final copy was ruled on in any window" not in out:
        fail("windows with copies=0 should say no final copy was ruled on:\n%s" % out)
    untreated = re.sub(r"flat runtime: treated=\d+", "flat runtime: treated=0", base)
    want_statuses(untreated, {"TREATED": "STOP", "UPSCALE": "STOP"}, "nothing treated")
    no_runtime = "\n".join(l for l in base.splitlines() if "flat runtime:" not in l) + "\n"
    want_statuses(no_runtime, {"TREATED": "n/a", "UPSCALE": "STOP"}, "no runtime lines")
    no_route = "\n".join(l for l in base.splitlines() if "flat route:" not in l) + "\n"
    want_statuses(no_route, {"UPSCALE": "n/a"}, "no route line")
    # The startup false warning: a warning shown before any scene is a STOP, whatever it says.
    startup = sub(base, "[15:12:08.368] flat stand-down: entered", "[15:12:08.360] flat settings warning: shown (mode=DLSS, frames refused for no-known-tone-pass, work stood down): DLSS is not "
                  "active: Elite's post-processing is not recognised. Turn off in Elite's graphics options: Anti-aliasing, Bloom, Depth of field\n[15:12:08.368] flat stand-down: entered", 1)
    want_statuses(startup, {"F8 WARNING": "STOP"}, "the false startup warning")
    _, out = statuses(startup)
    if "BEFORE ANY SCENE" not in out:
        fail("the false startup warning is not named:\n%s" % out)
    # A stand-down for a chain nothing recognised (no declines to explain it) is a STOP.
    chain = base + "\n".join(l for l in segments["game-aa"] if "flat stand-down:" in l or "flat runtime refusal" in l) + "\n"
    want_statuses(chain, {"STAND-DOWN": "STOP", "TONE REFUSALS": "WARN"}, "an unrecognised chain")
    hdr = base + "[15:20:00.000] flat stand-down: entered at frame=9: every frame for 5.0 s (400 frames) was refused for no-hdr-consumer, none treated; paused: x\n"
    want_statuses(hdr, {"STAND-DOWN": "STOP"}, "a missing HDR consumer")
    # Tone refusals with nothing treated are a STOP rather than a WARN.
    want_statuses(untreated + "[15:20:00.000] flat runtime refusal 5s: reason=no-known-tone-pass count=500\n", {"TONE REFUSALS": "STOP"}, "refusals with nothing treated")
    # A VR log has no flat lines: exit 1.
    rc, out = report("[10:00:00.000] version v0.18.0 (build 1)\n[10:00:01.000] vScreen: something\n")
    if rc != 1 or "no flat-profile line" not in out:
        fail("a log with no flat line should exit 1 and say so: rc=%d %r" % (rc, out))
    # This reader's copy of the header's key text: the fixture's key line is the runtime's.
    runtime_cpp = os.path.join(os.path.dirname(here), "src", "d3d11", "flat_runtime.cpp")
    if os.path.isfile(runtime_cpp):
        r = read_text(runtime_cpp)
        if "flat hdr route: %s (read at startup) at frame=%llu: %s" not in r:
            fail("src\\d3d11\\flat_runtime.cpp no longer writes the key line this reader parses")
    else:
        fail("src\\d3d11\\flat_runtime.cpp is not where the self-test looks for it (%s)" % runtime_cpp)

    # ---- --vr-supersampling, from the header's own text ----
    header = os.path.join(os.path.dirname(here), "src", "common", "vr_supersample_notice.h")
    vscreen = os.path.join(os.path.dirname(here), "src", "d3d11", "vscreen.cpp")
    notice_prefix = "vr supersampling: Elite draws the 3D world at %ux%u, %u%% of the %ux%u eye texture, and scales it up before EDVR sees it: "
    adopt_prefix = "vScreen: the world on this rig is rendered at %ux%u and scaled into the %ux%u the headset is handed -- %u%% of the width"
    if os.path.isfile(header):
        if notice_prefix.replace("\\", "") not in read_text(header).replace("\"\n        \"", ""):
            fail("src\\common\\vr_supersample_notice.h's log line is not the text this reader parses")
        if kBelow := re.search(r"constexpr uint32_t kBelowPercent = (\d+);", read_text(header)):
            if int(kBelow.group(1)) != VRSS_BELOW_PERCENT:
                fail("this reader's VRSS_BELOW_PERCENT (%d) is not the header's kBelowPercent (%s)" % (VRSS_BELOW_PERCENT, kBelow.group(1)))
        else:
            fail("kBelowPercent was not found in the header")
    else:
        fail("src\\common\\vr_supersample_notice.h is not where the self-test looks for it (%s)" % header)
    if os.path.isfile(vscreen):
        if adopt_prefix.replace("%ux%u", "%ux%u") not in read_text(vscreen).replace("\"\n                \"", ""):
            fail("src\\d3d11\\vscreen.cpp's adoption line is not the text this reader parses")
    notice = ("[09:30:12.100] " + (notice_prefix % (2112, 2304, 75, 2816, 3072)) + "Elite's Supersampling is below 1 (an upscaler in the chain reads the same). EDVR's DLSS then upscales an "
              "image that is already upscaled, which softens the world and the holograms. Set Elite's Supersampling to 1 and raise HMD Image Quality instead: EDVR's DLSS upscales from that. "
              "Measured from the render sizes, not read from Elite's settings file.\n")
    adopt = ("[09:30:12.098] " + (adopt_prefix % (2112, 2304, 2816, 3072, 75)) + ", which is what supersampling away from 1.0 and every upscaler in the chain do (FSR and NIS at their \"ultra quality\" are exactly this).\n")
    queued = "[09:30:12.300] vr supersampling: the headset notice is queued as a toast (\"Elite Supersampling is below 1: use HMD Image Quality\"); the Status page shows the advice as its hint while the menu is open.\n"
    vr = "[09:29:00.000] version v0.18.0-rc.5-26-g5ec0de01 (build 5EC0DE01) -- this DLL was linked 2026-10-01 20:05:44 UTC\n" + adopt + notice + queued
    p = parse_vr_supersampling(vr)
    if not p["notice"] or p["notice"]["r"] != (2112, 2304) or p["notice"]["pct"] != 75 or p["notice"]["e"] != (2816, 3072) or not p["adopt"] or p["adopt"]["pct"] != 75 or not p["queued"] or not p["toast"] or p["flat"]:
        fail("the VR lines parsed as %r" % (p,))
    want_statuses_vr = lambda text, want, label: [fail("%s: %s is %r, wanted %r" % (label, t, statuses(text, print_vr_supersampling)[0].get(t), s))
                                                  for t, s in want.items() if statuses(text, print_vr_supersampling)[0].get(t) != s]
    want_statuses_vr(vr, {"NOTICE": "PASS", "CONSISTENT": "PASS", "HEADSET": "PASS"}, "the good VR flight")
    _, out = statuses(vr, print_vr_supersampling)
    flat = re.sub(r"[ ]+", " ", out)
    for want in ("adoption 09:30:12.098: the world is drawn at 2112x2304 and scaled into the 2816x3072 the headset is handed (75% of the width)",
                 "notice 09:30:12.100: 2112x2304, 75% of the 2816x3072 eye texture",
                 "PASS (NOTICE) the world is drawn at 2112x2304, 75% of the 2816x3072 eye texture: Elite's Supersampling is below 1",
                 "PASS (CONSISTENT) the notice's sizes are vScreen's own adoption line's",
                 "PASS (HEADSET) the headset notice was queued as a toast (09:30:12.300)",
                 "vr supersampling verdict: PASS (3 PASS, 0 WARN, 0 STOP, 0 n/a)"):
        if want not in flat:
            fail("the VR report lacks %r:\n%s" % (want, out))
    want_statuses_vr(adopt + queued, {"NOTICE": "STOP"}, "the adoption line and no notice (the detection did not run)")
    want_statuses_vr(sub(sub(vr, "rendered at 2112x2304", "rendered at 2816x3072"), "75% of the width", "100% of the width").replace(notice, ""), {"NOTICE": "n/a"}, "a world at the eye's own size")
    # The DLL's compare is exact on both axes; the percent in the log is rounded. 2751x3000 in 2816x3072 is 97.7% (logged as 98) and 97.7%: below.
    near = vr.replace("2112x2304", "2751x3000").replace("75% of", "98% of")
    want_statuses_vr(near, {"NOTICE": "PASS", "CONSISTENT": "PASS"}, "97.7 percent, logged as 98: below, since the DLL's compare is exact")
    # One axis under the threshold and the other not is no notice (the DLL needs both).
    one_axis = vr.replace("2112x2304", "2112x3070")
    want_statuses_vr(one_axis, {"NOTICE": "STOP"}, "a notice for a world under the eye on one axis only")
    want_statuses_vr(sub(vr, "scaled into the 2816x3072", "scaled into the 2800x3072"), {"CONSISTENT": "STOP"}, "sizes that disagree")
    want_statuses_vr(vr.replace(queued, ""), {"HEADSET": "WARN"}, "no menu line")
    want_statuses_vr(vr + "[09:31:00.000] flat runtime: treated=1 refused=0 last=treated-jittered\n", {"FLAT": "STOP"}, "a flat log with the notice")
    rc, out = report("[09:00:00.000] version v0.18.0 (build 1)\n", print_vr_supersampling)
    if rc != 1 or "no `vr supersampling:`" not in out:
        fail("a log with no VR line should exit 1 and say so: rc=%d %r" % (rc, out))
    return ok


UICOMP_FIXTURE = "ui_composites_fixture.log"


def self_test_ui_composites():
    """--ui-composites on the checked-in synthetic catalogue (tools\\ui_composites_fixture.log, which tools\\ui_composite_census_test holds to exactly what the
    DLL's formatter writes): every shape of the census line parsed, then logs picked from it by clock time to make each verdict, and logs altered to break the
    things the reader keys on. Returns ok."""
    import contextlib
    import io
    ok = True

    def fail(msg):
        nonlocal ok
        print("ui-composites: %s" % msg)
        ok = False

    path = os.path.join(repo_root(), "tools", UICOMP_FIXTURE)
    try:
        base = read_text(path)
    except OSError:
        fail("the fixture %s is missing" % path)
        return False
    lines = base.splitlines()
    p = parse_ui_composites(base)
    ws = p["windows"]
    if [w["state"] for w in ws] != ["none", "idle", "none", "left", "left", "left", "left", "off", "none", "none", "left"] or p["unparsed"] or p["layer_windows"] != 2:
        fail("the fixture's windows read as %s (%d unparsed, %d layer lines)" % ([w["state"] for w in ws], len(p["unparsed"]), p["layer_windows"]))
        return False
    d = ws[3]
    if (d["ts"], d["left"], d["seen"], d["rate"], d["frames"], d["live"]) != ("16:02:00.001", 56320, 61440, 22.0, 2560, 2560) or d["pairs"] != [
            {"vs": "1989E6D3B405FDE0", "ps": "EAB8A1C95A13FFBE", "family": "no family", "rate": 22.0}] or d["past"] is not None:
        fail("the defect window reads as %r" % d)
    n = ws[5]
    if n["pairs"] != [{"vs": "81216C77F90DEDD6", "ps": "A2965EC2931A39C8", "family": "cockpit holo panels", "rate": 0.01}]:
        fail("the named-family window reads as %r" % n["pairs"])
    o = ws[6]
    if len(o["pairs"]) != 8 or o["past"] != {"n": 4000, "cap": 8, "rate": 1.56} or o["left"] != 4160 or o["pairs"][0]["vs"] != "0000000000005000":
        fail("the overflow window reads as %r / %r" % (len(o["pairs"]), o["past"]))
    if (ws[7]["kind"], ws[7]["frames"]) != ("off", 2560) or ws[8]["live"] != 1280 or ws[1]["seen"] != 0:
        fail("the NOT COUNTED, half-live or idle windows read as %r / %r / %r" % (ws[7], ws[8]["live"], ws[1]["seen"]))
    # The kept pair: a loading screen with only the null-output quad (0 left of 16182, the quad in its own clause), and a defect with the quad beside it.
    k = ws[9]
    if (k["ts"], k["left"], k["seen"], k["rate"], k["state"], k["pairs"], k["past"]) != ("16:05:00.001", 0, 16182, 0.0, "none", [], None) or k["kept"] != [
            {"vs": "B018D143700AB803", "ps": "258B95AC99520C1F", "name": "null-output quad", "n": 5394, "rate": 2.0}]:
        fail("the kept-only window reads as %r" % k)
    b = ws[10]
    if (b["left"], b["seen"], b["state"], b["past"]) != (56320, 66560, "left", None) or b["pairs"] != [
            {"vs": "1989E6D3B405FDE0", "ps": "EAB8A1C95A13FFBE", "family": "no family", "rate": 22.0}] or b["kept"] != [
            {"vs": "B018D143700AB803", "ps": "258B95AC99520C1F", "name": "null-output quad", "n": 5120, "rate": 2.0}]:
        fail("the window with a pair left and the kept pair beside it reads as %r / %r" % (b["pairs"], b["kept"]))
    # The table of kept pairs here is the DLL's (src/d3d11/ui_scene_composites.h kUiSceneKeptPairs): the same pairs, the same names.
    try:
        header = read_text(os.path.join(repo_root(), "src", "d3d11", "ui_scene_composites.h"))
        entries = re.findall(r'\{0x([0-9A-F]{16})ull,\s*0x([0-9A-F]{16})ull,\s*"([^"]+)"\}', header)
        if {(a, b_): n for a, b_, n in entries} != UICOMP_KEPT_PAIRS or len(entries) != len(UICOMP_KEPT_PAIRS):
            fail("UICOMP_KEPT_PAIRS %r is not the DLL's kUiSceneKeptPairs %r" % (UICOMP_KEPT_PAIRS, entries))
        if set(UICOMP_KEPT_WHY) != set(UICOMP_KEPT_PAIRS.values()):
            fail("UICOMP_KEPT_WHY names %r, the kept pairs are named %r" % (sorted(UICOMP_KEPT_WHY), sorted(UICOMP_KEPT_PAIRS.values())))
    except OSError:
        fail("src\\d3d11\\ui_scene_composites.h is not readable from the repo root")

    def run(text):
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            rc = print_ui_composites(text)
        return rc, buf.getvalue()

    version = next(l for l in lines if "version v0.0.0-fixture" in l)

    def pick(*stamps):
        """The version line and the lines at these clock times (exactly 'hh:mm:ss.mmm')."""
        chosen = [l for l in lines if any(l.startswith("[" + s + "]") for s in stamps)]
        if len(chosen) != len(stamps):
            fail("pick %s found %d line(s)" % (stamps, len(chosen)))
        return "\n".join([version] + chosen) + "\n"

    def verdict(what, text, want_rc, want_verdict, *want_in):
        rc, out = run(text)
        if rc != want_rc or ("ui-composites verdict: " + want_verdict) not in out or any(w not in out for w in want_in):
            fail("%s: rc=%d, wanted %d %s mentioning %r:\n%s" % (what, rc, want_rc, want_verdict, want_in, out))
        return out

    layer1, layer2 = "16:00:30.000", "16:01:30.000"
    menu, idle, cockpit, defect1, defect2, named, overflow, off, half = ("16:00:30.001", "16:01:00.001", "16:01:30.001", "16:02:00.001", "16:02:30.001",
                                                                       "16:03:00.001", "16:03:30.001", "16:04:00.001", "16:04:30.001")
    # A healthy flight: a menu, nothing drawn, a cockpit with the pair taken -- every window a zero, and the zeros are said.
    healthy = pick(layer1, menu, idle, layer2, cockpit)
    verdict("a healthy flight", healthy, 0, "PASS", "PASS (INSTRUMENT) 3 window(s) of the census line: 3 counted, 0 NOT COUNTED",
            "PASS (TAKEN) every interface composite drawn into an eye went into the layer: 66840 composite draws in 3 window(s)", "0 of 61440 composite draws (0.00 a frame)")
    # The defect: the Disable-GUI-effects panels left in the scene, nothing names them (the field's 22 draws a frame) -- twice a window apart.
    out = verdict("the defect", healthy + pick(defect1, defect2).split("\n", 1)[1], 1, "STOP",
                  "STOP (UNCLAIMED) vs 1989E6D3B405FDE0 ps EAB8A1C95A13FFBE: left in the scene in 2 of 5 counted window(s), up to 22.00 draws a frame (mean 22.00), first at 16:02:00.001",
                  "no family names it")
    if "(TAKEN)" in out:
        fail("a flight with composites left says every composite went into the layer:\n%s" % out)
    # The same pair below the line a sustained composite is: a stray at a transition, a WARN.
    stray = pick(layer1, cockpit, defect1).replace("22.00 a frame", "0.40 a frame").replace("(22.00 a frame)", "(0.40 a frame)")
    verdict("a stray", stray, 0, "WARN", "WARN (UNCLAIMED)", "up to 0.40 draws a frame")
    # A named family left: a WARN that says where the reason is, never a STOP.
    verdict("a named family left", pick(layer2, cockpit, named), 0, "WARN", "WARN (NAMED) vs 81216C77F90DEDD6 ps A2965EC2931A39C8 (cockpit holo panels): named and not taken in 1 window(s)",
            "left in the game's frame")
    # More pairs than the table names: the overflow is a STOP at a sustained rate, and the eight named pairs are strays.
    verdict("overflow", pick(layer2, overflow), 1, "STOP", "STOP (OVERFLOW)", "WARN (UNCLAIMED) vs 0000000000005000 ps 0000000000006000", "up to 1.56 draws a frame unnamed")
    # The interface depth pass off: a window that says NOT COUNTED is not a zero.
    verdict("NOT COUNTED", pick(layer2, off), 0, "WARN", "WARN (DETECTOR) 1 window(s) say NOT COUNTED", "NOT COUNTED in 2560 frames")
    # No composite drawn into an eye at all: the zero does not show the pair taken.
    verdict("an idle window", pick(layer1, idle), 0, "WARN", "WARN (TAKEN) no composite was drawn into an eye in any counted window")
    # The layer live for half the window: said in the table, no verdict of its own.
    out = verdict("half live", pick(layer1, half), 0, "PASS", "0 of 30720 composite draws (0.00 a frame) in 2560 frames (1280 live)")
    # The null-output quad kept: a loading screen with only that pair is a PASS that says the pair, why it is no defect, and that its draws are not interface; never a
    # STOP and never a NAMED/UNCLAIMED finding. The 15:09 flight as the DLL now writes it.
    loading, mixed = "16:05:00.001", "16:05:30.001"
    out = verdict("a loading screen with the kept pair", pick(layer1, cockpit, loading), 0, "PASS",
                  "PASS (KEPT) vs B018D143700AB803 ps 258B95AC99520C1F (null-output quad): kept in the scene by design in 1 of 2 counted window(s), up to 2.00 draws a frame (5394 draws in all)",
                  "writes zero and reads nothing", "so the layer never takes it",
                  "PASS (TAKEN) every interface composite drawn into an eye went into the layer: 72228 composite draws in 2 window(s), none left in the scene (5394 more draws are the kept pair's, above, and are not interface)",
                  "0 of 16182 composite draws (0.00 a frame) in 2697 frames (2697 live)  none; B018D143700AB803/258B95AC99520C1F (null-output quad, kept by design) 5394 draws, 2.00")
    if "UNCLAIMED" in out or "NAMED" in out:
        fail("the kept pair is read as a finding:\n%s" % out)
    # ...and a real defect beside it is still a STOP: the kept pair hides nothing.
    verdict("the defect with the kept pair beside it", pick(layer1, mixed), 1, "STOP", "STOP (UNCLAIMED) vs 1989E6D3B405FDE0 ps EAB8A1C95A13FFBE: left in the scene in 1 of 1 counted window(s), up to 22.00 draws a frame",
            "PASS (KEPT) vs B018D143700AB803 ps 258B95AC99520C1F (null-output quad)")
    # Nothing but the kept pair drawn: the zero does not show a composite taken (a WARN, as an idle window is).
    only_kept_text = pick(layer1, loading).replace("0 of 16182 composite draws", "0 of 5394 composite draws")
    verdict("only the kept pair drawn", only_kept_text, 0, "WARN", "WARN (TAKEN) no composite was drawn into an eye in any counted window but the kept pair's (5394 draws, above)")
    # A build between 13c62cd6 and 069ebee4 has no kept clause: it counted the quad as a composite left with no family (the 15:10:50 window of the 15:09 flight, verbatim).
    legacy_line = "[15:10:50.165] ui quality: composites left in the scene: 5394 of 16182 composite draws (2.00 a frame) in 2697 frames (2697 live) -- vs B018D143700AB803 ps 258B95AC99520C1F (no family) 2.00 a frame."
    legacy = version + "\n" + legacy_line + "\n"
    out = verdict("an older build's line", legacy, 0, "PASS", "PASS (KEPT) vs B018D143700AB803 ps 258B95AC99520C1F (null-output quad): kept in the scene by design in 1 of 1 counted window(s)",
                  "(5394 draws in all)", "read here as kept", "PASS (TAKEN) every interface composite drawn into an eye went into the layer: 10788 composite draws in 1 window(s)")
    if "UNCLAIMED" in out:
        fail("an older build's null-output quad is still read as unclaimed:\n%s" % out)
    # ...but only that exact pair: one bit off, or with a family named, it is what it always was.
    verdict("an older line, one bit off", version + "\n" + legacy_line.replace("B018D143700AB803", "B018D143700AB802") + "\n", 1, "STOP", "STOP (UNCLAIMED) vs B018D143700AB802 ps 258B95AC99520C1F")
    verdict("an older line, ps one bit off", version + "\n" + legacy_line.replace("258B95AC99520C1F", "258B95AC99520C1E") + "\n", 1, "STOP", "STOP (UNCLAIMED) vs B018D143700AB803 ps 258B95AC99520C1E")
    verdict("an older line, a family named", version + "\n" + legacy_line.replace("(no family)", "(interface composite, not taken)") + "\n", 0, "WARN", "WARN (NAMED) vs B018D143700AB803 ps 258B95AC99520C1F (interface composite)")
    # Beside another pair an older line's kept draws are the rate over the frames, and the other pair is still said.
    mixed_legacy = version + "\n" + ("[15:10:50.165] ui quality: composites left in the scene: 61440 of 66560 composite draws (24.00 a frame) in 2560 frames (2560 live) -- "
                                     "vs 1989E6D3B405FDE0 ps EAB8A1C95A13FFBE (no family) 22.00 a frame; vs B018D143700AB803 ps 258B95AC99520C1F (no family) 2.00 a frame.") + "\n"
    verdict("an older line beside a defect", mixed_legacy, 1, "STOP", "STOP (UNCLAIMED) vs 1989E6D3B405FDE0 ps EAB8A1C95A13FFBE", "PASS (KEPT) vs B018D143700AB803 ps 258B95AC99520C1F (null-output quad)", "(5120 draws in all)")
    # The layer's own 30 s lines and no census line: a build before it, or a census that never ran. Exit 3 and a STOP line that says so.
    verdict("no census line", pick(layer1, layer2), 3, "STOP", "STOP (INSTRUMENT) the layer printed 2 30 s line(s) and not one `composites left in the scene` line")
    # No ui quality line at all: the key was off.
    verdict("the key off", version + "\n", 3, "n/a", "n/a (INSTRUMENT) no `ui quality:` 30 s line at all")
    # A line that starts with the prefix and is none of its shapes is said, and no window is made of it.
    drift = pick(layer1, defect1).replace("composite draws", "composite draw")
    out = verdict("a drifted line", drift, 3, "STOP", "WARN (SHAPE) 1 line(s) start with the census prefix and are none of its shapes")
    if parse_ui_composites(drift)["windows"]:
        fail("a drifted line made a window")
    # The reader's own shapes: the pair regex wants sixteen upper-case hex digits, and a hash that is not is not a pair.
    short = pick(layer1, defect1).replace("vs 1989E6D3B405FDE0", "vs 1989E6D3B405FDE")
    if parse_ui_composites(short)["windows"]:
        fail("a pair with a fifteen-digit hash made a window")
    return ok


def self_test_maps_sharp():
    """--maps-sharp on the checked-in synthetic flight (tools\\maps_sharp_fixture.log, which tools\\on_foot_maps_test holds to exactly what the
    DLL's formatters write), then on logs altered to remove each thing the verdict judges. Returns ok."""
    import contextlib
    import io
    ok = True

    def fail(msg):
        nonlocal ok
        print("maps-sharp: %s" % msg)
        ok = False

    path = os.path.join(repo_root(), "tools", "maps_sharp_fixture.log")
    try:
        base = read_text(path)
    except OSError:
        fail("the fixture %s is missing" % path)
        return False
    p = parse_maps_sharp(base)
    kinds = [e["kind"] for e in p["events"]]
    if kinds != ["on", "take", "notempty", "back", "take", "back", "take", "back", "off"]:
        fail("the fixture's events read as %s" % kinds)
    if len(p["windows"]) != 7 or p["unparsed"]:
        fail("the fixture has %d windows (want 7) and %d unparsed line(s)" % (len(p["windows"]), len(p["unparsed"])))
    on = p["events"][0] if p["events"] else {}
    if (on.get("frame"), on.get("start"), on.get("journal")) != (900, "the world", "on foot"):
        fail("the ON line reads as %r" % on)
    take = p["events"][1] if len(p["events"]) > 1 else {}
    if (take.get("frame"), take.get("run"), take.get("world"), take.get("secs"), take.get("journal")) != (27844, 3, 26944, 149.9, "on foot"):
        fail("the first TAKES line reads as %r" % take)
    back = p["events"][3] if len(p["events"]) > 3 else {}
    if (back.get("frame"), back.get("frames"), back.get("secs"), back.get("only"), back.get("kept"), back.get("why")) != (
            29443, 1599, 17.8, 3190, 2, "a world camera named the screen's source for 2 frames in a row"):
        fail("the first HANDS BACK line reads as %r" % back)
    ne = p["events"][2] if len(p["events"]) > 2 else {}
    if (ne.get("eye"), ne.get("seq"), ne.get("draws"), ne.get("taken")) != (0, 31200, 3, 2):
        fail("the not-empty line reads as %r" % ne)
    w = p["windows"][3] if len(p["windows"]) > 3 else {}
    if (w.get("gate"), w.get("frames"), w.get("unnamed"), w.get("panel"), w.get("takes"), w.get("recognised"), w.get("only"), w.get("notempty")) != (
            "panel", 450, 450, 450, 900, 900, 898, 2):
        fail("the fourth window reads as %r" % w)
    if [r["kind"] for r in p["route"]] != ["released", "owns"]:
        fail("the fixture's route lines read as %s" % [r["kind"] for r in p["route"]])
    eps = maps_sharp_episodes(p)
    if len(eps) != 3 or [e["frames"] for e in eps] != [1599, 4, 900]:
        fail("the fixture's panel periods read as %s" % [e.get("frames") for e in eps])
    elif eps[0]["route_released"] is None or abs(eps[0]["route_released"]["t"] - eps[0]["take"]["t"]) > 1e-6 or \
            eps[0]["route_owns"] is None or abs((eps[0]["route_owns"]["t"] - eps[0]["end"]["t"]) - 0.144) > 1e-6 or eps[1]["route_released"] is not None:
        fail("the first panel period's route pairing: released %r, owns %r" % (eps[0]["route_released"], eps[0]["route_owns"]))

    def run(text):
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            rc = print_maps_sharp(text)
        return rc, buf.getvalue()

    # The fixture as it is: a 4-frame panel period is a flap (STOP), the not-empty eyes are a WARN, and the report says all of it.
    rc, out = run(base)
    if rc != 1 or "maps-sharp verdict: STOP" not in out or "a flap" not in out or "door-not-empty" not in out or "VR world route let go at" not in out:
        fail("the fixture's report (rc=%d) lacks its flap STOP, its not-empty WARN or its route pairing:\n%s" % (rc, out))
    if "the route owned the world again at 16:23:00.126 (144 ms after the hand-back)" not in out:
        fail("the report does not say the route owned again 144 ms after the first hand-back:\n%s" % out)

    # A clean flight: the flap, the not-empty eyes and their counters removed.
    clean_lines = []
    for raw in base.splitlines():
        if "16:23:20.000" in raw[:14] or "16:23:20.050" in raw[:14] or "the layer took the 2D screen for eye" in raw:
            continue
        raw = raw.replace("door-layer-only=898 door-not-empty=2", "door-layer-only=900 door-not-empty=0")
        raw = raw.replace("2 kept the upscaler", "0 kept the upscaler")
        clean_lines.append(raw)
    clean = "\n".join(clean_lines) + "\n"
    rc, out = run(clean)
    if rc != 0 or "maps-sharp verdict: PASS" not in out:
        fail("the clean flight does not PASS (rc=%d):\n%s" % (rc, out))

    def altered(what, text, want_rc, want_verdict, want_in_out):
        rc2, out2 = run(text)
        if rc2 != want_rc or ("maps-sharp verdict: " + want_verdict) not in out2 or want_in_out not in out2:
            fail("%s: rc=%d, wanted %d %s mentioning %r:\n%s" % (what, rc2, want_rc, want_verdict, want_in_out, out2))

    # The trap: taken composites that were never recognised.
    altered("recognised=0 with screen-takes>0", clean.replace("screen-takes=810 recognised=810", "screen-takes=810 recognised=0"), 1, "STOP",
            "none recognised")
    # A black eye is the sharpen door's own failure line, and only that.
    altered("a black eye", clean + "[16:23:41.000] native sharpen: LAYER-ONLY eye 0 (sequence 5) got NO composite from the UI layer -- the eye "
                                   "is BLACK for this frame (1 so far); the layer stands down\n", 1, "STOP", "a black eye was reported")
    # The luma probe's lines are not black-eye evidence. Real flights carry them by the dozen: a sample line every 2 s, and a
    # "first black stage is X" line at every CHANGE of the first black stage ("is none" when a black arrival gives way to a world, "is
    # game" for as long as the layer holds the screen). This test once demanded a STOP for "is game", and every real flight stopped.
    luma_real = ("[16:23:41.000] luma probe: eye=0 game=0.000/0.000/100% dlss_out=0.000/0.000/100% final=0.000/0.000/100%\n"
                 "[16:23:41.001] luma probe: eye=0 first black stage is game (game 0.000 dlss_out 0.000 final 0.000).\n"
                 "[16:23:43.000] luma probe: eye=0 game=0.226/0.824/0% dlss_out=0.226/0.820/0% final=0.226/0.820/0%\n"
                 "[16:23:43.001] luma probe: eye=0 first black stage is none (game 0.226 dlss_out 0.226 final 0.226).\n")
    altered("the luma probe's lines outside any panel period", clean + luma_real, 0, "PASS", "(0 STOP, 0 WARN)")
    if parse_maps_sharp(clean + luma_real)["black"]:
        fail("the luma probe's transition lines were read as black eyes")
    # The same black sample inside a panel period the journal calls on foot: a map or a menu was on screen, so look.
    altered("a black final stage under an on-foot map",
            clean.replace("[16:22:50.100]", "[16:22:50.000] luma probe: eye=0 game=0.000/0.000/100% dlss_out=0.000/0.000/100% final=0.000/0.000/100%\n"
                          "[16:22:50.100]", 1), 0, "WARN", "calls on foot")
    # The journal's reading with parentheses of its own (every arrival, before Status.json exists): the ON and TAKES lines must still
    # parse, with the whole reading as the journal (a regex that stopped at the first ")" lost both lines as "unknown").
    nested = base.replace("the journal: on foot)", "the journal: no Flags2 in Status.json (a menu, or no file yet))")
    pn = parse_maps_sharp(nested)
    jn = "no Flags2 in Status.json (a menu, or no file yet)"
    if [e["kind"] for e in pn["events"]] != kinds or pn["unparsed"] or len(pn["events"]) < 2 or \
            (pn["events"][0].get("journal"), pn["events"][1].get("journal"), pn["events"][1].get("world")) != (jn, jn, 26944):
        fail("with parentheses inside the journal's reading the events read as %s (%d unparsed), journals %r" % (
            [e["kind"] for e in pn["events"]], len(pn["unparsed"]), [e.get("journal") for e in pn["events"][:2]]))
    # The arrival with the key on from launch: the gate starts as a panel (the main menu, the load), so there is no TAKES line and the
    # period begins at the ON line. It must be a period, paired with its HANDS BACK and the route owning again, with the black luma
    # sample a note (the journal had no Flags2) and not a WARN or a STOP.
    base_lines = base.splitlines()

    def first_line(pred):
        return next(l for l in base_lines if pred(l))

    arrival = "\n".join([
        first_line(lambda l: "version v0.0.0-fixture" in l),
        first_line(lambda l: "ON at frame=" in l).replace("left it: the world (the journal: on foot).",
                                                         "left it: not the world (the journal: no Flags2 in Status.json (a menu, or no file yet))."),
        first_line(lambda l: "5s:" in l and "gate=panel" in l),
        "[16:22:50.000] luma probe: eye=0 game=0.000/0.000/100% dlss_out=0.000/0.000/100% final=0.000/0.000/100%",
        first_line(lambda l: "HANDS BACK" in l).replace("2 kept the upscaler", "0 kept the upscaler"),
        first_line(lambda l: "vr world route: OWNS" in l),
    ]) + "\n"
    ea = maps_sharp_episodes(parse_maps_sharp(arrival))
    if len(ea) != 1 or not ea[0]["take"].get("from_on") or ea[0]["frames"] != 1599 or ea[0]["route_released"] is not None or \
            ea[0]["route_owns"] is None or (ea[0]["luma_samples"], ea[0]["luma_black"]) != (1, 1):
        fail("the arrival period (a gate that starts as a panel) reads as %r" % (ea,))
    rc, out = run(arrival)
    if rc != 0 or "maps-sharp verdict: PASS" not in out or "(0 STOP, 0 WARN)" not in out or \
            "ON frame 900, the screen already a panel (the journal: no Flags2 in Status.json (a menu, or no file yet))" not in out or \
            "black by content" not in out or "the route owned the world again" not in out:
        fail("the arrival report (rc=%d):\n%s" % (rc, out))
    # The route letting go apart from the gate.
    altered("the route apart", clean.replace("[16:22:41.915] vr world route: RELEASED", "[16:22:42.300] vr world route: RELEASED"), 0, "WARN",
            "not the same boundary")
    # The door short of its eyes without a counted reason.
    altered("the door short", clean.replace("panel-frames=450 holds=0 releases=0 screen-takes=900 recognised=900 door-layer-only=900",
                                            "panel-frames=450 holds=0 releases=0 screen-takes=900 recognised=900 door-layer-only=300"), 0, "WARN",
            "only 300 eyes through the layer-only door")
    # The key on but the gate not decided by naming.
    altered("not live frames", clean.replace("not-live-frames=0", "not-live-frames=12", 1), 0, "WARN", "not decided by naming")
    # A declined layer-only eye.
    altered("a declined door", clean + "[16:23:41.000] native temporal: layer-only declined for eye 0 (sequence 7): the UI layer is not live; the "
                                       "eye route serves this eye through the pass, as it does without the layer-only door.\n", 0, "WARN",
            "declined an eye")
    # No panel period closed.
    altered("nothing handed back", "\n".join(l for l in clean.splitlines() if "HANDS BACK" not in l) + "\n", 0, "WARN", "no panel period closed")
    # A log with no line of the feature says so, and exits 3.
    rc, out = run("[16:20:00.000] version v0.0.0 (build 1) -- this DLL was linked x\n[16:20:01.000] something else\n")
    if rc != 3 or "no 'on foot maps sharp' line" not in out:
        fail("a log with no feature line (rc=%d):\n%s" % (rc, out))
    # A line of the feature the reader does not know is reported, not dropped, and the first one is quoted so the next defect is not a hunt.
    altered("an unknown line", clean + "[16:23:41.000] on foot maps sharp: something the formatters never wrote\n", 0, "WARN",
            "the first: [16:23:41.000] on foot maps sharp: something the formatters never wrote")

    # Every reason the DLL can give for the gate stopping or standing aside (src\\d3d11\\ui_layer_math.h uiLayerNotLiveReasonFor; ui_layer.cpp mapsGate and
    # mapsLayerNotLive), in the OFF line's "(why)" and the not-live line's last words. Three carry parentheses of their own, and the OFF pattern stopped
    # at the first ")": a real flight (edvr_gfx_20261001_085519.log, fix.temporal_aa off) lost its OFF line as "unknown". The list is held to the DLL's
    # sources, so a reason that is reworded fails here and not in the ten minutes after a flight.
    reasons = ["the key went off", "screen motion is not live", "fix.ui_quality is off", "no temporal mode is on (fix.temporal_aa is off)",
               "the eye jitter is not as shipped", "the layer stood down for the session",
               "screen motion is not live (fix.temporal_aa is off, or it stood down)", "the UI layer is not live"]
    reason_src = ""
    for rel in ("src/d3d11/ui_layer_math.h", "src/d3d11/ui_layer.cpp"):
        try:
            reason_src += read_text(os.path.join(repo_root(), *rel.split("/")))
        except OSError:
            fail("%s is missing: the reasons below cannot be held to the DLL's sources" % rel)
    for why in reasons:
        if reason_src and '"%s"' % why not in reason_src:
            fail("the reason %r is not in the DLL's sources any more: reword it here too" % why)
        pr = parse_maps_sharp(
            "[16:23:45.000] on foot maps sharp: OFF at frame=32500 (%s): the 2D screen is the world by the journal's reading or the screen's own "
            "depth again, as without the key.\n"
            "[16:23:46.000] on foot maps sharp: the maps gate is on but the 2D screen's gate stays the journal's and the screen's "
            "own depth, as without the key: %s.\n" % (why, why))
        got = [(e["kind"], e.get("why")) for e in pr["events"]]
        if got != [("off", why), ("notlive", why)] or pr["unparsed"]:
            fail("a gate-stopped line with the reason %r reads as %r (%d unparsed)" % (why, got, len(pr["unparsed"])))

    # The first flight (design doc 8.10: edvr_gfx_20261001_082459.log, Frontier, df9172db). A map was handed back with the route owning again;
    # later the player BOARDED his ship: the gate released and the route let go on one boundary (TAKES and RELEASED at one stamp), the game
    # stopped drawing the 2D screen (a cockpit has no screen composite), and every window after the take read gate=panel with screen-takes=0.
    # The reader of that build judged the door against PANEL FRAMES and called the cockpit six failed doors, and the journal's reading at the
    # take (stale by 1.6 s: still "on foot") turned the game's exit fade into a luma WARN. These are that log's lines; the TAKES, RELEASED and
    # ON lines are the fixture's (formatter) text restamped. The old format has no screen-draws; the new one does.
    def restamp(line, ts):
        return "[%s]%s\n" % (ts, line[line.index("]") + 1:])

    def win(ts, gate, frames, named, world, panel, takes, rel, draws=None):
        return ("[%s] on foot maps sharp 5s: key=on 5 s mode=naming gate=%s frames=%d named=%d unnamed=%d world-frames=%d panel-frames=%d holds=0 "
                "releases=%d screen-takes=%d recognised=%d door-layer-only=%d door-not-empty=0 not-live-frames=0%s\n"
                % (ts, gate, frames, named, frames - named, world, panel, rel, takes, takes, takes,
                   "" if draws is None else " screen-draws=%d" % draws))

    def flight(new_format, cockpit_draws=0):
        d = (lambda n: n) if new_format else (lambda n: None)
        cockpit = [("08:33:03.156", 428), ("08:33:08.155", 450), ("08:33:13.156", 428), ("08:33:18.159", 429), ("08:33:23.160", 364)]
        return "".join([
            "[08:24:59.501] version v0.18.0-rc.5-37-gdf9172db (build 6ABE6A99) -- this DLL was linked 2026-10-01 14:13:45 UTC\n",
            restamp(first_line(lambda l: "ON at frame=" in l), "08:31:23.144"),
            # period 1: a map, handed back clean, the route owning the world again 163 ms later
            restamp(first_line(lambda l: "the layer TAKES" in l), "08:31:23.166"),
            win("08:31:28.150", "panel", 450, 0, 0, 450, 900, 0, d(900)),
            win("08:31:33.150", "panel", 450, 0, 0, 450, 900, 0, d(900)),
            win("08:31:38.150", "panel", 450, 0, 0, 450, 900, 0, d(900)),
            restamp(first_line(lambda l: "HANDS BACK" in l).replace("2 kept the upscaler", "0 kept the upscaler"), "08:31:44.796"),
            restamp(first_line(lambda l: "vr world route: OWNS" in l), "08:31:44.959"),
            win("08:32:48.156", "world", 448, 448, 448, 0, 0, 0, d(896)),
            win("08:32:53.159", "world", 451, 451, 451, 0, 0, 0, d(902)),
            restamp(first_line(lambda l: "the layer TAKES" in l), "08:32:55.975"),
            restamp(first_line(lambda l: "vr world route: RELEASED" in l), "08:32:55.975"),
            win("08:32:58.157", "panel", 375, 211, 213, 162, 0, 1, d(426)),
            "".join(win(ts, "panel", n, 0, 0, n, 0, 0, d(cockpit_draws)) for ts, n in cockpit),
            "[08:33:22.312] luma probe: eye=0 game=0.000/0.000/100% dlss_out=0.000/0.000/100% final=0.000/0.000/100%\n",
            "[08:33:22.312] luma probe: eye=0 first black stage is game (game 0.000 dlss_out 0.000 final 0.000).\n",
            "[08:33:25.110] native sharpen totals: treated=79454, off=0, refusals=0, invalidations=8, stood_down=0, layer_only=14114, layer_only_black=0.\n",
        ])

    rc, out = run(flight(False))
    if rc != 0 or "(0 STOP, 0 WARN)" not in out or "layer-only door (expected" in out or \
            "the layer took no 2D screen composite in this panel period's 5 whole-panel window(s)" not in out or "not the layer's picture" not in out:
        fail("the boarding flight, 5 s line without screen-draws (rc=%d): a cockpit with nothing taken must not read as a failed door or a black eye:\n%s"
             % (rc, out))
    rc, out = run(flight(True))
    if rc != 0 or "(0 STOP, 0 WARN)" not in out or "layer-only door (expected" in out or \
            "no 2D screen composite was drawn in this panel period's 5 whole-panel window(s)" not in out or \
            "drawn 0, taken 0 -- none was drawn" not in out or "the decision saw" not in out:
        fail("the boarding flight, 5 s line with screen-draws (rc=%d): the cockpit's windows say none was drawn:\n%s" % (rc, out))
    # Composites that WERE drawn while the gate gave the layer the whole window and that the layer did not take: that is the failure the old
    # reading imagined, and the new line can tell it from the cockpit.
    rc, out = run(flight(True, cockpit_draws=40))
    if rc != 0 or "maps-sharp verdict: WARN" not in out or "the layer left 40 in the game's frame" not in out:
        fail("composites drawn but not taken in a whole-panel window (rc=%d) must WARN, naming the count:\n%s" % (rc, out))
    return ok


VSCREEN_FIXTURE = "vscreen_fit_fixture.log"


def self_test_vscreen_fit():
    """--vscreen-fit on the checked-in synthetic flight (tools\\vscreen_fit_fixture.log, which tools\\vscreen_fit_test holds to
    exactly what the DLL's formatters write), then on logs altered to take away each thing the report depends on and to break each thing
    the verdict judges, and the census verdict's size read from the log. Returns ok."""
    import contextlib
    import io
    ok = True

    def fail(msg):
        nonlocal ok
        print("vscreen fit: %s" % msg)
        ok = False

    def report(text):
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            rc = print_vscreen_fit(text)
        return rc, buf.getvalue()

    def sub(text, old, new, count=-1):
        """text with `old` replaced by `new`; fails the test (and returns text) when `old` is not there: a mutation that changes
        nothing would pass every check for the wrong reason."""
        if old not in text:
            fail("the mutation %r -> %r found nothing to change" % (old, new))
            return text
        return text.replace(old, new, count)

    def statuses(text):
        """{tag: status} of the verdict lines of a log."""
        _, out = report(text)
        found = {}
        for line in out.splitlines():
            m = re.match(r"^(PASS|WARN|STOP|n/a) \(([A-Z ]+)\) ", line)
            if m:
                found[m.group(2)] = m.group(1)
        return found, out

    here = os.path.dirname(os.path.abspath(__file__))
    fixture = os.path.join(here, VSCREEN_FIXTURE)
    if not os.path.isfile(fixture):
        fail("the fixture %s is missing beside this script" % VSCREEN_FIXTURE)
        return False
    text = read_text(fixture)

    # ---- this copy of the rule's constants is the header's ----
    header = os.path.join(os.path.dirname(here), "src", "common", "vscreen_fit.h")
    if os.path.isfile(header):
        h = read_text(header)
        for name, value in (("kMultiplier", VSCREEN_M), ("kFloorWidth", VSCREEN_FLOOR), ("kLegacyMultiplier", VSCREEN_LEGACY_M),
                            ("kChosenWidthPx", VSCREEN_CHOSEN), ("kSeedFootprintPx", VSCREEN_SEED[0]), ("kSeedDistance", VSCREEN_SEED[1]),
                            ("kSeedEyeWidthPx", VSCREEN_SEED[2]), ("kFootprintQuantile", VSCREEN_QUANTILE)):
            m = re.search(r"constexpr (?:double|uint32_t) %s = ([0-9.]+);" % name, h)
            if not m or abs(float(m.group(1)) - value) > 1e-9:
                fail("this reader's constant for %s (%r) is not src\\common\\vscreen_fit.h's (%r)" % (name, value, m.group(1) if m else None))
    else:
        fail("src\\common\\vscreen_fit.h is not where the self-test looks for it (%s)" % header)
    if abs(VSCREEN_M * VSCREEN_SEED[0] - VSCREEN_CHOSEN) > 1.0 or _vround16(VSCREEN_M * VSCREEN_SEED[0]) != int(VSCREEN_CHOSEN):
        fail("this reader's m (%r) x seed footprint (%r) is not the chosen width (%r): the three move together" % (VSCREEN_M, VSCREEN_SEED[0], VSCREEN_CHOSEN))

    # ---- the parser ----
    f = parse_vscreen_fit(text)
    kv = f["rule"]["kv"] if f["rule"] else {}
    if not f["rule"] or f["rule"]["width"] != 3504 or kv.get("rule") != "fitted" or kv.get("source") != "seed" or kv.get("route") != "run" or \
            kv.get("eye") != "4032" or kv.get("distance") != "0.700" or kv.get("legacy") != "5040" or kv.get("footprint") != "5006" or \
            kv.get("m") != "0.700" or kv.get("floor") != "2880" or kv.get("cap") != "5040" or kv.get("clamp") != "none":
        fail("the fixture's rule line parsed as %r" % (f["rule"],))
    if not f["applied"] or (f["applied"]["w"], f["applied"]["h"], f["applied"]["sites"]) != (3504, 1971, 6) or len(f["armed"]) != 1 or \
            f["armed"][0][1] != "armed" or len(f["windows"]) != 4:
        fail("the fixture parsed as applied %r, %d arming line(s), %d window(s)" % (f["applied"], len(f["armed"]), len(f["windows"])))
    w = vscreen_fit_windows(f)
    if len(w) != 4 or (w[0]["samples"], w[0]["on_foot"], w[0]["other"], w[0]["draws"], w[0]["fp"], w[0]["other_fp"], w[0]["applied"]) != (44, 0, 44, 5280, None, 5640.0, None) or \
            (w[1]["fp"], w[1]["lo"], w[1]["hi"], w[1]["shape"], w[1]["at1"], w[1]["session_frac1"], w[1]["persisted"], w[1]["fit"], w[1]["legacy"], w[1]["eye"]) != \
            (5262.0, 5011.0, 5890.0, 1.71, 3683.0, 0.8694, 0.8694, 3504.0, 5040.0, (4032, 3898)) or w[3]["session_n"] != 177 or w[3]["session_frac1"] != 0.8691 or \
            w[3]["persisted"] != 0.8694:
        fail("the fixture's windows parsed as %r" % (w,))
    if f["fov"] is not None:
        fail("the fixture carries no FOV line, but the parser read one: %r" % (f["fov"],))
    # A line cut short or garbled is skipped, never fatal.
    g = parse_vscreen_fit("vscreen footprint 30s: window=x samples=y on-foot=\nvScreen resolution: auto = 12 wide\n"
                          "vScreen resolution: 1920x1080 -> 3504x1971 at\nnothing at all\nvscreen footprint: armed -- x\n")
    if g["rule"] is not None or g["applied"] is not None or len(g["armed"]) != 1 or len(g["windows"]) != 1:
        fail("a cut-short line was mis-parsed: %r" % (g,))
    # The eyes' FOV is read from the graphics log's benchmark line (angles in radians, order left/right/up/down): the first real one.
    fov_line = ("[06:07:04.100] native benchmark workload: window 1, feature epoch 0, treatments 14/14, game FOV radians L -0.89775/0.71858/0.79986/-0.79986 "
                "R -0.71858/0.89775/0.79986/-0.79986; order left/right/up/down; input is submitted ROI, output is active XR target.")
    flat_zero = ("[06:07:04.000] native benchmark workload: window 1, feature epoch 0, treatments 0/0, game FOV radians L 0.00000/0.00000/0.00000/0.00000 "
                 "R 0.00000/0.00000/0.00000/0.00000; order left/right/up/down; input is submitted ROI, output is active XR target.")
    want_fov = (-0.89775, 0.71858, 0.79986, -0.79986)
    if parse_vscreen_fit(fov_line)["fov"] != want_fov or parse_vscreen_fit(flat_zero + "\n" + fov_line)["fov"] != want_fov or \
            parse_vscreen_fit(flat_zero)["fov"] is not None:
        fail("the FOV was not read from the benchmark line (or a flat log's zeros were taken for one): %r" % (parse_vscreen_fit(fov_line)["fov"],))

    # ---- the report on the fixture: every question PASSes, the one the log cannot answer says so ----
    rc, out = report(text)
    flat = re.sub(r"[ ]+", " ", out)
    for want in (
            "vscreen fit: the rule line, an apply line, 1 arming line(s), 4 footprint window(s)",
            "launch: auto = 3504 wide, rule=fitted source=seed route=run eye=4032 distance=0.700 footprint=5006 clamp=none legacy=5040; applied 3504x1971",
            "window 1: samples 44 (on foot 0, other 44), draws 5280, skipped 0, late 0; eye 4032x3898",
            "window 4: samples 59 (on foot 59, other 0), draws 5400, skipped 0, late 0; eye 4032x3898 distance 0.7 applied 0.7; on foot fp=5270",
            "PASS (RULE) FITTED to 3504 wide from a seed footprint of 5006 px at fix.panel_distance 0.700 on a 4032 px eye (m=0.700, floor 2880, cap 5040, "
            "clamp none); legacy would have been 5040; applied 3504x1971 at 6 site(s)",
            "PASS (INSTRUMENT) 4 window(s): 221 sample(s) (177 on foot, 44 other), 21480 composite draw(s) seen, 0 skipped, 0 late",
            "PASS (ON FOOT) 3 on-foot window(s), 177 sample(s); the screen spans 5270 px (median of the windows' medians 5262..5281) of the eye",
            "PASS (STABLE) the screen's width held: 0.4% between 3 windows (3683..3697 px at distance 1; under 8%)",
            "PASS (SHAPE) the median of the windows' footprint shapes reads 1.700 against 16:9 = 1.778 (square eye pixels assumed: this log carries no per-eye FOV line): -4.4%",
            "n/a (DISTANCE LAW) one panel distance in this log (0.70)",
            "PASS (CALIBRATION) at Sean's calibration point (a 4032 px eye) the session's stored head-on floor (p10) is 5006 px at distance 0.7 against the 5006 px his 3504 is "
            "fitted from (100.0%, within 5%): m = 0.70 fits 3504 wide there",
            "PASS (STORED) the on-foot head-on floor (p10) is stored: 0.8694 of the eye at panel distance 1 (177 sample(s)); the next launch fits 3504 wide, the width this launch ran at (3504)",
            "vscreen fit verdict: PASS (7 PASS, 0 WARN, 0 STOP, 1 n/a)"):
        if want not in flat:
            fail("the fixture's report lacks %r:\n%s" % (want, out))
    if rc != 0:
        fail("the fixture reported exit %d" % rc)

    # ---- the rule line, taken apart ----
    legacy_line = ("[06:07:03.620] vScreen resolution: auto = 5040 wide: rule=legacy source=none route=no eye=4032 distance=0.700 legacy=5040 m=1.25 -- LEGACY: "
                   "125% of the 4032 px the runtime last rendered per eye, because the world route will not run: experimental.temporal_aa_on_foot_world is "
                   "not auto. Without the route nothing anti-aliases the on-foot world before it reaches the panel, and the extra "
                   "width does that job.")
    rule_line = next(l for l in text.splitlines() if "vScreen resolution: auto = " in l)
    legacy = sub(sub(text, rule_line, legacy_line), "-> 3504x1971", "-> 5040x2835")
    st, out = statuses(legacy)
    if st.get("RULE") != "PASS" or "LEGACY 5040 wide (125% of the 4032 px eye): the world route will not run: experimental.temporal_aa_on_foot_world is not auto" not in re.sub(r"[ ]+", " ", out):
        fail("a legacy launch did not say which condition failed: %r\n%s" % (st, out))
    if st.get("STORED") != "PASS" or "this launch's route=no (legacy), so a launch fits only once the world route will run" not in out:
        fail("a legacy launch's stored footprint did not say the next launch fits only when the route will run: %r\n%s" % (st, out))
    st, out = statuses(sub(legacy, "auto = 5040 wide", "auto = 4800 wide"))
    if st.get("RULE") != "STOP":
        fail("a legacy width that is not 125%% of the eye did not STOP: %r" % (st,))
    st, out = statuses(sub(text, "-> 3504x1971", "-> 5040x2835"))
    if st.get("RULE") != "STOP" or "the panel patch applied 5040x2835, not 3504 wide" not in out:
        fail("a width that was not the one applied did not STOP: %r\n%s" % (st, out))
    st, out = statuses(sub(text, "auto = 3504 wide", "auto = 3600 wide"))
    if st.get("RULE") != "STOP" or "its own tokens" not in out:
        fail("a width that is not what its own tokens give did not STOP: %r\n%s" % (st, out))
    st, out = statuses(sub(text, "footprint=5006 m=0.700", "footprint=5006 m=0.650"))
    if st.get("RULE") != "STOP" or "m 0.650" not in out:
        fail("a different m in the tokens did not change the recomputed width (and print at three decimals): %r\n%s" % (st, out))
    # The footprint prints rounded to a pixel, so a printed width one step (16) from the recomputed one is rounding, and two are not.
    one_step = lambda t: sub(sub(t, "auto = 3504 wide", "auto = 3520 wide"), "-> 3504x1971", "-> 3520x1980")
    st, out = statuses(one_step(text))
    if st.get("RULE") != "PASS":
        fail("a width one step (16) from the one its own tokens give was refused: that is the rounding of the printed footprint: %r\n%s" % (st, out))
    st, out = statuses(sub(sub(text, "auto = 3504 wide", "auto = 3536 wide"), "-> 3504x1971", "-> 3536x1989"))
    if st.get("RULE") != "STOP" or "its own tokens" not in out:
        fail("a width two steps (32) from the one its own tokens give was accepted: %r\n%s" % (st, out))
    # A nudge off another target's size moves a width by several steps: allowed only when the line says nudged=yes, and only up to eight.
    nudged = lambda t, w, h: sub(sub(sub(t, "clamp=none", "clamp=none nudged=yes"), "auto = 3504 wide", "auto = %d wide" % w), "-> 3504x1971", "-> %dx%d" % (w, h))
    st, out = statuses(nudged(text, 3584, 2016))
    if st.get("RULE") != "PASS":
        fail("a nudged width within a few steps of the recomputed one was refused: %r\n%s" % (st, out))
    st, out = statuses(sub(sub(text, "auto = 3504 wide", "auto = 3584 wide"), "-> 3504x1971", "-> 3584x2016"))
    if st.get("RULE") != "STOP":
        fail("a width five steps from the recomputed one passed without the nudged=yes that would explain it: %r" % (st,))
    st, out = statuses(nudged(text, 3664, 2061))
    if st.get("RULE") != "STOP":
        fail("a nudged width ten steps from the recomputed one passed: %r" % (st,))
    st, out = statuses("\n".join(l for l in text.splitlines() if "-> 3504x1971" not in l))
    if st.get("RULE") != "PASS" or "no `vScreen resolution: ... ->` apply line" not in out:
        fail("a log with no apply line did not say so: %r" % (st,))
    explicit = "[06:07:03.620] vScreen resolution: explicit 3504 wide (fix.vscreen_res_width), 3504x1971 at 16:9, used exactly: auto's rule is not in play."
    not_armed = ("[06:07:05.412] vscreen footprint: not armed -- fix.vscreen_res_width is \"3504\", not auto: an explicit width is used exactly and there is "
                 "nothing to fit.")
    ex = "\n".join([l for l in text.splitlines() if "version" in l] + [explicit, not_armed])
    st, out = statuses(ex)
    if st.get("RULE") != "n/a" or st.get("INSTRUMENT") != "n/a" or report(ex)[0] != 0:
        fail("an explicit width did not read n/a for the rule and the instrument: %r" % (st,))
    fresh = "[06:07:03.620] vScreen resolution: auto, but no per-eye render width is on record yet for this install -- the on-foot screen stays at the game's own 1920x1080"
    st, out = statuses(fresh)
    if st.get("RULE") != "n/a" or "fresh install" not in out:
        fail("an auto with no eye width on record did not say so: %r" % (st,))

    # ---- the instrument ----
    nothing = "\n".join(l for l in text.splitlines() if not re.match(r"^\[[0-9:.]+\] vscreen footprint", l))
    st, out = statuses(nothing)
    if st.get("INSTRUMENT") != "STOP" or "never ran" not in out:
        fail("a launch with no footprint line at all did not STOP the instrument: %r\n%s" % (st, out))
    armed_only = "\n".join(l for l in text.splitlines() if not re.match(r"^\[[0-9:.]+\] vscreen footprint 30s:", l))
    st, out = statuses(armed_only)
    if st.get("INSTRUMENT") != "WARN" or "shorter than 30 s" not in out:
        fail("an armed instrument with no closed window did not WARN: %r" % (st,))
    st, out = statuses(sub(text, "draws=5400", "draws=0").replace("draws=5280", "draws=0"))
    if st.get("INSTRUMENT") != "STOP" or "never seen" not in out:
        fail("draws=0 in every window did not STOP: %r\n%s" % (st, out))
    st, out = statuses(re.sub(r"samples=\d+ on-foot=\d+ other=\d+", "samples=0 on-foot=0 other=0", text))
    if st.get("INSTRUMENT") != "STOP" or "not one sample" not in out:
        fail("a composite seen but not one sample read did not STOP: %r\n%s" % (st, out))
    st, out = statuses(sub(text, "window=2 samples=60 on-foot=60 other=0 skipped=0 late=0", "window=2 samples=60 on-foot=60 other=0 skipped=2 late=0 why=vb0-stride:2"))
    if st.get("INSTRUMENT") != "PASS" or "2 skipped (vb0-stride x2)" not in out:
        fail("skipped samples were not counted by reason: %r\n%s" % (st, out))
    st, out = statuses(sub(text, "window=2 samples=60 on-foot=60 other=0 skipped=0 late=0", "window=2 samples=60 on-foot=60 other=0 skipped=0 late=3"))
    if st.get("INSTRUMENT") != "WARN" or "not ready in time" not in out:
        fail("late copies did not WARN: %r" % (st,))

    # ---- on foot, stable, shape, the distance law, the calibration point, what is stored ----
    menu_only = "\n".join(l for l in text.splitlines() if "window=2 " not in l and "window=3 " not in l and "window=4 " not in l)
    st, out = statuses(menu_only)
    if st.get("ON FOOT") != "WARN" or st.get("STORED") != "n/a" or "menu only (5640 px)" not in out:
        fail("a menu-only session did not WARN on foot and store nothing: %r\n%s" % (st, out))
    # STABLE: the windows' widths at distance 1 (fp x applied), (max - min) / median, against 8%. What is inside a window is not judged.
    st, out = statuses(sub(text, "fp=5281 frac=1.3098 range=5004..6044", "fp=5800 frac=1.4385 range=5004..6044"))
    if st.get("STABLE") != "WARN" or "the screen's width moved" not in out or "10.2% between 3 windows" not in out:
        fail("a window 10%% wider than the others did not WARN stable: %r\n%s" % (st, out))
    st, out = statuses(sub(text, "fp=5281 frac=1.3098 range=5004..6044", "fp=5500 frac=1.3641 range=5004..6044"))
    if st.get("STABLE") != "PASS" or "4.5% between 3 windows" not in out:
        fail("a window 4%% wider than the others (a head turning) did not hold: %r\n%s" % (st, out))
    st, out = statuses(sub(text, "fp=5281 frac=1.3098 range=5004..6044", "fp=5281 frac=1.3098 range=3300..9000"))
    if st.get("STABLE") != "PASS":
        fail("a wide range inside one window was judged unstable (it is the head moving, not the screen): %r" % (st,))
    # Thin windows (a transition: one on-foot sample, or six) are left out of ON FOOT, STABLE and SHAPE, and ON FOOT says how many.
    thin3 = sub(sub(text, "window=3 samples=58 on-foot=58 other=0", "window=3 samples=58 on-foot=1 other=57"),
                "fp=5281 frac=1.3098 range=5004..6044 h=0.8017 shape=1.690", "fp=10746 frac=2.6652 range=10746..10746 h=1.9283 shape=1.430")
    st, out = statuses(thin3)
    if st.get("ON FOOT") != "PASS" or st.get("STABLE") != "PASS" or st.get("SHAPE") != "PASS" or "2 on-foot window(s), 119 sample(s)" not in out or \
            "1 thinner window(s) (under 12 on-foot samples) left out" not in out:
        fail("a window of one on-foot sample (fp 10746, shape 1.43) was not left out of ON FOOT, STABLE and SHAPE: %r\n%s" % (st, out))
    all_thin = re.sub(r"on-foot=(60|58|59) other=0", r"on-foot=6 other=\1", text)
    st, out = statuses(all_thin)
    if st.get("ON FOOT") != "WARN" or "no window has 12" not in out or "STABLE" in st or "SHAPE" in st or st.get("STORED") != "PASS":
        fail("windows that all have fewer than 12 on-foot samples were judged: %r\n%s" % (st, out))
    # SHAPE: the MEDIAN of the windows' shapes against 16:9: -9% to +3% passes, to +-15% WARNs, beyond STOPs.
    for shape, want, why in (("1.650", "PASS", "-7.2%"), ("1.800", "PASS", "+1.3%"), ("1.600", "WARN", "-10.0%"), ("1.850", "WARN", "+4.1%"),
                             ("1.500", "STOP", "-15.6%"), ("2.100", "STOP", "+18.1%")):
        st, out = statuses(re.sub(r"shape=\d\.\d{3}", "shape=" + shape, text))
        if st.get("SHAPE") != want or (why not in out):
            fail("every window's shape %s (%s off 16:9) did not read %s: %r\n%s" % (shape, why, want, st, out))
        if want != "PASS" and "not what the instrument assumes" not in out:
            fail("a shape that did not pass did not say what is wrong: %r\n%s" % (st, out))
    st, out = statuses(sub(text, "range=5004..6044 h=0.8017 shape=1.690", "range=5004..6044 h=1.0000 shape=1.200"))
    if st.get("SHAPE") != "PASS" or "reads 1.700 against" not in out:
        fail("one window of an odd shape (1.2) beside two good ones moved the median verdict: %r\n%s" % (st, out))
    st, out = statuses(re.sub(r" shape=\d\.\d{3}", "", text))
    if st.get("SHAPE") != "n/a":
        fail("windows with no shape token did not read n/a: %r" % (st,))
    # SHAPE in the eye's own pixels: the log's game FOV, when it carries one, gives fx/fy and the reference 16/9 x fx/fy.
    st, out = statuses(text + "\n" + fov_line)
    if st.get("SHAPE") != "PASS" or "16:9 in this eye's pixels = 1.778 (16:9 x fx/fy 1.0003, from the log's game FOV)" not in re.sub(r"[ ]+", " ", out):
        fail("a square-pixel FOV did not give the reference 1.778 from the log: %r\n%s" % (st, out))
    wide_pixels = fov_line.replace("L -0.89775/0.71858/0.79986/-0.79986 R -0.71858/0.89775/0.79986/-0.79986",
                                   "L -0.74692/0.74692/0.79986/-0.79986 R -0.74692/0.74692/0.79986/-0.79986")
    st, out = statuses(text + "\n" + wide_pixels)
    if st.get("SHAPE") != "STOP" or "16:9 x fx/fy 1.1500" not in re.sub(r"[ ]+", " ", out) or "-16.8%" not in out:
        fail("an eye whose pixels are 15%% wider than tall did not move the reference to 2.044 (shape 1.700 reads -16.8%% off it): %r\n%s" % (st, out))
    tall_pixels = fov_line.replace("L -0.89775/0.71858/0.79986/-0.79986 R -0.71858/0.89775/0.79986/-0.79986",
                                   "L -0.89708/0.89708/0.79986/-0.79986 R -0.89708/0.89708/0.79986/-0.79986")
    st, out = statuses(text + "\n" + tall_pixels)
    if st.get("SHAPE") != "WARN" or "16:9 x fx/fy 0.8500" not in re.sub(r"[ ]+", " ", out) or "+12.5%" not in out:
        fail("an eye whose pixels are 15%% taller than wide did not move the reference to 1.511 (shape 1.700 reads +12.5%% off it): %r\n%s" % (st, out))
    # Two distances: the last window flown at 1.0 with the footprint scaled by 1/d (A at distance 1 unchanged): the law holds; if it did not
    # scale (the screen the same width at another distance) it does not.
    w4 = next(l for l in text.splitlines() if "window=4 " in l)
    w4d = (w4.replace("distance=0.700 applied=0.700", "distance=1.000 applied=1.000").replace("fp=5270 frac=1.3070 range=5009..5961 h=0.7953",
                                                                                             "fp=3689 frac=0.9149 range=3506..4173 h=0.5567"))
    st, out = statuses(text.replace(w4, w4d))
    if st.get("DISTANCE LAW") != "PASS" or "varies as 1/d" not in out or st.get("STABLE") != "PASS":
        fail("a window flown at another panel distance with the footprint scaled by 1/d did not PASS the law (and hold): %r\n%s" % (st, out))
    w4bad = w4d.replace("at1=3689 frac1=0.9149", "at1=5270 frac1=1.3070")
    st, out = statuses(text.replace(w4, w4bad))
    if st.get("DISTANCE LAW") != "STOP" or "does NOT vary as 1/d" not in out:
        fail("a footprint that did not scale with the distance did not STOP the law: %r\n%s" % (st, out))
    # CALIBRATION is the session's STORED value (the last window's session-frac1, the p10) at a 4032 px eye: x 4032 / 0.7 is the screen's
    # head-on footprint at the calibration distance, whatever distance was flown; within 5% of 5006 passes.
    st, out = statuses(sub(text, "session-frac1=0.8691", "session-frac1=0.7909"))
    if st.get("CALIBRATION") != "WARN" or "m = 0.769 reproduces 3504" not in out or "4556 px" not in out or "91.0%" not in out:
        fail("a stored floor 9%% under the calibration point did not WARN and name the m that reproduces 3504 (0.769): %r\n%s" % (st, out))
    st, out = statuses(sub(text, "session-frac1=0.8691", "session-frac1=0.8300"))
    if st.get("CALIBRATION") != "PASS" or "95.5%" not in out:
        fail("a stored floor 4.5%% under the calibration point did not PASS: %r\n%s" % (st, out))
    st, out = statuses(sub(text, "session-frac1=0.8691", "session-frac1=0.8170"))
    if st.get("CALIBRATION") != "WARN" or "94.0%" not in out:
        fail("a stored floor 6%% under the calibration point did not WARN: %r\n%s" % (st, out))
    st, out = statuses(re.sub(r"(?<![-\w])fp=\d+", "fp=3200", text))
    if st.get("CALIBRATION") != "PASS":
        fail("the calibration read the windows' medians (all 3200 here) instead of the session's stored value: %r\n%s" % (st, out))
    st, out = statuses(text.replace("eye=4032x3898", "eye=3296x3186"))
    if st.get("CALIBRATION") != "n/a" or "not at the calibration point (a 4032 px eye)" not in out:
        fail("another eye did not read n/a at the calibration point: %r\n%s" % (st, out))
    st, out = statuses(re.sub(r"session-n=\d+", "session-n=6", text))
    if st.get("CALIBRATION") != "n/a" or "fewer than 12 on-foot samples" not in out:
        fail("a session of fewer than 12 on-foot samples did not read n/a at the calibration point: %r\n%s" % (st, out))
    # A log from a build before the p10 calibration (its window lines print m=1.00; it stored the session's median) is not held against the
    # p10 calibration point, and says what it stored.
    st, out = statuses(text.replace("legacy=5040 m=0.700", "legacy=5040 m=1.00"))
    if st.get("CALIBRATION") != "n/a" or "from before the p10 calibration" not in out or st.get("STORED") != "PASS" or \
            "the on-foot MEDIAN (this log is from a build before the p10 calibration: m=1.000 on its window lines) is stored" not in out:
        fail("a log whose window lines print m=1.00 was held against the p10 calibration point as if it stored a p10: %r\n%s" % (st, out))
    st, out = statuses(re.sub(r"persisted=[0-9.]+", "persisted=no", text))
    if st.get("STORED") != "WARN" or "needs at least 12 on-foot samples" not in out:
        fail("on-foot samples with nothing stored did not WARN: %r" % (st,))
    st, out = statuses(sub(text, "persisted=0.8694 fit=3504", "persisted=0.8694 fit=3856"))
    if st.get("STORED") != "PASS" or "the next launch fits 3856 wide (this launch: 3504)" not in out or "head-on floor (p10) is stored: 0.8694" not in out:
        fail("a next launch that would change the width did not say so: %r\n%s" % (st, out))
    # A save that failed: the window lines carry save-failed=N (the count so far this session; read by key, absent in every log before the first
    # failure and from a build without it), and STORED WARNs, whichever window it was and whatever the later windows managed to save.
    def with_line(t, window, old, new):
        return "\n".join(l.replace(old, new) if (" window=%d " % window) in l else l for l in t.splitlines())
    if any(x["save_failed"] for x in vscreen_fit_windows(parse_vscreen_fit(text))):
        fail("the fixture has no save-failed token, but a window reads one")
    failed_first = with_line(with_line(with_line(text, 2, "persisted=0.8694 fit=", "persisted=no save-failed=1 fit="),
                                       3, "persisted=0.8694 fit=", "persisted=0.8694 save-failed=1 fit="), 4, "persisted=0.8694 fit=", "persisted=0.8694 save-failed=1 fit=")
    wf = vscreen_fit_windows(parse_vscreen_fit(failed_first))
    if [x["save_failed"] for x in wf] != [0, 1, 1, 1] or [x["persisted"] for x in wf] != [None, None, 0.8694, 0.8694]:
        fail("save-failed was not read by key from the window lines (or persisted=no read as a number): %r" % ([(x["save_failed"], x["persisted"]) for x in wf],))
    st, out = statuses(failed_first)
    flat_out = re.sub(r"[ ]+", " ", out)
    if st.get("STORED") != "WARN" or "1 save(s) of the on-foot footprint FAILED (first in window 2;" not in flat_out or \
            "`vscreen footprint: SAVE FAILED` line names the Win32 error" not in flat_out or \
            "the last value that did reach it is 0.8694 of the eye at panel distance 1 (window 4)" not in flat_out or "persisted None save-failed 1;" not in flat_out or \
            "persisted 0.8694 save-failed 1;" not in flat_out:
        fail("a first save that failed and was retried did not WARN STORED, naming the window, the log line and the last value that reached the file: %r\n%s" % (st, out))
    never = text
    for win, n in ((2, 1), (3, 2), (4, 3)):
        never = with_line(never, win, "persisted=0.8694 fit=", "persisted=no save-failed=%d fit=" % n)
    st, out = statuses(never)
    if st.get("STORED") != "WARN" or "3 save(s) of the on-foot footprint FAILED (first in window 2;" not in out or \
            "nothing from this session has reached it (persisted=no on every window)" not in out:
        fail("saves that failed in every window did not WARN STORED, saying nothing reached the file: %r\n%s" % (st, out))
    # The failure line of the log is no arming line (the reader tells those apart by their first words), and changes no other verdict.
    failure_line = ("[06:08:05.500] vscreen footprint: SAVE FAILED (Win32 error 5) -- the on-foot footprint did not reach vscreen_auto_footprint.txt: the file keeps its "
                    "previous value, and the next 30 s window tries again. Failed saves this session: 1; the 30 s lines carry save-failed=N from now on, and persisted=no "
                    "(or the last value that did reach the file) until a save succeeds.")
    pf = parse_vscreen_fit(failed_first + "\n" + failure_line)
    if len(pf["armed"]) != 1 or pf["armed"][0][1] != "armed" or len(pf["windows"]) != 4:
        fail("the SAVE FAILED line was read as an arming or a window line: %r" % (pf["armed"],))
    st_with, _ = statuses(failed_first + "\n" + failure_line)
    st_without, _ = statuses(failed_first)
    if st_with != st_without or st_with.get("INSTRUMENT") != "PASS":
        fail("the SAVE FAILED line changed a verdict: %r vs %r" % (st_with, st_without))
    rc, out = report("nothing of the kind\nvr camera census: x\n")
    if rc != 1 or "no vscreen auto-fit line" not in out:
        fail("a log with none of the lines did not exit 1 and say so:\n%s" % out)

    # ---- the census verdict reads the render size from the log, never assumes it ----
    census_path = os.path.join(here, CENSUS_FIXTURE)
    if os.path.isfile(census_path):
        census = read_text(census_path).replace("\r\n", "\n")  # a CRLF checkout (issue 64)
        apply_line = "[00:00:01.000] vScreen resolution: 1920x1080 -> 3504x1971 at 6 site(s). This writes to game CODE\n"
        blind = census.replace("hdr=5040x2835", "hdr=0x0").replace(" px in 5040x2835,", " px in 0x0,")
        if blind == census:
            fail("the census fixture has no hdr= or JITTERED size to blank")
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            print_camera_census(apply_line + blind)
        got = buf.getvalue()
        if "about 1.8e-04" in got or "NDC at 3504x1971" not in got:
            fail("with the route's sizes blanked the census verdict did not read the size the panel patch logged (3504x1971):\n%s" % got)
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            print_camera_census(blind)
        leak = [l for l in buf.getvalue().splitlines() if "(i) LEAK" in l]
        if len(leak) != 1 or "NDC at" in leak[0] or "the render size is not in this log" not in leak[0]:
            fail("with no size anywhere in the log the census verdict's LEAK line still assumed one: %r" % (leak,))
        if route_hdr([], route_events(apply_line)) != (3504, 1971, "the panel patch's `vScreen resolution:` line"):
            fail("route_hdr did not fall back to the panel patch's line: %r" % (route_hdr([], route_events(apply_line)),))
    else:
        fail("the census fixture %s is missing beside this script" % CENSUS_FIXTURE)
    return ok


def self_test_camera_census():
    """--camera-census on the checked-in synthetic flight (tools\\camera_census_fixture.log,
    which tools\\vr_camera_census_test holds to exactly what the DLL's formatters write),
    then on logs altered to take away each thing the report depends on, and to break each
    thing the stage 2 verdict judges. Returns ok."""
    import contextlib
    import io
    ok = True

    def fail(msg):
        nonlocal ok
        print("camera census: %s" % msg)
        ok = False

    def report(text):
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            rc = print_camera_census(text)
        return rc, buf.getvalue()

    def mutate(text, prefix, fn):
        """The same log with every census line that starts (after its stamp and
        the `vr camera census: ` words) with `prefix` replaced by fn(line)."""
        out = []
        for line in text.split("\n"):
            body = line.split("] ", 1)[1] if line.startswith("[") and "] " in line else line
            body = body[len("vr camera census: "):] if body.startswith("vr camera census: ") else ""
            out.append(fn(line) if body.startswith(prefix) else line)
        return "\n".join(out)

    def sub(text, old, new, count=-1):
        """text with `old` replaced by `new`; fails the test (and returns text) when `old` is not there: a mutation
        that changes nothing would pass every check for the wrong reason."""
        if old not in text:
            fail("the mutation %r -> %r found nothing to change" % (old, new))
            return text
        return text.replace(old, new, count)

    def squash(out):
        return re.sub(r"[ ]+", " ", out)

    def verdict_line(out, tag):
        """The verdict line (status word first) for (tag), or ''."""
        for line in out.split("\n"):
            m = re.match(r"^(PASS|WARN|STOP|n/a)\s+\(%s\) " % re.escape(tag), line)
            if m:
                return line
        return ""

    def status(out, tag):
        return verdict_line(out, tag).split(" ", 1)[0]

    fixture = os.path.join(os.path.dirname(os.path.abspath(__file__)), CENSUS_FIXTURE)
    if not os.path.isfile(fixture):
        fail("the fixture %s is missing beside this script" % CENSUS_FIXTURE)
        return False
    text = read_text(fixture).replace("\r\n", "\n")  # a CRLF checkout (issue 64)

    # ---- the parser ----
    c = parse_camera_census(text)
    if (len(c["windows"]), len(c["order"]), len(c["sequences"]), len(c["eyes"]),
            len(c["geometry"]), len(c["threads"])) != (2, 5, 3, 8, 8, 0):
        fail("the fixture parsed as %r, not 2 windows, 5 cameras, 3 sequences, 8 eye draws, 8 geometry lines, no "
             "other-thread entry" % ((len(c["windows"]), len(c["order"]), len(c["sequences"]), len(c["eyes"]),
                                      len(c["geometry"]), len(c["threads"])),))
    cam = c["cameras"].get(0x241DF6D0BB0)
    if not cam or cam["kind"] != 5 or cam["caller"] != 0x594E13 or cam["tone"] != "after" or \
            abs(cam["near"] - 0.025) > 1e-9 or cam["tan"] is not None or \
            cam["viewport"] != (0.0, 0.0) or cam["frame"] != 4 or cam["view"] != 0x241DD00E000 or \
            cam["vctx"] != 0x241DCE749F40:
        fail("the eye camera's line (kind 5: no tangents, no viewport) parsed as %r" % (cam,))
    world = c["cameras"].get(0x241DC2E2960)
    if not world or world["kind"] != 5 or world["frame"] != 1 or world["tone"] != "none":
        fail("the world camera's first-sight line (it was an eye camera in the cockpit: kind 5) parsed as %r" % (world,))
    seq = c["sequences"][0]
    if seq["frame"] != 4 or seq["calls"] != 24 or len(seq["rows"]) != 24 or seq["rows"][0]["rows"] is None or \
            len(seq["rows"][0]["rows"]) != 16 or seq["rows"][15]["kind"] != 1 or seq["rows"][15]["rows"] is not None or \
            seq["rows"][15]["post"] != 0x4 or seq["rows"][0]["pre"] != 0x1C or seq["rows"][0]["post"] != 0x0 or \
            seq["foot"] != "yes" or seq["rows"][0]["view"] != 0x241DD00A000 or seq["rows"][23]["view"] != 0x241DD00F000:
        fail("a call sequence parsed wrong: %r" % (seq["rows"][15],))
    if seq["phase_state"] != "on" or seq["phase"] != (0.252, -0.126) or \
            [(r["kind"], r["inj"], r["role"]) for r in (seq["rows"][0], seq["rows"][12], seq["rows"][15], seq["rows"][17])] != \
            [(3, 1, "scene"), (3, 1, "fp"), (1, 0, "-"), (5, 0, "-")]:
        fail("the sequence's phase, or a call's inj and role, parsed wrong: %r %r" % (seq["phase"], seq["rows"][0]))
    if [s["phase"] for s in c["sequences"]] != [(0.252, -0.126), (-0.189, 0.063), (0.126, 0.252)] or \
            [e["phase"] for e in c["eyes"]][:2] != [(0.252, -0.126), (0.252, -0.126)] or \
            any(e["phase_state"] != "on" for e in c["eyes"]):
        fail("the phases of the sequences and the eye draws parsed wrong: %r" % ([s["phase"] for s in c["sequences"]],))
    g0 = seq["rows"][0]["geo"]
    if not g0 or abs(g0["near"] - 0.025) > 1e-9 or abs(g0["shift"][0] - 2 * 0.252 / 5040) > 1e-6 or \
            abs(g0["shift"][1] - 2 * 0.126 / 2835) > 1e-6 or abs(g0["aspect"] - 5040.0 / 2835.0) > 1e-4:
        fail("a call's geometry read back from its rows is wrong: %r" % (g0,))
    if any(e["foot"] != "yes" for e in c["eyes"]):
        fail("an eye draw parsed without the journal's word: %r" % (c["eyes"][0],))
    if not any("kind" in ch["fields"] and ch["fields"]["kind"] == "5->3" for chs in c["changes"].values() for ch in chs) or \
            any(ch["fields"].keys() - {"bound", "kind", "aspect", "fov", "viewport", "near"} for chs in c["changes"].values() for ch in chs):
        fail("the 'changed:' lines parsed as %r (the world camera's kind 5->3 is one)" % (c["changes"],))
    routes = parse_world_route(text)
    if len(routes) != 2 or routes[0]["route"]["jitter"] != "idle" or routes[1]["route"]["jitter"] != "on" or \
            route_tok(routes[1], "hdr") != "5040x2835" or route_tok(routes[1], "inj-kinds") != "3:6750" or \
            _cpair(route_tok(routes[1], "phase")) != (-0.252, -0.063) or route_tok(routes[1], "fp-mode") != "0/450/0" or \
            route_tok(routes[1], "pair-checked") != "448" or route_hdr(routes) != (5040, 2835, "a `vr world route 5s:` line's hdr="):
        fail("the route's two lines parsed as %r" % (routes,))
    # A line with no stamp, one with, and a value the DLL printed as nan; and an older census's call line, which has no inj and no role.
    n = parse_camera_census("vr camera census: call frame=9 n=1 camera=0x10 kind=3 caller=+0x594E13 draw=- tone=none "
                            "fl=0x1C>- rows=[1,nan,0,0,0,0,0,0,0,0,0,0,0,0,0,0]\n"
                            "[01:02:03.004] vr camera census: call frame=9 n=2 camera=0x10 kind=- caller=+0x1 draw=7 "
                            "tone=after inj=1 role=scene fl=0x0>0x0 rows=-\nsomething else entirely\n")
    rows = n["sequences"][0]["rows"] if n["sequences"] else []
    if len(rows) != 2 or rows[0]["draw"] is not None or rows[0]["post"] is not None or rows[1]["kind"] is not None or \
            rows[1]["rows"] is not None or rows[0]["rows"][1] == rows[0]["rows"][1] or n["lines"] != 2 or \
            (rows[0]["inj"], rows[0]["role"]) != (None, None) or (rows[1]["inj"], rows[1]["role"]) != (1, "scene") or \
            rows[0]["geo"] is not None or n["sequences"][0]["phase_state"] != "absent":
        fail("the stamp, a dash, a nan, and a line with no inj/role/phase parsed wrong: %r lines=%r" % (rows, n["lines"]))
    if (_cphase(None), _cphase("-"), _cphase("0.25,-0.5"), _cphase("x,y"), _cphase("1,2,3"), _cphase("nan,1")) != \
            (("absent", None), ("off", None), ("on", (0.25, -0.5)), ("absent", None), ("absent", None), ("absent", None)):
        fail("_cphase")
    if (_ckinds("none"), _ckinds("3:54"), _ckinds("3:54,other:2"), _ckinds("3:x"), _ckinds(None), _ckinds("")) != \
            ({}, {"3": 54}, {"3": 54, "other": 2}, None, None, None):
        fail("_ckinds")
    if (_cint("12"), _cint("-1"), _cint(None), _cint("")) != (12, None, None, None):
        fail("_cint")

    # A line cut short or garbled is counted and skipped, and a call with no camera has nothing to digest.
    g = parse_camera_census("vr camera census: call frame=x n=1 camera=0x10 kind=3 caller=+0x1 draw=- tone=none fl=0x0>- rows=-\n"
                            "vr camera census: call frame=5 n=1 kind=3 caller=+0x1 draw=- tone=none fl=0x0>- rows=-\n"
                            "vr camera census: sequence frame=5 index=1/3 foot=yes calls=1 recorded=1 truncated=0\n"
                            "vr camera census: call frame=5 n=1 camera=0x20 kind=3 caller=+0x1 draw=- tone=none fl=0x0>- view=0x99 rows=-\n")
    if g["unparsed"] != 2 or len(g["sequences"]) != 1 or len(g["sequences"][0]["rows"]) != 1 or \
            g["sequences"][0]["rows"][0]["view"] != 0x99:
        fail("a garbled line and a call with no camera were not skipped: %r" % (g,))
    rc, out = report("vr camera census: call frame=x n=1 camera=0x10 kind=3\n")
    if rc != 0 or "1 census line(s) could not be parsed" not in out:
        fail("a log of only garbled lines did not say so:\n%s" % out)

    # ---- the geometry a call's rows carry, read back out of them (flight 1's real rows) ----
    scene = [-1.04384, -0.007344862, 0, 0.113728, -2.103792e-08, 1.866733, 0, 0.03455516, -0.1195614, 0.06412452, 0, -0.9929108,
             0, 0, 0.025, 0]
    first_person = [-1.285258, -0.009043574, 0, 0.113728, -2.590355e-08, 2.298469, 0, 0.03455516, -0.1472135, 0.07895518, 0,
                    -0.9929108, 0, 0, 0.0675, 0]
    eye_left = [0.9415472, -0.06691322, 0, 0.02606118, 0.07811873, 0.9655237, 0, 0.08863233, 0.1475604, -0.08419283, 0, 0.9957235,
                0, 0, 0.025, 0]
    gs, gf, ge = census_geometry(scene), census_geometry(first_person), census_geometry(eye_left)
    if not gs or abs(gs["xs"] - 1.05066) > 1e-4 or abs(gs["ys"] - 1.86785) > 1e-4 or abs(gs["aspect"] - 16.0 / 9.0) > 1e-4 or \
            abs(gs["near"] - 0.025) > 1e-9 or max(abs(v) for v in gs["shift"]) > 1e-6:
        fail("flight 1's scene rows read back as %r, not 1.0507 x 1.8678, aspect 16:9, near 0.025, no shift" % (gs,))
    if not gf or abs(gf["ys"] / gs["ys"] - 1.2313) > 1e-3 or abs(gf["near"] - 0.0675) > 1e-9:
        fail("flight 1's first-person rows read back as %r, not x1.231 tighter with near 0.0675" % (gf,))
    if not ge or abs(ge["shift"][0] - 0.178391) > 1e-5 or abs(ge["aspect"] - 1.0344) > 1e-3 or abs(ge["fov"] - 1.59971) > 1e-3:
        fail("flight 1's left eye rows read back as %r, not shift +0.1784, aspect 1.0344, fov 1.5997" % (ge,))
    if census_geometry(None) is not None or census_geometry([1.0] * 15) is not None or \
            census_geometry([float("nan")] + [0.0] * 15) is not None or census_geometry([0.0] * 16) is not None:
        fail("rows that are absent, short, not finite or degenerate produced a geometry")
    if census_phase_ndc((0.252, -0.126), 5040, 2835) != (2 * 0.252 / 5040, 2 * 0.126 / 2835) or \
            census_phase_ndc((0.0, 0.0), 5040, 2835) != (0.0, -0.0):
        fail("census_phase_ndc is not flatProjectionJitter's rule (x = 2 px / W, y = -2 py / H)")
    # One object, two projections: grouped by scale and near, whichever way the head is turned.
    def row(n, rows):
        return {"n": n, "kind": 3, "camera": 0x1, "caller": 0x594E13, "tone": "before", "view": 0x5, "frame": 7,
                "rows": rows, "geo": census_geometry(rows), "inj": None, "role": None}
    groups = census_projections([row(1, scene), row(2, first_person), row(3, scene), row(4, scene)])
    if [grp["count"] for grp in groups] != [3, 1] or groups[1]["near"] != 0.0675 or groups[0]["near"] != 0.025:
        fail("one camera's kind-3 calls were not split into its two projections: %r" % ([(g["count"], g["near"]) for g in groups],))

    # ---- the report on the fixture ----
    rc, out = report(text)
    flat = squash(out)
    want = [
        "165 census line(s): 2 5 s line(s), 5 camera(s), 3 call sequence(s), 8 eye draw(s), 0 other-thread entries, 3 episode(s)",
        "off-thread calls: 0 in every window -- the refresh runs on the render thread",
        "frames with the tone drawn: 898; sampled (tone while the journal says on foot, or no journal; while the world route "
        "jitters, with a non-zero phase): 450; the journal said: no, yes",
        "world route: 2 5 s window(s); jitter: idle x1, on x1",
        "0x241DC2E2960 5 +0x594E13 0.95 0.025 1.5708 50000 (0, 0) (0, 0) 0x241DD00E000 none 1 k3 x45 WORLD; 3 'changed:' line(s)",
        "0x241DF6D0BB0 5 +0x594E13 0.95 0.025 1.5708 50000 (0, 0) (0, 0) 0x241DD00E000 after 4 k5 x9 EYE",
        "0x241DC2E2960 frame 4 moved: aspect 0.95->1.777778, bound (0,0)->(5e-05,4.444444e-05), fov 1.570796->1.0122, kind 5->3, "
        "viewport (0,0)->(5040,2835)",
        "camera 0x241DC2E2960 WORLD: k3 x45 over 3 logged frame(s) (15.0 a frame); kind-3 calls before the tone 45, after 0; "
        "first seen as kind 5",
        "projection 1: 12.0 call(s) a frame, scale 1.0149 x 1.8042 (aspect 1.7778), near 0.025, off-centre (9.99e-05, 8.89e-05); "
        "role= scene x36; injected 36 of 36",
        "projection 2: 3.0 call(s) a frame, scale 1.2507 x 2.2234 (aspect 1.7778), near 0.0675, x1.232 tighter than projection 1",
        "role= fp x9; injected 9 of 9 -- the first-person weapon camera's signature: the same object, a tighter field of view and "
        "a larger near plane",
        "camera 0x241DE100200 world-side: k3 x2, k5 x1 over 3 logged frame(s) (1.0 a frame); kind-3 calls before the tone 2, after 0; "
        "SEVERAL KINDS (frame 4: k5 x1, frame 5: k3 x1, frame 6: k3 x1)",
        "camera 0x241DF6D0BB0 EYE: k5 x9 over 3 logged frame(s) (3.0 a frame)",
        "frame 4 (sequence 1/3, journal foot=yes, phase (0.2520, -0.1260) px): 24 call(s), 24 recorded, 0 truncated",
        "camera 0x241DC2E2960 k3: 15 call(s), callers +0x594E13 x7, +0x594EAB x4, +0x594FE1 x4, view 0x241DD00A000, 0x241DD00A800, "
        "0x241DD00B000, tone before x15, draws 350..5047; injected 15 (fp 3, scene 12)",
        "camera 0x241DE100200 k5: 1 call(s), callers +0x594FE1 x1, view 0x241DD00C800, tone after x1, draws 8209..8209",
        "camera 0x241DE100200 k3: 1 call(s), callers +0x58DE73 x1, view 0x241DD00C800, tone before x1, draws 5059..5059; injected 0 (aux 1)",
        "k5 +0x594FE1 after x1 (calls 18..18) camera 0x241DE100200",
        "eye 0 frame 4 draw 8213 (journal foot=yes, phase (0.2520, -0.1260) px) b1 0x1EB2E751E20 first=0 bytes=5376",
        "eye 0 frame 4 draw 8213: camera 0x241DF6D0BB0 (kind 5) caller +0x594E13, +0x594EAB, +0x594FE1, 3 call(s) n=19/20/21, "
        "draw 8210/8211/8212, tone after",
        "eye 1 frame 6 draw 8220: camera 0x241DF6D0FF0 (kind 5) caller +0x594E13, +0x594EAB, +0x594FE1, 3 call(s) n=22/23/24",
        "eye camera 0x241DF6D0BB0 signature: kind 5, caller +0x594E13, aspect 0.95, near 0.025, far 50000, fov 1.5708, bound (0, 0), "
        "offcentre (0.263158, -0.1), viewport (0, 0), tangents -, view 0x241DD00E000",
        "eye 1 frame 7 draw 8220: frame 7's call sequence was not logged (only the first 3 on-foot frames are)",
        "eye camera(s): 0x241DF6D0BB0, 0x241DF6D0FF0; world camera: 0x241DC2E2960; other world-side kind-3 camera(s): 0x241DE100200",
        "world camera 0x241DC2E2960: 45 kind-3 call(s) over 3 logged frame(s) = 15.0 a frame, 45 of them before the tone "
        "(2 projection(s); its first-seen kind was 5)",
        "(A) call signature (kind, and what a call's composed rows say: aspect, fov, near, off-centre): kind, aspect, shift separate "
        "every eye camera from EVERY world-side kind-3 camera",
        "(B) content join: 6 of 8 eye draw(s) joined to a camera; eye camera(s) 0x241DF6D0BB0, 0x241DF6D0FF0",
        "(C) place in the frame: every refresh of a world-side camera precedes every refresh of an eye camera in 3 of 3 logged "
        "sequence(s)",
        "frame 4: world-side 15 call(s) [tone before x15; last draw 5047], eye 6 call(s) [tone after x6; first draw 8210]",
        "the tone flag separates the two sets",
        "(D) caller: eye camera(s) call from +0x594E13, +0x594EAB, +0x594FE1; the world camera from +0x594E13, +0x594EAB, +0x594FE1; "
        "shared: +0x594E13, +0x594EAB, +0x594FE1 -- the caller alone does not separate them",
        "(F) view (the refresh's second argument, the pass object): eye camera(s) are refreshed with 0x241DD00E000, 0x241DD00F000; "
        "the world camera with 0x241DD00A000, 0x241DD00A800, 0x241DD00B000; no view is shared: the view separates them",
        "(E) tangents: not compared: the eye cameras' lines carry no tangents (tan=-",
        "leak measure over 8 eye draw(s): largest |leak| = 3.648e-08 NDC",
        "the rows cannot tell which way the shift is carried: every candidate leaves about 5.26e-09 NDC",
        "PASS (i) LEAK: 8 of 8 eye draw(s) had a non-zero world phase (up to 0.2520 px, about 1.8e-04 NDC at 5040x2835); worst |leak| "
        "3.65e-08 NDC over 8 measured, below 1e-06",
        "PASS (ii) KIND-3 ROWS CARRY THE PHASE: 45 injected kind-3 call(s) in 3 sequence(s) with a non-zero phase measure the phase "
        "they were given (scene 36 call(s) worst 9.48e-08, first-person 9 call(s) worst 1.16e-07), within 1e-06 NDC; expected: "
        "x = 2 px / W, y = -2 py / H at 5040x2835 (from a `vr world route 5s:` line's hdr=)",
        "PASS (iii) INJECTED KINDS: only kind 3 was injected (route inj-kinds=3:6750 over 2 window(s); 45 logged call(s) with inj=1, "
        "all kind 3)",
        "PASS (iv) OFF-THREAD / UNREADABLE: none: census off-thread 0 and no unreadable kind over 2 window line(s), route off-thread 0 "
        "and unreadable 0 over 2 route window(s)",
        "PASS (v) ROLES: the route counted inj-scene 5400, inj-fp 1350, inj-refused 0, warming 0, aux 450, after 0, unsupported 2700, "
        "other-kind 900; logged calls in non-zero-phase sequences: scene 36 (36 injected), first-person 9 (9 injected), auxiliary 2 "
        "(0 injected)",
        "note: the route's own pair check of the rows it believes: 448 checked, 0 inconsistent",
        "note: a fully-on-foot census window holds 6.0 kind-5 calls a frame (design: 6.0, two eyes x three call sites); 15.0 injected "
        "calls a frame (design: 54-68) (over 450 frame(s) in 1 window(s))",
        "PASS (vi) INJECTION WINDOW: inj-shut=0 over 2 route window(s); inj-unnamed=0 (one per world-to-map change is expected: its first frame "
        "cannot be told from a world frame before the cameras refresh; this log shows 0 such episode(s)); jitter=unnamed in 0 window(s)",
        "stage 2 verdict: PASS (6 PASS, 0 WARN, 0 STOP, 0 n/a)",
    ]
    if rc != 0:
        fail("the fixture reported exit %d" % rc)
    for w in want:
        if w not in flat:
            fail("the report on the fixture lacks %r:\n%s" % (w, out))
            break
    # The two things flight 1 showed: a camera object labelled by its FIRST call's kind, and (C), (D) and (F) then unanswerable.
    if "not enough calls logged to say" in out or "other kind" in out:
        fail("the report on the fixture still says 'not enough calls' or labels a camera 'other kind':\n%s" % out)

    # ---- the join, a row at a time ----
    # A float moved by 9e-6 still joins; one moved by 2e-5 does not, and that draw says so.
    def nudge(delta):
        def fn(line):
            head, _, tail = line.partition("rows=[")
            values, _, rest = tail.partition("]")
            parts = values.split(",")
            parts[0] = "%.9g" % (float(parts[0]) + delta)
            return head + "rows=[" + ",".join(parts) + "]" + rest
        return fn
    near = mutate(text, "eye=0 frame=5 ", nudge(9e-6))
    far = mutate(text, "eye=0 frame=5 ", nudge(2e-5))
    _, out = report(near)
    if "(B) content join: 6 of 8" not in out:
        fail("rows 9e-6 apart did not join (the tolerance is 1e-5):\n%s" % out)
    _, out = report(far)
    if "(B) content join: 5 of 8" not in out or \
            "eye 0 frame 5 draw 8213: NO logged call of frame 5 produced these rows" not in out:
        fail("rows 2e-5 apart joined, or the unmatched draw was not named:\n%s" % out)
    # No census lines at all: exit 1 and a sentence that says why.
    rc, out = report("[00:00:01.000] version 0.18.0 (build ABCD)\n[00:00:02.000] something else\n")
    if rc != 1 or "no `vr camera census` line in this log" not in out:
        fail("a log with no census lines reported exit %d:\n%s" % (rc, out))
    # The route never reported progress: a window with progress=no says so, and there is nothing to join.
    only = "[00:00:05.000] vr camera census 5s: frames=450 calls=1000 posts=1000 off-thread=0 stale=0 kinds=3:1000 " \
           "callers=+0x594E13:1000 cameras-seen=1 cameras-total=1 tone=0/0/1000 on-foot-frames=0 eye-draws=900/0 " \
           "progress=no hook=installed windows=1 cam-overflow=0 thread-overflow=0\n"
    rc, out = report(only)
    if rc != 0 or "progress=no in every window" not in out or "No eye draw was read back" not in out or \
            "off-thread calls: 0 in every window" not in out or "stage 2 verdict: n/a" not in out:
        fail("a progress=no window was not called out:\n%s" % out)
    # The tone was drawn all session but the journal never said on foot (a ship, a menu): named, and nothing is joined.
    ship = "[00:00:05.000] vr camera census 5s: frames=450 calls=1000 posts=1000 off-thread=0 stale=0 kinds=3:1000 " \
           "callers=+0x594E13:1000 cameras-seen=1 cameras-total=1 tone=500/500/0 tone-frames=450 on-foot-frames=0 foot=no " \
           "eye-draws=900/0 progress=yes hook=installed windows=1 cam-overflow=0 thread-overflow=0\n"
    rc, out = report(ship)
    if rc != 0 or "the tone was drawn in 450 frame(s) but no frame was sampled" not in out or \
            "the journal said: no" not in out or "No eye draw was read back" not in out:
        fail("a tone with no on-foot frame was not called out:\n%s" % out)
    # ... and when the route was jittering it says the phase is the other reason a frame is not sampled.
    jitter_ship = "[00:00:05.000] vr world route 5s: key=auto state=owned last=treated jitter=on phase=0.0000,0.0000 rows=0.0000,0.0000 " \
                  "fp-mode=0/1/0 last-trigger=VS=1 PS=2 target=2520x1417 hdr=5040x2835 selection=selected:1\n" + ship.replace("foot=no", "foot=yes")
    rc, out = report(jitter_ship)
    if "the route was jittering, and the census samples a frame then only with a non-zero phase" not in out:
        fail("a jittering route with no sampled frame did not name the phase as a reason:\n%s" % out)
    # Eye draws whose rows match nothing at all: the eye's camera is not at the refresh.
    every = mutate(text, "eye=", lambda line: line if "eye-geometry" in line else
                   line.replace("rows=[", "rows=[9").replace("[99", "[9"))
    _, out = report(every)
    if "no camera was joined to an eye draw" not in out or "NO logged call of frame 4" not in out or \
            "Cameras with kind-5 calls in the logged sequences (candidates, not joined): " not in out or \
            "== stage 2 verdict" not in out:
        fail("eye rows that join to no call did not say so (and still print the verdict):\n%s" % out)
    # Two cameras with the same rows are both named; a failed readback is named, not joined.
    twice = text + "[12:00:09.000] vr camera census: call frame=4 n=22 camera=0x241DF6D0FF0 kind=5 caller=+0x594FE1 draw=8214 tone=after fl=0x1C>0x0 rows=-\n"
    rc, out = report(twice)
    if rc != 0:
        fail("a log with an extra call line failed to report")
    failed = mutate(text, "eye=1 frame=6 ", lambda line: line.split("rows=")[0] + "rows=- meas=- why=map")
    _, out = report(failed)
    if "eye 1 frame 6 draw 8220: no rows were read (map)" not in out or "(B) content join: 5 of 8" not in out:
        fail("a failed readback was joined or not named:\n%s" % out)
    # A shift of nothing (the jitter off): every sign candidate is the same number, so no sign is named. (Found on the
    # glue rig's first real log: a tie was sorted by the candidates' spelling and named +shift.x, +shift.y.)
    zero = mutate(text, "eye-geometry ", lambda line: re.sub(r" shift=\([^)]*\)", " shift=(0,0)", line))
    _, out = report(zero)
    if "the rows cannot tell which way the shift is carried" not in out or "\n    the rows measure as " in out:
        fail("a zero shift named a sign, or did not say it could not:\n%s" % out)

    # ---- roles from the calls (flight 1's bug: a camera labelled by its first-seen kind) ----
    # The world camera's first-sight line says kind 5 and every call of it is kind 3: it is the WORLD camera, not "other kind".
    _, out = report(text)
    table = [l for l in out.split("\n") if l.startswith("0x241DC2E2960")]
    if not table or "WORLD" not in table[0] or "other kind" in table[0]:
        fail("the world camera (first seen as kind 5) was not labelled by its calls:\n%s" % table)
    # Take the world camera's kind-3 calls away and it has no call to be labelled by: no world camera is named.
    no_world = "\n".join(l for l in text.split("\n") if not (": call frame=" in l and "camera=0x241dc2e2960" in l.lower() and " kind=3 " in l))
    _, out = report(no_world)
    if "world camera: 0x241DC2E2960" in out:
        fail("a camera with no kind-3 call was named the world camera:\n%s" % out)
    # A camera that is kind 5 in one frame and kind 3 in another is labelled by BOTH, and its kind-5 call is not an eye call.
    _, out = report(text)
    if "SEVERAL KINDS (frame 4: k5 x1, frame 5: k3 x1, frame 6: k3 x1)" not in out or \
            "eye camera(s): 0x241DF6D0BB0, 0x241DF6D0FF0;" not in out:
        fail("a camera that changed kind between frames was not labelled by each frame's kind, or became an eye:\n%s" % out)
    # The eye cameras, when an eye draw's rows join a kind-3 call (an older build's eyes): still found, by the join.
    old_eyes = text.replace(" kind=5 caller", " kind=3 caller")
    _, out = report(old_eyes)
    if "eye camera(s): 0x241DF6D0BB0, 0x241DF6D0FF0;" not in out:
        fail("the eye cameras are found by the join whatever their kind:\n%s" % out)
    # The world camera is the camera with the most kind-3 calls BEFORE the tone (then the most kind-3 calls, then the first seen), not the first
    # camera of the table: move the auxiliary camera's line (2 kind-3 calls) ahead of the world camera's and the world camera is still found.
    lines = text.split("\n")
    aux_at = next(i for i, l in enumerate(lines) if "vr camera census: camera=0x241de100200 " in l)
    world_at = next(i for i, l in enumerate(lines) if "vr camera census: camera=0x241dc2e2960 " in l)
    lines.insert(world_at, lines.pop(aux_at))
    _, out = report("\n".join(lines))
    if "world camera: 0x241DC2E2960" not in out or out.index("0x241DE100200") > out.index("0x241DC2E2960 "):
        fail("the world camera was chosen by its place in the camera table, not by its calls:\n%s" % out)
    def kind3_call(n, camera, tone):
        return "vr camera census: call frame=1 n=%d camera=0x%X kind=3 caller=+0x594E13 draw=%d tone=%s fl=0x0>0x0 view=0x%X rows=-" % (
            n, camera, n, tone, camera + 0x1000)
    tie = parse_camera_census("vr camera census: sequence frame=1 index=1/3 calls=20 recorded=20 truncated=0\n" + "\n".join(
        [kind3_call(n, 0xA0, "after") for n in range(1, 11)] + [kind3_call(n, 0xB0, "before") for n in range(11, 21)]) + "\n")
    tie2 = parse_camera_census("vr camera census: sequence frame=1 index=1/3 calls=25 recorded=25 truncated=0\n" + "\n".join(
        [kind3_call(n, 0xA0, "before") for n in range(1, 11)] + [kind3_call(n, 0xB0, "before") for n in range(11, 26)]) + "\n")
    tie3 = parse_camera_census("vr camera census: sequence frame=1 index=1/3 calls=30 recorded=30 truncated=0\n" + "\n".join(
        [kind3_call(n, 0xA0, "before") for n in range(1, 11)] + [kind3_call(n, 0xB0, "before") for n in range(11, 21)] +
        [kind3_call(n, 0xB0, "after") for n in range(21, 31)]) + "\n")
    if census_roles(tie, [])["world"] != 0xB0 or census_roles(tie2, [])["world"] != 0xB0 or census_roles(tie3, [])["world"] != 0xB0:
        fail("the world camera is the one with the most kind-3 calls before the tone, then the most kind-3 calls: %r %r %r"
             % (census_roles(tie, [])["world"], census_roles(tie2, [])["world"], census_roles(tie3, [])["world"]))
    # No draw progress: the tone flag is unavailable and (C) says so instead of deciding.
    no_tone = text.replace("tone=before", "tone=none").replace("tone=after", "tone=none")
    _, out = report(no_tone)
    if "the tone flag is unavailable" not in out:
        fail("calls with tone=none were judged by a tone flag they do not have:\n%s" % out)

    # ---- the stage 2 verdict ----
    _, base = report(text)
    # (i) the leak: PASS below 1e-6, WARN to 1e-5, STOP above.
    def with_leak(value):
        return mutate(text, "eye-geometry eye=0 frame=5 ", lambda line: re.sub(r" leak=\([^)]*\)", " leak=(%s,0.000e+00)" % value, line))
    for value, want_status in (("9.0e-07", "PASS"), ("2.0e-06", "WARN"), ("9.0e-06", "WARN"), ("2.0e-05", "STOP"), ("-2.0e-04", "STOP")):
        _, out = report(with_leak(value))
        if status(out, "i") != want_status:
            fail("a leak of %s was %s, want %s:\n%s" % (value, status(out, "i"), want_status, verdict_line(out, "i")))
    _, out = report(with_leak("2.0e-04"))
    if "worst |leak| 2.00e-04 NDC at eye 0 frame 5" not in verdict_line(out, "i") or "stage 2 verdict: STOP" not in out:
        fail("a leaking eye was not named with its eye and frame, or did not make the verdict STOP:\n%s" % verdict_line(out, "i"))
    # A leak in a frame with a ZERO phase is not judged (nothing could leak): the same number is the baseline there.
    zero_frame = mutate(with_leak("2.0e-04"), "eye=0 frame=5 ", lambda line: re.sub(r" phase=\S+", " phase=0.0000,0.0000", line))
    _, out = report(zero_frame)
    if status(out, "i") != "PASS" or "7 of 8 eye draw(s) had a non-zero world phase" not in verdict_line(out, "i"):
        fail("an eye draw with a zero phase was judged for a leak:\n%s" % verdict_line(out, "i"))
    # An eye shift that is ON while the phase is non-zero: a warning, and the leak is the best fit over the shift's signs.
    shifted = mutate(text, "eye-geometry eye=0 frame=5 ", lambda line: re.sub(r" shift=\([^)]*\)", " shift=(0.0001,0.00005)", line))
    _, out = report(shifted)
    if status(out, "i") != "WARN" or "had the eye shift on while the world phase was non-zero" not in verdict_line(out, "i"):
        fail("an eye shift on during a non-zero phase was not warned about:\n%s" % verdict_line(out, "i"))
    # ... and when the shift is on, the game may carry it with the other sign than `leak=` assumes: an eye whose rows are the frustum moved by
    # MINUS the shift has a large `leak=` and a zero residual at the best fit, so it is the eye shift that is reported (WARN), not a leak (STOP).
    frustum0 = c["geometry"][(0, 5)]["frustum"]
    minus = census_expected_measure(frustum0, -0.0001, -0.00005)
    plus = census_expected_measure(frustum0, 0.0001, 0.00005)

    def carried_minus(line):
        line = re.sub(r" shift=\([^)]*\)", " shift=(0.0001,0.00005)", line)
        return re.sub(r" leak=\([^)]*\)", " leak=(%.3e,%.3e)" % (minus[0] - plus[0], minus[1] - plus[1]), line)
    signed = mutate(text, "eye-geometry eye=0 frame=5 ", carried_minus)
    signed = mutate(signed, "eye=0 frame=5 ", lambda line: re.sub(r" meas=\([^)]*\)", " meas=(%.9g,%.9g)" % minus, line))
    _, out = report(signed)
    worst_seen = re.search(r"worst \|leak\| ([0-9.e+-]+) NDC over", verdict_line(out, "i"))
    if status(out, "i") != "WARN" or "had the eye shift on while the world phase was non-zero" not in verdict_line(out, "i") or \
            not worst_seen or float(worst_seen.group(1)) > 1e-6 or abs(minus[0] - plus[0]) < 1e-4:
        fail("an eye carrying the shift with the other sign was not judged by its best fit:\n%s" % verdict_line(out, "i"))
    # No eye draw sampled with a non-zero phase while the route jitters: STOP with its reason (zero phases; phase=- earlier).
    all_zero = mutate(text, "eye=", lambda line: line if "eye-geometry" in line else re.sub(r" phase=\S+", " phase=0.0000,0.0000", line))
    _, out = report(all_zero)
    if status(out, "i") != "STOP" or "no eye draw was sampled with a non-zero phase although the route was jittering" not in verdict_line(out, "i"):
        fail("eye draws with only zero phases were not a STOP with its reason:\n%s" % verdict_line(out, "i"))
    early = mutate(text, "eye=", lambda line: line if "eye-geometry" in line else re.sub(r" phase=\S+", " phase=-", line))
    _, out = report(early)
    if status(out, "i") != "STOP" or "were read earlier with phase=-" not in verdict_line(out, "i") or "(jitter=on in 1 route window(s))" not in verdict_line(out, "i"):
        fail("eye draws read before the route jittered were not explained:\n%s" % verdict_line(out, "i"))
    no_eyes = "\n".join(l for l in text.split("\n") if "vr camera census: eye" not in l)
    _, out = report(no_eyes)
    if status(out, "i") != "STOP" or "no eye draw was read back" not in verdict_line(out, "i"):
        fail("a jittering route with no eye draw read back was not a STOP:\n%s" % verdict_line(out, "i"))
    # The route never jittered (the census on, the route key off): nothing to judge, and not a STOP.
    idle_route = mutate(text, "eye=", lambda line: line if "eye-geometry" in line else re.sub(r" phase=\S+", " phase=-", line))
    idle_route = "\n".join(l for l in idle_route.split("\n") if "vr world route" not in l)
    idle_route = re.sub(r" phase=0\.\d{4},-?0\.\d{4} calls=", " phase=- calls=", idle_route)
    idle_route = re.sub(r" phase=-0\.\d{4},-?0\.\d{4} calls=", " phase=- calls=", idle_route)
    _, out = report(idle_route)
    if status(out, "i") != "n/a" or "the route never jittered in this log" not in verdict_line(out, "i") or \
            status(out, "ii") != "n/a":
        fail("a census with the route never jittering was judged:\n%s\n%s" % (verdict_line(out, "i"), verdict_line(out, "ii")))
    # A phase that differs between a frame's sequence header and its eye line.
    mismatch = mutate(text, "sequence frame=4 ", lambda line: line.replace("phase=0.2520,-0.1260", "phase=0.2520,-0.1000"))
    _, out = report(mismatch)
    if status(out, "i") != "WARN" or "frame(s) 4 name different phases" not in verdict_line(out, "i"):
        fail("a sequence and an eye line with different phases were not warned about:\n%s" % verdict_line(out, "i"))

    # (ii) the kind-3 rows carry the phase, within 1e-6 of flatProjectionJitter's shift.
    def with_header_phase(old, new):
        return mutate(text, "sequence frame=4 ", lambda line: line.replace("phase=" + old, "phase=" + new))
    wrong = with_header_phase("0.2520,-0.1260", "0.3520,-0.1260")      # 1e-4 px further right: 2e-5 NDC
    _, out = report(wrong)
    if status(out, "ii") != "STOP" or "do not measure the phase they were given" not in verdict_line(out, "ii"):
        fail("rows that carry another phase were not a STOP:\n%s" % verdict_line(out, "ii"))
    tiny = with_header_phase("0.2520,-0.1260", "0.2524,-0.1260")      # 4e-4 px: 8e-8 NDC, inside the tolerance
    _, out = report(tiny)
    if status(out, "ii") != "PASS":
        fail("a phase within the tolerance of the rows was not a PASS:\n%s" % verdict_line(out, "ii"))
    between = with_header_phase("0.2520,-0.1260", "0.2640,-0.1260")    # 0.012 px: 4.8e-6 NDC, between 1e-6 and 1e-5
    _, out = report(between)
    if status(out, "ii") != "WARN" or "is between 1e-06 and 1e-05" not in verdict_line(out, "ii"):
        fail("rows between the tolerance and the stop were not a WARN:\n%s" % verdict_line(out, "ii"))
    flipped = with_header_phase("0.2520,-0.1260", "0.2520,0.1260")     # the y sign the other way
    _, out = report(flipped)
    if status(out, "ii") != "STOP" or "the rows that miss DO fit the phase with y flipped" not in verdict_line(out, "ii"):
        fail("rows carrying the y sign the other way were not recognised:\n%s" % verdict_line(out, "ii"))
    # The phase never reached a call: every scene and first-person call is inj=0.
    not_injected = text.replace(" inj=1 role=scene ", " inj=0 role=scene ").replace(" inj=1 role=fp ", " inj=0 role=fp ")
    _, out = report(not_injected)
    if status(out, "ii") != "STOP" or "hold no injected (inj=1) scene or first-person call" not in verdict_line(out, "ii"):
        fail("a phase that reached no call was not a STOP:\n%s" % verdict_line(out, "ii"))
    one_refused = text.replace(" inj=1 role=scene ", " inj=0 role=scene ", 1)
    _, out = report(one_refused)
    if status(out, "ii") != "WARN" or "were not injected (inj=0)" not in verdict_line(out, "ii"):
        fail("one scene call not injected was not a WARN:\n%s" % verdict_line(out, "ii"))
    after_tone = text.replace("tone=before inj=1 role=scene fl=0x1C>0x0 view=0x241dd00a000", "tone=after inj=1 role=scene fl=0x1C>0x0 view=0x241dd00a000", 1)
    _, out = report(after_tone)
    if status(out, "ii") != "WARN" or "came after the tone" not in verdict_line(out, "ii"):
        fail("an injected call after the tone was not a WARN:\n%s" % verdict_line(out, "ii"))
    no_hdr = re.sub(r" hdr=\d+x\d+ ", " hdr=0x0 ", text)
    _, out = report(no_hdr)
    if status(out, "ii") != "WARN" or "the render size is unknown" not in verdict_line(out, "ii"):
        fail("a log with no render size was not a WARN:\n%s" % verdict_line(out, "ii"))
    # The size the phase is in matters: the same rows against another render size do not carry that phase.
    small = text.replace("hdr=5040x2835", "hdr=2520x1417")
    _, out = report(small)
    if status(out, "ii") != "STOP":
        fail("rows measured against the wrong render size were not a STOP:\n%s" % verdict_line(out, "ii"))
    # An older census (no inj= on the calls) cannot be judged here.
    _, out = report(re.sub(r" inj=\d role=\S+", "", text))
    if status(out, "ii") != "n/a" or "no inj= token" not in verdict_line(out, "ii"):
        fail("calls with no inj= were judged:\n%s" % verdict_line(out, "ii"))

    # (iii) only kind 3 is injected: from the route's inj-kinds= and from the census's own call lines.
    kinds = text.replace("inj-kinds=3:6750", "inj-kinds=3:6745,other:5")
    _, out = report(kinds)
    if status(out, "iii") != "STOP" or "kind other x5" not in verdict_line(out, "iii"):
        fail("an injected kind other than 3 (route token) was not a STOP:\n%s" % verdict_line(out, "iii"))
    kind_five = mutate(text, "call frame=4 n=19 ", lambda line: line.replace("inj=0 role=-", "inj=1 role=-"))
    _, out = report(kind_five)
    if status(out, "iii") != "STOP" or "are not kind 3 (k5)" not in verdict_line(out, "iii"):
        fail("an injected kind-5 call (census line) was not a STOP:\n%s" % verdict_line(out, "iii"))
    # A kind-5 call that claims a scene role and an injection is judged as what it is: (iii) STOPs, and (ii) measures only kind-3 calls (the
    # eye call's rows carry the eye's own off-centre, not the phase: measured as a scene call they would be a second, false STOP).
    kind_five_scene = mutate(text, "call frame=4 n=19 ", lambda line: line.replace("inj=0 role=-", "inj=1 role=scene"))
    _, out = report(kind_five_scene)
    if status(out, "iii") != "STOP" or status(out, "ii") != "PASS":
        fail("an injected kind-5 call was measured as a kind-3 scene call in (ii), or was not a STOP in (iii):\n%s\n%s"
             % (verdict_line(out, "ii"), verdict_line(out, "iii")))
    nothing = text.replace("inj-kinds=3:6750", "inj-kinds=none")
    nothing = re.sub(r" inj=1 role=", " inj=0 role=", nothing)
    _, out = report(nothing)
    if status(out, "iii") != "WARN" or "nothing was injected" not in verdict_line(out, "iii"):
        fail("a flight that injected nothing was not a WARN:\n%s" % verdict_line(out, "iii"))
    # (iv) off-thread or unreadable, from the census's 5 s lines and the route's tokens.
    off_census = text.replace("off-thread=0 stale=0 inj-calls=6750", "off-thread=7 stale=0 inj-calls=6750")
    _, out = report(off_census)
    if status(out, "iv") != "STOP" or "the census counted 7 call(s) off the render thread" not in verdict_line(out, "iv"):
        fail("census off-thread calls were not a STOP:\n%s" % verdict_line(out, "iv"))
    off_route = text.replace("unreadable=0 off-thread=0 write-fail=0 inj-kinds=3:6750", "unreadable=0 off-thread=2 write-fail=0 inj-kinds=3:6750")
    _, out = report(off_route)
    if status(out, "iv") != "STOP" or "the route counted 2 off-thread call(s)" not in verdict_line(out, "iv"):
        fail("route off-thread calls were not a STOP:\n%s" % verdict_line(out, "iv"))
    unreadable = text.replace("unreadable=0 off-thread=0 write-fail=0 inj-kinds=3:6750", "unreadable=3 off-thread=0 write-fail=0 inj-kinds=3:6750")
    _, out = report(unreadable)
    if status(out, "iv") != "STOP" or "the route counted 3 call(s) whose kind could not be read" not in verdict_line(out, "iv"):
        fail("route unreadable calls were not a STOP:\n%s" % verdict_line(out, "iv"))
    unreadable_census = text.replace("kinds=1:900,3:7200,5:2700", "kinds=1:900,3:7200,5:2700,unreadable:4")
    _, out = report(unreadable_census)
    if status(out, "iv") != "STOP" or "the census counted 4 call(s) whose kind could not be read" not in verdict_line(out, "iv"):
        fail("census unreadable kinds were not a STOP:\n%s" % verdict_line(out, "iv"))
    thread_line = text + "[12:00:09.000] vr camera census: other-thread tid=4321 camera=0x241dc2e2960 kind=3 caller=+0x594E13 calls=57\n"
    rc, out = report(thread_line.replace("off-thread=0 stale=0 inj-calls=6750", "off-thread=57 stale=0 inj-calls=6750"))
    if "57 refresh call(s) ran on a thread other than the render thread" not in out or "1 other-thread entry" not in out or \
            "other-thread: tid 4321 camera 0x241DC2E2960 kind 3 caller 0x594E13 calls 57" not in out or status(out, "iv") != "STOP":
        fail("an off-thread call was not reported, listed and a STOP:\n%s" % out)
    # (v) the roles: an auxiliary call injected is a STOP; calls refused a WARN.
    aux_injected = text.replace(" inj=0 role=aux ", " inj=1 role=aux ")
    _, out = report(aux_injected)
    if status(out, "v") != "STOP" or "auxiliary call(s) were injected" not in verdict_line(out, "v"):
        fail("an injected auxiliary call was not a STOP:\n%s" % verdict_line(out, "v"))
    refused = text.replace("inj-refused=0 warming=0 aux=450", "inj-refused=4 warming=0 aux=450")
    _, out = report(refused)
    if status(out, "v") != "WARN" or "4 call(s) were refused for want of a write" not in verdict_line(out, "v"):
        fail("refused calls were not a WARN:\n%s" % verdict_line(out, "v"))
    write_failed = text.replace("write-fail=0 inj-kinds=3:6750", "write-fail=2 inj-kinds=3:6750")
    _, out = report(write_failed)
    if status(out, "v") != "WARN" or "2 write(s) failed" not in verdict_line(out, "v"):
        fail("failed writes were not a WARN:\n%s" % verdict_line(out, "v"))
    no_scene = text + "[12:00:10.000] vr world route: jitter is wanted but no scene camera call was injected (frame=4)\n"
    _, out = report(no_scene)
    if status(out, "v") != "WARN" or "the route logged: " not in verdict_line(out, "v"):
        fail("the route's own 'no scene camera call was injected' line was not a WARN:\n%s" % verdict_line(out, "v"))
    # (vi) the injection window: inj-shut is always 0; inj-unnamed is one per world-to-map change; the route's own STOP lines are quoted.
    shut = text.replace("pair-checked=448 pair-bad=0 inj-unnamed=0 inj-shut=0", "pair-checked=448 pair-bad=0 inj-unnamed=0 inj-shut=3")
    _, out = report(shut)
    if status(out, "vi") != "STOP" or "inj-shut=3: camera calls were INJECTED on a frame whose window the route had shut" not in verdict_line(out, "vi"):
        fail("inj-shut above 0 was not a STOP:\n%s" % verdict_line(out, "vi"))
    route_stop = text + "[12:00:10.000] vr world route: STOP at frame=812: camera calls were injected on a frame whose window the route had shut (decision: unnamed)\n"
    _, out = report(route_stop)
    if status(out, "vi") != "STOP" or "the route logged: " not in verdict_line(out, "vi") or \
            "the route's own STOP line: " in verdict_line(out, "vi"):
        fail("the route's shut-window STOP line was not quoted once as a STOP:\n%s" % verdict_line(out, "vi"))
    other_stop = text + "[12:00:10.000] vr world route: STOP at frame=813: some other route-side invariant failed\n"
    _, out = report(other_stop)
    if status(out, "vi") != "STOP" or "the route's own STOP line: " not in verdict_line(out, "vi"):
        fail("any other route STOP line was not quoted as a STOP:\n%s" % verdict_line(out, "vi"))
    kind_other = text + "[12:00:10.000] vr world route: STOP at frame=9: camera calls of a kind other than 3 were INJECTED (kind 5 x2)\n"
    _, out = report(kind_other)
    if status(out, "iii") != "STOP" or "the route logged: " not in verdict_line(out, "iii"):
        fail("the route's 'kind other than 3 were INJECTED' line was not a STOP in (iii):\n%s" % verdict_line(out, "iii"))
    if status(out, "vi") == "STOP":
        fail("the route's kind-other STOP line was counted against the injection window as well:\n%s" % verdict_line(out, "vi"))
    # The size the phase is in may come from the route's own JITTERED line when no 5 s line has an hdr=; hdr= wins when both are there.
    jittered = "[12:00:10.000] vr world route: the world is JITTERED from frame=13805: phase (0.2520,-0.1260) px in 5040x2835, 12 scene and 3 first-person " \
               "camera call(s) injected this frame (kind 3 only; the eye cameras, kind 5, are never written)\n"
    _, out = report(re.sub(r" hdr=\d+x\d+ ", " hdr=0x0 ", text) + jittered)
    if status(out, "ii") != "PASS" or "(from the route's `the world is JITTERED` line)" not in verdict_line(out, "ii"):
        fail("the render size was not taken from the route's JITTERED line when no 5 s line has one:\n%s" % verdict_line(out, "ii"))
    _, out = report(text + jittered.replace("5040x2835", "2520x1417"))
    if status(out, "ii") != "PASS" or "(from a `vr world route 5s:` line's hdr=)" not in verdict_line(out, "ii"):
        fail("the JITTERED line's size beat the route line's hdr=:\n%s" % verdict_line(out, "ii"))
    # The signatures the route excluded by role are quoted, so the roles that still need deciding are in the verdict.
    excluded = text + "[12:00:10.000] vr world route: camera call EXCLUDED, not a screen view: kind 3 aspect=1.0000 fov=1.5708 near=0.1000 far=50000.0 " \
                      "caller=+0x58DE73 (the screen's aspect is 1.7778; a screen view is within 4%): 6 call(s) so far, never injected\n"
    _, out = report(excluded)
    if "the route excluded 1 kind-3 call signature(s) by role and never injected them" not in out or \
            "aspect=1.0000 fov=1.5708 near=0.1000 far=50000.0 caller=+0x58DE73 (6 call(s) so far)" not in out:
        fail("the route's EXCLUDED line was not quoted as a note:\n%s" % out)
    fault = text.replace("jitter=on", "jitter=fault")
    _, out = report(fault)
    if status(out, "vi") != "STOP" or "jitter=fault in 1 route window(s)" not in verdict_line(out, "vi"):
        fail("jitter=fault was not a STOP:\n%s" % verdict_line(out, "vi"))
    no_hook = text.replace("jitter=on", "jitter=no-hook")
    _, out = report(no_hook)
    if status(out, "vi") != "WARN" or "jitter=no-hook in 1 route window(s)" not in verdict_line(out, "vi"):
        fail("jitter=no-hook was not a WARN:\n%s" % verdict_line(out, "vi"))
    # inj-unnamed: one per world-to-map change is expected, and the verdict can only compare it with the episodes the log shows.
    unnamed_no_episode = text.replace("pair-checked=448 pair-bad=0 inj-unnamed=0 inj-shut=0", "pair-checked=448 pair-bad=0 inj-unnamed=2 inj-shut=0")
    _, out = report(unnamed_no_episode)
    if status(out, "vi") != "WARN" or "inj-unnamed=2 exceeds the 0 map/menu/release episode(s) this log shows" not in verdict_line(out, "vi"):
        fail("inj-unnamed with no episode in the log was not a WARN:\n%s" % verdict_line(out, "vi"))
    with_unnamed_window = unnamed_no_episode.replace("inj-unnamed=2", "inj-unnamed=1").replace("jitter=on", "jitter=unnamed")
    _, out = report(with_unnamed_window)
    if status(out, "vi") != "PASS" or "inj-unnamed=1 (one per world-to-map change is expected" not in verdict_line(out, "vi") or \
            "this log shows 1 such episode(s)" not in verdict_line(out, "vi") or "jitter=unnamed in 1 window(s)" not in verdict_line(out, "vi"):
        fail("inj-unnamed explained by a jitter=unnamed episode was not a PASS with its note:\n%s" % verdict_line(out, "vi"))
    with_release = unnamed_no_episode + "[12:00:10.000] vr world route: RELEASED the world at frame=812 (frames-not-treated) after 13044 owned frame(s); the eye shift is back on\n" \
                   "[12:00:11.000] vr world route: RELEASED the world at frame=1812 (frames-not-treated) after 98 owned frame(s); the eye shift is back on\n"
    _, out = report(with_release)
    if status(out, "vi") != "PASS" or "this log shows 2 such episode(s)" not in verdict_line(out, "vi") or \
            "RELEASED 2 time(s)" not in out:
        fail("inj-unnamed explained by RELEASED lines was not a PASS:\n%s\n%s" % (verdict_line(out, "vi"), out))
    old_route = "\n".join(l.replace(" inj-unnamed=0 inj-shut=0", "") for l in text.split("\n"))
    _, out = report(old_route)
    if status(out, "vi") != "n/a" or "no route line has inj-shut= or inj-unnamed=" not in verdict_line(out, "vi") or \
            "stage 2 verdict: PASS (5 PASS, 0 WARN, 0 STOP, 1 n/a)" not in out:
        fail("a route line without inj-shut/inj-unnamed was not n/a (and did not leave the verdict PASS):\n%s" % out)
    # The route's jitter= has six values (on, off, idle, unnamed, no-hook, fault): all are counted, none crashes the report.
    six = text.replace("jitter=idle", "jitter=unnamed")
    rc, out = report(six)
    if rc != 0 or "world route: 2 5 s window(s); jitter: on x1, unnamed x1" not in out:
        fail("the route's jitter= states were not all counted:\n%s" % out)
    pair_bad = text.replace("pair-checked=448 pair-bad=0", "pair-checked=448 pair-bad=3")
    _, out = report(pair_bad)
    if "WARN  (note) the route's own pair check of the rows it believes: 448 checked, 3 inconsistent" not in squash(out).replace("WARN (note)", "WARN  (note)"):
        fail("the route's pair check failing was not a WARN:\n%s" % out)
    # The overall word.
    _, out = report(with_leak("2.0e-04"))
    if "stage 2 verdict: STOP (" not in out:
        fail("one STOP did not make the verdict STOP")
    _, out = report(with_leak("2.0e-06"))
    if "stage 2 verdict: WARN (" not in out:
        fail("one WARN and no STOP did not make the verdict WARN")

    # ---- the stage 2 experiment build: the refusal census, the key's state, the view, the weapon's role ----
    # The parser: the fixture's two refusal windows, each joined to the route and inject lines of its own window.
    rw = parse_refusal_windows(text)
    if len(rw) != 2 or [refusal_state(w) for w in rw] != ["idle", "measured"] or rw[1]["pixels"] != 1600300800 or rw[1]["refused"] != 58410978 or \
            rw[1]["causes"]["stale-refused"] != 40007520 or rw[1]["causes"]["masked"] != 800150 or rw[1]["causes"]["sentinel"] != 16003008 or \
            rw[1]["causes"]["range"] != 1600300 or rw[1]["kept"] != 0 or (rw[1]["check_ran"], rw[1]["check_skipped"]) != (0, 0) or \
            rw[1]["steady"] != "off" or rw[1]["view"] != "off" or \
            (rw[1]["w"], rw[1]["h"]) != (5040, 2835) or abs(rw[1]["pct"] - 3.650) > 1e-9 or rw[1]["every"] != 4 or rw[1]["dropped"] != 0 or \
            rw[1]["route"]["fp-mode"] != "0/450/0" or rw[1]["inject"]["fov"] != "0.8203..0.9831" or rw[0]["route"]["jitter"] != "idle" or \
            rw[0]["inject"]["fov"] != "-" or not rw[0]["census"]:
        fail("the fixture's refusal lines parsed as %r" % ([(w["ts"], refusal_state(w), w["pixels"], w["refused"]) for w in rw],))
    if len(parse_world_route(text)) != 2:
        fail("the refusal lines joined the route's own windows (they are a third line and a list of their own)")
    if (_cmodes("0/450/0"), _cmodes("0/0/12"), _cmodes("0/450"), _cmodes(None), _cmodes("a/b/c")) != ((0, 450, 0), (0, 0, 12), None, None, None):
        fail("_cmodes")
    if (_cfov("0.8203..0.9831"), _cfov("-"), _cfov(None), _cfov("1..2"), _cfov("0.8203.0.9831")) != ((0.8203, 0.9831), None, None, (1.0, 2.0), None):
        fail("_cfov")
    garbled = parse_refusal_windows("vr world route refusal 5s: census=on treated=x asked=0\n")
    if len(garbled) != 1 or garbled[0]["treated"] is not None or refusal_state(garbled[0]) != "unreadable":
        fail("a refusal counter that is not a number parsed as one, or its window was not 'unreadable'")
    if [e for e in route_events("[00:00:01.000] vr world route: steady-detail is ON from frame=100 (x)\n"
                                "[00:00:02.000] vr world route: the refusal view is ON from frame=200 (y)\n").keys()] != ["steady-detail", "refusal-view"]:
        fail("the route's steady-detail and refusal-view lines are not among its event markers")
    # The report on the fixture: the section, and the weapon's numbers in (v), whose old PASS text is unchanged.
    rc, out = report(text)
    flat = squash(out)
    for w in (
        "== refusal census (advanced.vr_camera_census: the route's own resolve, the prep's per-pixel classification, one sample in 4 of the "
        "resolves that ask; shares are of the pixels the samples examined) ==",
        "census=on: NOT MEASURED: the route treated no frame in this window (nothing was measured) | route state=observing jitter=idle "
        "fp-mode 0/0/0, inj-fp 0, struct fov -",
        "census=on: MEASURED 112 sample(s) of 5040x2835 (450 asked, 113 dispatched, 0 dropped), treated 450; pixels 1600300800; refused "
        "3.650% (58410978): stale-refused 2.500%, masked 0.050%, sentinel 1.000%, range 0.100%; stale-kept 0.000% | route state=owned jitter=on "
        "fp-mode 0/450/0, inj-fp 1350, struct fov 0.8203..0.9831",
        "totals, steady-detail=off: 1 measured window(s), 112 sample(s), pixels 1600300800, refused 3.650% (58410978): stale-refused 2.500%, masked "
        "0.050%, sentinel 1.000%, range 0.100%; stale-kept 0.000%; of the refused: stale-refused 68.5%, masked 1.4%, sentinel 27.4%, range 2.7%",
        "causes seen: stale-refused = the engine slot's depth was not the pixel's (a later draw overdrew it) and the steady-detail depth check, if it is on, "
        "did not confirm the camera term; masked = a rig record with no usable history",
        "refusal census: consistent (1 of 2 window(s) measured)",
        "; struct field of view 0.8203..0.9831 rad over 1 window(s); the weapon's fold-in ran in 450 frame(s) (mode 1: 450, mode 2: 0), about "
        "3.0 first-person call(s) credited a frame",
    ):
        if w not in flat:
            fail("the report on the fixture lacks %r:\n%s" % (w, out))
            break
    if "!! " in out or "refusal note:" in out:
        fail("the fixture's refusal census raised a finding:\n%s" % out)

    def refusal_line(**kw):
        """One refusal line of the fixture's measured window, with some tokens changed."""
        v = dict(census="on", every=4, treated=450, asked=450, sampled=113, read=112, dropped=0, size="5040x2835", pixels=1600300800,
                 refused=58410978, pct="3.650", stale=40007520, masked=800150, corrupt=0, sentinel=16003008, unreprojectable=0, camera=0,
                 range=1600300, depth=0, weapon=0, other=0, kept=0, ran=0, skipped=0, steady="off", view="off", skinned=None)
        v.update(kw)
        line = ("[12:00:09.000] vr world route refusal 5s: census=%(census)s every=%(every)d treated=%(treated)d asked=%(asked)d "
                "sampled=%(sampled)d read=%(read)d dropped=%(dropped)d size=%(size)s pixels=%(pixels)d refused=%(refused)d "
                "refused-pct=%(pct)s stale-refused=%(stale)d masked=%(masked)d corrupt=%(corrupt)d sentinel=%(sentinel)d "
                "unreprojectable=%(unreprojectable)d camera=%(camera)d range=%(range)d depth=%(depth)d weapon=%(weapon)d other=%(other)d "
                "stale-kept=%(kept)d depth-check=%(ran)d/%(skipped)d steady-detail=%(steady)s view=%(view)s") % v
        return line + ((" skinned-joined=%d" % v["skinned"]) if v["skinned"] is not None else "") + "\n"

    nothing = dict(refused=0, pct="0.000", stale=0, masked=0, sentinel=0, range=0)
    # F2 on foot: `skinned-joined=N` ends the line of a build that has it. It parses (None for a build that predates it), it is printed in the MEASURED line and it is
    # not part of the refused total (those pixels took their exact motion).
    with_skin = text + refusal_line(skinned=19, **nothing)
    wins = parse_refusal_windows(with_skin)
    _, out = report(with_skin)
    if wins[-1]["skinned"] != 19 or wins[0]["skinned"] != 0 or "skinned-joined 19 (F2 on foot" not in squash(out) or "!! " in out or \
            "refused 1.825% (58410978)" not in squash(out):
        fail("skinned-joined=N did not parse, print in the measured line, or stay out of the refused total:\n%s" % out)
    # (the fixture's own lines are the formatter's: they end skinned-joined=0; a log from a build before F2 on foot has no token at all)
    before_f2 = text.replace(" skinned-joined=0", "")
    if any(w["skinned"] is not None for w in parse_refusal_windows(before_f2 + refusal_line(**nothing))) or \
            "skinned-joined" in report(before_f2 + refusal_line(**nothing))[1]:
        fail("a build without the token printed a skinned-joined count")
    # "Ran, 0 refused" (pixels > 0, refused=0) is never the same text as "never ran" (treated=0, asked=0, read=0).
    _, out = report(text + refusal_line(**nothing))
    if "refused 0.000% (0): none refused; stale-kept 0.000%" not in squash(out) or "!! " in out or \
            "totals, steady-detail=off: 2 measured window(s), 224 sample(s), pixels 3200601600, refused 1.825% (58410978)" not in squash(out):
        fail("a window that measured and found nothing refused was not reported as measured, or the totals did not add the two:\n%s" % out)
    _, out = report(text + refusal_line(treated=450, asked=0, sampled=0, read=0, pixels=0, **nothing))
    if "NOT MEASURED: the route treated 450 frame(s) and none asked for the census" not in out or \
            "!! [12:00:09.000]: the route treated 450 frame(s) and none asked the resolver for the census" not in out or \
            "refusal census: WARN (1 finding(s) above)" not in out or "0.000% (0): none refused" in out.split("NOT MEASURED")[1].split("\n")[0]:
        fail("a route that treated frames and asked for no census was not called out as never having measured:\n%s" % out)
    _, out = report(text + refusal_line(treated=3, asked=3, sampled=0, read=0, pixels=0, **nothing))
    if "NOT MEASURED: 3 ask(s), fewer than one sample's worth (every 4)" not in out or "!! " in out:
        fail("fewer asks than one sample took was a WARN (it is a short window), or was not said:\n%s" % out)
    _, out = report(text + refusal_line(treated=450, asked=450, sampled=5, read=0, pixels=0, **nothing))
    if "NOT MEASURED: 5 sample(s) dispatched, none read back" not in out or "5 sample(s) were dispatched and none was read back" not in out:
        fail("samples dispatched and never read back were not a WARN:\n%s" % out)
    _, out = report(text.replace("treated=0 asked=0 sampled=0 read=0", "treated=450 asked=450 sampled=0 read=0").replace(
        "treated=450 asked=450 sampled=113 read=112 dropped=0 size=5040x2835 pixels=1600300800", "treated=450 asked=450 sampled=113 read=0 dropped=0 size=5040x2835 pixels=0"))
    if "the census never measured: the route treated 900 frame(s) over 2 window(s) and no sample was read back" not in out:
        fail("a census that never read a sample back was not a WARN:\n%s" % out)
    _, out = report("\n".join(l for l in text.split("\n") if "vr world route refusal" not in l))
    if "none: no `vr world route refusal 5s:` line in this log" not in out or "!! " in out or "stage 2 verdict: PASS (6 PASS" not in out:
        fail("a log without refusal lines (the census key off, or an older build) did not say so, or its verdict moved:\n%s" % out)
    # The key's state: each state is totalled on its own; with it on a stale slot is kept (last frame's depth confirmed the camera term) or
    # refused (it did not), and the depth check's own frames are counted. 38007520 + 2000000 = the fixture's 40007520 stale pixels.
    on = refusal_line(steady="on", stale=2000000, kept=38007520, refused=20403458, pct="1.275", read=112, ran=448, skipped=2)
    _, out = report(text + on)
    if "!! " in out or "totals, steady-detail=on: 1 measured window(s), 112 sample(s), pixels 1600300800, refused 1.275% (20403458)" not in squash(out) or \
            "stale-kept 2.375% (95.0% of the stale pixels); depth-check 448 ran, 2 skipped" not in squash(out) or \
            "stale-refused 0.125%" not in squash(out) or "totals, steady-detail=off: 1 measured window(s)" not in squash(out):
        fail("a window with the steady-detail key on was not totalled on its own, with its stale-kept share and the depth check's frames:\n%s" % out)
    _, out = report(text + refusal_line(steady="on", stale=40007520, kept=0, ran=0, skipped=450))
    if "steady-detail=on and the depth check never ran (0 ran, 450 skipped)" not in out or "refusal census: WARN" not in out:
        fail("the key on with a depth check that never ran (every frame skipped) was not a WARN:\n%s" % out)
    _, out = report(text + refusal_line(steady="on", stale=40007520, kept=0, ran=0, skipped=0))
    if "the resolver counted no depth-check frame in a window that treated 450 frame(s)" not in out or "refusal census: WARN" not in out:
        fail("the key on with no depth-check frame counted at all was not a WARN:\n%s" % out)
    _, out = report(text + refusal_line(steady="on", stale=40007520, kept=0, ran=448, skipped=2))
    if "the depth check ran in 448 frame(s) and kept no stale pixel (40007520 refused)" not in out or "!! " in out:
        fail("the depth check that ran and kept nothing was not a note (and only a note):\n%s" % out)
    _, out = report(text + refusal_line(steady="off", kept=9))
    if "9 stale pixel(s) were kept while steady-detail=off" not in out:
        fail("pixels kept with the key off were not a WARN:\n%s" % out)
    _, out = report(text + refusal_line(steady="off", ran=3, skipped=1))
    if "the depth check counted frames (3 ran, 1 skipped) while steady-detail=off" not in out:
        fail("depth-check frames with the key off were not a WARN:\n%s" % out)
    # The flight-3 build's spellings (`stale=`, `forgiven=`, no depth-check) still parse, as stale-refused and stale-kept, and its blanket rule is judged as it was.
    old_spelling = lambda line: line.replace("stale-refused=", "stale=").replace(" stale-kept=", " forgiven=").replace(" depth-check=0/0", "")
    flight3 = old_spelling(refusal_line(steady="on", stale=0, kept=40007520, refused=18403458, pct="1.150", read=112))
    if " depth-check=" in flight3 or " forgiven=40007520 " not in flight3 or " stale=0 " not in flight3:
        fail("the flight-3 line was not built")
    w3 = parse_refusal_windows(flight3)[0]
    if w3["kept"] != 40007520 or w3["causes"]["stale-refused"] != 0 or w3["check_ran"] is not None or w3["check_skipped"] is not None:
        fail("the flight-3 spellings did not parse as stale-kept and stale-refused with no depth-check: %r" % ((w3["kept"], w3["causes"]["stale-refused"], w3["check_ran"]),))
    _, out = report(text + flight3)
    if "!! " in out or "stale-kept 2.500%" not in squash(out) or "depth-check" in out.split("totals, steady-detail=on")[1].split("\n")[0]:
        fail("a flight-3 window with its key on was not read as stale-kept 2.500% with no depth-check, and no finding:\n%s" % out)
    _, out = report(text + old_spelling(refusal_line(steady="on", stale=5, kept=40007515)))
    if "steady-detail=on and 5 stale pixel(s) were still refused" not in out or "refusal census: WARN" not in out:
        fail("stale pixels still refused with the flight-3 build's key on were not a WARN:\n%s" % out)
    # The census key OFF with the steady-detail key on (its default): the route prints the line for the depth check's frame counts, and
    # such a window is `census-off`: not a census that failed to measure, nothing to WARN about unless the depth check's own counts are wrong.
    off_kw = dict(census="off", every=4, treated=450, asked=0, sampled=0, read=0, size="0x0", pixels=0, refused=0, pct="0.000",
                  stale=0, masked=0, sentinel=0, range=0, steady="on", kept=0, ran=448, skipped=2)
    cw = parse_refusal_windows(refusal_line(**off_kw))[0]
    if refusal_state(cw) != "census-off" or (cw["check_ran"], cw["check_skipped"]) != (448, 2) or cw["census"]:
        fail("a census=off line with nothing asked was not the census-off state: %r" % (refusal_state(cw),))
    _, out = report(text + refusal_line(**off_kw))
    if "!! " in out or "census off in 1 window(s), steady-detail=on (the line is printed for the key): the route treated 450 frame(s); " \
            "depth-check 448 ran, 2 skipped" not in squash(out) or \
            "refusal census: consistent (1 of 2 window(s) measured); 1 other window(s) had the census off" not in squash(out) or \
            "NOT MEASURED: the route treated 450" in out:
        fail("a census-off window beside a measured one was not summarised on its own line, or was called a failed census:\n%s" % out)
    _, out = report("\n".join(l for l in text.split("\n") if "vr world route refusal" not in l) + refusal_line(**off_kw) +
                    refusal_line(**dict(off_kw, ran=450, skipped=0)))
    if "!! " in out or "census off in 2 window(s), steady-detail=on" not in squash(out) or "depth-check 898 ran, 2 skipped" not in squash(out) or \
            "refusal census: the census key was off in all 2 window(s): nothing to measure" not in squash(out) or \
            "the census never measured" in out:
        fail("a log whose every refusal line is census-off was not read as 'nothing to measure' without a WARN:\n%s" % out)
    _, out = report(text + refusal_line(**dict(off_kw, ran=0, skipped=0)))
    if "steady-detail=on and the resolver counted no depth-check frame in a window that treated 450 frame(s)" not in out or \
            "refusal census: WARN" not in out:
        fail("a census-off window with the key on and no depth-check frame was not a WARN:\n%s" % out)
    _, out = report(text + refusal_line(**dict(off_kw, ran=0, skipped=450)))
    if "steady-detail=on and the depth check never ran (0 ran, 450 skipped)" not in out:
        fail("a census-off window whose depth check never ran was not a WARN:\n%s" % out)
    _, out = report(text + refusal_line(**dict(off_kw, steady="off", ran=3, skipped=1)))
    if "the depth check counted frames (3 ran, 1 skipped) while steady-detail=off" not in out:
        fail("a census-off window with depth-check frames and the key off was not a WARN:\n%s" % out)
    _, out = report(text + refusal_line(**dict(off_kw, treated=0, ran=0, skipped=0)))
    if "!! " in out:
        fail("a census-off window that treated no frame (nothing to check) was a finding:\n%s" % out)
    _, out = report(text + refusal_line(refused=1600300801))
    if "refused 1600300801 exceeds the pixels examined, 1600300800" not in out:
        fail("more pixels refused than examined was not a WARN:\n%s" % out)
    _, out = report(text + refusal_line(dropped=3, other=7))
    if "3 sample(s) were skipped because the read-back ring was full" not in out or "7 refused pixel(s) are of a class the census cannot name" not in out or \
            "refusal census: consistent" not in out:
        fail("dropped samples and unnamed causes were not notes (consistent, not WARN):\n%s" % out)
    _, out = report(text + refusal_line(view="on", steady="on", stale=0, kept=1, ran=1))
    if "the refusal view was painting: the headset showed the prep's classification, not the world" not in out:
        fail("a window with the refusal view on was not noted:\n%s" % out)
    _, out = report(text + "[00:00:10.000] vr world route: steady-detail is ON from frame=100 (the line an older build printed when it read its key): x\n"
                    "[00:00:11.000] vr world route: the refusal view is ON from frame=200 (advanced.temporal_aa_debug = motion_source): y\n")
    if "route log: [00:00:10.000] vr world route: steady-detail is ON from frame=100" not in out or "route log: [00:00:11.000] vr world route: the refusal view is ON" not in out:
        fail("the route's state lines were not quoted in the refusal section:\n%s" % out)
    # The weapon's role in (v): flight 2's picture (inj-fp 0 with the fold-in in mode 2 on every weapon frame) is a WARN and says what the
    # field-of-view range makes of it; a credited weapon with a mode-1 fold-in is the fixture's PASS.
    fl2 = text.replace("inj-fp=1350", "inj-fp=0").replace("fp-mode=0/450/0", "fp-mode=0/0/450")
    _, out = report(fl2.replace("fov=0.8203..0.9831", "fov=-"))
    line = verdict_line(out, "v")
    if status(out, "v") != "WARN" or "no first-person call was credited (inj-fp 0) although the weapon's fold-in ran in 450 frame(s) (mode 1: 0, mode 2: 450)" not in line or \
            "no inject line carries a fov= range" not in line:
        fail("flight 2's weapon (inj-fp 0, mode 2, no fov=) was not a WARN naming the missing field-of-view range:\n%s" % line)
    _, out = report(fl2)
    if status(out, "v") != "WARN" or "the struct carries two fields of view (0.8203 and 0.9831 rad)" not in verdict_line(out, "v") or \
            "a fault in the field-of-view test" not in verdict_line(out, "v"):
        fail("inj-fp 0 with two fields of view in the struct was not a WARN naming a fault in the role test:\n%s" % verdict_line(out, "v"))
    _, out = report(fl2.replace("fov=0.8203..0.9831", "fov=0.9831..0.9831"))
    if status(out, "v") != "WARN" or "the struct carries one field of view (0.9831 rad)" not in verdict_line(out, "v"):
        fail("inj-fp 0 with one field of view was not a WARN saying the weapon cannot be told by it:\n%s" % verdict_line(out, "v"))
    _, out = report(text.replace("fp-mode=0/450/0", "fp-mode=0/0/450"))
    if status(out, "v") != "WARN" or "yet the fold-in ran only in mode 2 (450 frame(s), mode 1 in none)" not in verdict_line(out, "v"):
        fail("a credited weapon whose fold-in never ran in mode 1 was not a WARN:\n%s" % verdict_line(out, "v"))
    _, out = report(text.replace("inj-scene=5400", "inj-scene=1000"))
    if status(out, "v") != "WARN" or "as many first-person calls as scene calls (1350 against 1000" not in verdict_line(out, "v"):
        fail("more first-person calls than scene calls was not a WARN:\n%s" % verdict_line(out, "v"))
    _, out = report(text.replace("inj-fp=1350", "inj-fp=0").replace("fp-mode=0/450/0", "fp-mode=450/0/0"))
    if status(out, "v") != "PASS":
        fail("no weapon drawn (the fold-in never ran) with inj-fp 0 is not a fault:\n%s" % verdict_line(out, "v"))
    _, out = report(fl2 + "[12:00:10.000] vr world route: jitter is wanted but no scene camera call was injected at frame=4 (warming 4)\n")
    if status(out, "v") != "WARN" or "the route logged: " not in verdict_line(out, "v"):
        fail("the route's no-scene line did not keep its place ahead of the weapon's checks:\n%s" % verdict_line(out, "v"))

    # ---- older logs: missing tokens are said, not crashed on ----
    older = re.sub(r" inj=\d role=\S+", "", text)
    older = re.sub(r" phase=\S+ calls=", " calls=", older)
    older = re.sub(r" phase=\S+ draw=", " draw=", older)
    older = re.sub(r" inj-calls=\d+", "", older)
    older = "\n".join(l for l in older.split("\n") if "vr world route" not in l)
    rc, out = report(older)
    if rc != 0 or "stage 2 verdict: n/a" not in out or status(out, "i") != "n/a" or status(out, "ii") != "n/a" or \
            "no `vr world route 5s:` line in this log" not in out or "this log predates stage 2" not in out:
        fail("an older log (no phase, inj, role or route lines) was not reported as not judged:\n%s" % out)
    # ... and its roles still come from the calls: the world camera is found, and (C), (D), (F) are answered.
    if "world camera: 0x241DC2E2960" not in out or "(C) place in the frame: every refresh of a world-side camera precedes every" not in out \
            or "not enough calls logged to say" in out:
        fail("an older log's roles were not read from its calls:\n%s" % out)
    # The route tokens on ONE line (the first wording of the route's format), and tokens missing from a window.
    lines = text.split("\n")
    merged, i = [], 0
    while i < len(lines):
        if "vr world route 5s:" in lines[i] and i + 1 < len(lines) and "vr world route inject 5s:" in lines[i + 1]:
            inject_tokens = lines[i + 1].split("vr world route inject 5s: ", 1)[1]
            merged.append(lines[i].replace(" last-trigger=", " " + inject_tokens + " last-trigger=", 1))
            i += 2
        else:
            merged.append(lines[i])
            i += 1
    rc, out = report("\n".join(merged))
    if rc != 0 or "vr world route inject" in "\n".join(merged) or status(out, "iii") != "PASS" or \
            "stage 2 verdict: PASS (6 PASS" not in out:
        fail("the route tokens all on one line (the first wording of the format) were not read:\n%s" % out)
    partial ="\n".join(l.replace(" jitter=on", "") if "vr world route 5s:" in l else l for l in text.split("\n"))
    rc, out = report(partial)
    if rc != 0 or "0 with jitter=on" not in out:
        fail("a route line missing its jitter= token broke the report:\n%s" % out)
    inject_only = "\n".join(l for l in text.split("\n") if "vr world route 5s:" not in l)
    rc, out = report(inject_only)
    if rc != 0 or status(out, "iii") != "PASS" or "hdr" not in verdict_line(out, "ii") and status(out, "ii") != "WARN":
        fail("inject lines with no route line were not read (the size is then unknown):\n%s" % verdict_line(out, "ii"))
    route_only = "\n".join(l for l in text.split("\n") if "vr world route inject 5s:" not in l)
    rc, out = report(route_only)
    if rc != 0 or status(out, "iii") != "PASS" or status(out, "ii") != "PASS":
        fail("route lines with no inject line broke the verdict (the census's own call lines still say what was injected):\n%s" % out)
    rows_joined = parse_world_route("[00:00:01.000] vr world route inject 5s: inj-scene=1\n[00:00:02.000] vr world route inject 5s: inj-scene=2\n"
                                    "[00:00:03.000] vr world route 5s: jitter=on hdr=10x20\n[00:00:03.001] vr world route inject 5s: inj-scene=3\n")
    if [(w["route"] is not None, w["inject"] is not None) for w in rows_joined] != [(False, True), (False, True), (True, True)]:
        fail("route and inject lines were not joined by order: %r" % (rows_joined,))

    # ---- the pieces ----
    rows = [{"n": i + 1, "kind": k, "caller": cl, "tone": t} for i, (k, cl, t) in enumerate(
        [(3, 1, "before")] * 5 + [(3, 2, "before")] * 2 + [(3, 1, "before"), (3, 1, "after"), (3, 1, "after")])]
    runs = census_runs(rows)
    if runs != [(3, 1, "before", 5, 1, 5), (3, 2, "before", 2, 6, 7), (3, 1, "before", 1, 8, 8), (3, 1, "after", 2, 9, 10)]:
        fail("census_runs: %r" % (runs,))
    if not _cequal(1.0, 1.00001) or _cequal(1.0, 1.001) or _cequal((0.0, 0.0), (0.0, 0.01)) or not _cequal((0.0, 1.0), (0.0, 1.0)) \
            or _cequal(float("nan"), float("nan")) or not _cequal(3, 3) or _cequal(3, 4) or not _cequal(None, None):
        fail("_cequal")
    if _cf("nan") == _cf("nan") or _cf("-") == _cf("-") or _cf("1.5") != 1.5 or _chex("+0x594E13") != 0x594E13 or \
            _chex("0x241dc2e2960") != 0x241DC2E2960 or _chex("-") is not None or _ctuple("(1,2)") != (1.0, 2.0) or \
            _ctuple("-") is not None or _clist("[1,2,3]") != [1.0, 2.0, 3.0] or _clist("-") is not None:
        fail("the value parsers")
    # The sign of the shift in the rows: the fit names the convention the rows carry, whichever it is.
    frustum = [-1.2, 0.7, -0.9, 1.1]
    shift = (0.001, 0.0005)
    for label, sx, sy in (("(+shift.x, +shift.y)", 1, 1), ("(+shift.x, -shift.y)", 1, -1),
                          ("(-shift.x, +shift.y)", -1, 1), ("(-shift.x, -shift.y)", -1, -1)):
        meas = census_expected_measure(frustum, sx * shift[0], sy * shift[1])
        fit = census_shift_fit(meas, frustum, shift)
        if not fit or label not in fit[0] or fit[1] > 1e-12 or not fit[3] > 1e-5:
            fail("census_shift_fit did not find %s: %r" % (label, fit))
    meas = census_expected_measure(frustum, 0.0, 0.0)
    fit = census_shift_fit(meas, frustum, shift)
    if not fit or "unshifted" not in fit[0]:
        fail("census_shift_fit did not find the unshifted frustum: %r" % (fit,))
    if census_expected_measure([1, 1, 0, 1], 0, 0) is not None or census_shift_fit(None, frustum, shift) is not None:
        fail("a degenerate window or a missing measurement produced a fit")
    # A frame with more runs than the report shows names how many it left out.
    many = []
    for i in range(40):
        many.append("[00:00:00.%03d] vr camera census: call frame=8 n=%d camera=0x10 kind=3 caller=+0x%X draw=%d tone=before "
                    "fl=0x1C>0x0 rows=-" % (i, i + 1, 0x594E13 + (i & 1), 100 + i))
    many.insert(0, "[00:00:00.000] vr camera census: sequence frame=8 index=1/3 calls=40 recorded=40 truncated=0")
    rc, out = report("\n".join(many) + "\n")
    if "... 16 more run(s) ..." not in out or out.count("(calls ") != CENSUS_RUNS_SHOWN:
        fail("a 40-run frame did not show %d runs and name the 16 it left out:\n%s" % (CENSUS_RUNS_SHOWN, out))
    # No field separates, and the tone flag is unavailable: both are said, not guessed.
    same = "\n".join([
        "vr camera census: camera=0x100 kind=3 caller=+0x594E13 thread=owner aspect=1 near=0.025 far=50000 fov=1 bound=(0,0) "
        "offcentre=(0,0) viewport=(100,100) tan=(-1,1,-1,1) first-call=1 draw=1 tone=none frame=1",
        "vr camera census: camera=0x200 kind=3 caller=+0x594E13 thread=owner aspect=1 near=0.025 far=50000 fov=1 bound=(0,0) "
        "offcentre=(0,0) viewport=(100,100) tan=(-1,1,-1,1) first-call=2 draw=2 tone=none frame=1",
        "vr camera census: sequence frame=1 index=1/3 calls=2 recorded=2 truncated=0",
        "vr camera census: call frame=1 n=1 camera=0x100 kind=3 caller=+0x594E13 draw=1 tone=none fl=0x1C>0x0 rows=[1,0,0,0,0,1,0,0,0,0,1,0,0,0,0.1,0]",
        "vr camera census: call frame=1 n=2 camera=0x200 kind=3 caller=+0x594E13 draw=2 tone=none fl=0x1C>0x0 rows=[2,0,0,0,0,1,0,0,0,0,1,0,0,0,0.1,0]",
        "vr camera census: eye=0 frame=1 draw=2 b1=0x5 first=0 bytes=5376 rows=[2,0,0,0,0,1,0,0,0,0,1,0,0,0,0.1,0] meas=(0,0)",
        ""])
    rc, out = report(same)
    if "(A) call signature: no single field" not in out:
        fail("two cameras with one signature were separated by a field:\n%s" % out)
    if "eye camera(s): 0x200; world camera: 0x100" not in out:
        fail("the joined camera was not the eye and the other the world:\n%s" % out)
    if "the tone flag is unavailable" not in out:
        fail("calls with tone=none were judged by a tone flag they do not have:\n%s" % out)
    if "(F) view" not in out or "not enough calls logged to say" not in out:
        fail("calls with no view were judged by one:\n%s" % out)
    if "(D) caller: eye camera(s) call from +0x594E13; the world camera from +0x594E13; shared: +0x594E13" not in out:
        fail("two cameras calling from one site were separated by the caller:\n%s" % out)

    # ---- the episodes (Phase 0): the value parsers, the parser, the report, and logs altered to take away each thing the section depends on ----
    if (_ckindmap("0:12,3:290,5:10"), _ckindmap("-"), _ckindmap("1:2,other:3,unreadable:4"), _ckindmap("x"), _ckindmap(None)) != \
            ({0: 12, 3: 290, 5: 10}, {}, {1: 2, "other": 3, "unreadable": 4}, None, None):
        fail("_ckindmap")
    if (_ccallers("+0x58DE73:300,+0x594E13:130,+more:2"), _ccallers("-"), _ccallers("+0x1:1,"), _ccallers(None)) != \
            (([(0x58DE73, 300), (0x594E13, 130)], 2), ([], 0), None, None):
        fail("_ccallers")
    if (_ctrigger("key-on"), _ctrigger("gui:0>6"), _ctrigger("naming:unnamed>named"), _ctrigger("foot:no"), _ctrigger(""), _ctrigger("bogus:1>2"),
            _ctrigger("zzz"), _ctrigger("gui:>6"), _ctrigger(None)) != \
            (("key-on", None, None), ("gui", "0", "6"), ("naming", "unnamed", "named"), None, None, None, None, None, None):
        fail("_ctrigger")
    if (_cmatch("98,101"), _cmatch("1,2,3,4,5,6+48"), _cmatch("-"), _cmatch("1,x"), _cmatch("3+"), _cmatch(None)) != \
            (([98, 101], 0), ([1, 2, 3, 4, 5, 6], 48), ([], 0), None, None, None):
        fail("_cmatch")
    if _cbins("1:1,2:0,3:0,4-8:0,9-30:0,31-89:1,90+:1") != {"1": 1, "2": 0, "3": 0, "4-8": 0, "9-30": 0, "31-89": 1, "90+": 1} or \
            _cbins("1:1,2:0") is not None or _cbins("2:1,1:0,3:0,4-8:0,9-30:0,31-89:0,90+:0") is not None or _cbins(None) is not None:
        fail("_cbins")
    if (_cpair_us("25/30"), _cpair_us("0.31/4.2"), _cpair_us("-"), _cpair_us("a/b"), _cpair_us(None)) != ((25.0, 30.0), (0.31, 4.2), None, None, None):
        fail("_cpair_us")

    c = parse_camera_census(text)
    eps = c["episodes"]
    if len(c["windows"]) != 2 or (len(eps), len(c["episode_counters"]), len(c["runs"]), len(c["detour"])) != (3, 2, 2, 2) or c["unparsed"]:
        fail("the fixture parsed as %r episodes, counters, runs, detour lines (want 3, 2, 2, 2), %d 5 s windows (want 2: the companions are not windows), %d unparsed"
             % ((len(eps), len(c["episode_counters"]), len(c["runs"]), len(c["detour"])), len(c["windows"]), c["unparsed"]))
    e1, e2, e3 = eps
    if (e1["n"], e1["of"], e1["frame"], e1["armed"], e1["trigger"], e1["foot"], e1["gui"], e1["named"], e1["phase_state"]) != \
            (1, 10, 31, 1, ("key-on", None, None), "no", 0, False, "off") or \
            (e2["trigger"], e2["gui"], e2["armed"], e2["frame"]) != (("gui", "0", "6"), 6, 1530, 1560) or \
            (e3["trigger"], e3["foot"], e3["gui"], e3["phase"], e3["named"]) != (("naming", "named", "unnamed"), "yes", None, (0.252, -0.126), False):
        fail("the episode headers parsed wrong: %r" % ([{k: v for k, v in e.items() if k not in ("rows", "joins", "pass")} for e in eps],))
    if (e1["calls"], e1["recorded"], e1["printed"], len(e1["rows"])) != (14, 14, 14, 14) or e1["kinds"] != {1: 2, 3: 6, 5: 6} or \
            sorted(e1["callers"]) != [(0x58DE73, 2), (0x594E13, 4), (0x594EAB, 4), (0x594FE1, 4)] or e1["callers_more"] != 0 or \
            [r["kind"] for r in e1["rows"]] != [1, 1] + [3] * 6 + [5] * 6 or any(r["frame"] != 31 for r in e1["rows"]) or \
            len(c["sequences"]) != 3:
        fail("an episode's call lines did not follow its header, or leaked into the first three sequences: %r" % ([r["kind"] for r in e1["rows"]],))
    j1 = e1["joins"]
    if [(j["sig"], j["depth"], j["eye"], j["w"], j["h"], j["draw"], j["draws"], j["dw"], j["b1"], j["bytes"], j["read"], j["why"], j["match"]) for j in j1] != \
            [(1, "eye", 0, 2620, 2533, 8210, 1432, True, 0x1EB2E751E20, 5376, True, None, [9, 10, 11]),
             (2, "eye", 0, 2620, 2533, 8650, 61, False, 0x1EB2E751E20, 5376, False, "skip", None),
             (3, "eye", 1, 2620, 2533, 8215, 1432, True, 0x1EB2E751E20, 5376, True, None, [12, 13, 14])] or \
            [len(j["rows"]) if j["rows"] else None for j in j1] != [16, None, 16] or j1[0]["vs"] != 0x5C36AF051B98B9F1 or j1[0]["ps"] != 0xCFE84157BC76E921:
        fail("the join signatures of episode 1 parsed wrong: %r" % (j1,))
    j3 = e3["joins"]
    if [(j["depth"], j["eye"], j["match"], j["b1"], j["why"]) for j in j3] != \
            [("screen", None, [], 0x1EB2E751E20, None), ("screen", None, None, 0x1EB2E751E20, "skip"), ("eye", None, None, None, "no-b1")]:
        fail("the join signatures of episode 3 (a screen depth, no match, a skipped signature, no b1) parsed wrong: %r" % (j3,))
    p1, p2, p3 = e1["pass"], e2["pass"], e3["pass"]
    if (p1["valid"], p1["bound"], len(p1["rows"]), p1["match"], p1["how"], p1["nearest"], p1["diff"]) != (True, True, 12, [9, 10, 11], "identity", 9, 0.0) or \
            (p2["valid"], p2["bound"], p2["match"], p2["how"], p2["nearest"]) != (True, False, [5, 6, 7, 8, 9], "transpose", 5) or \
            (p3["valid"], p3["rows"], p3["match"], p3["how"]) != (False, None, [], None):
        fail("the pass's rows parsed wrong: %r" % ((p1, p2, p3),))
    r2 = c["runs"][1]
    if r2["frames"] != 413 or r2["named"]["31-89"] != 1 or r2["unnamed"]["4-8"] != 1 or (r2["longest_named"], r2["longest_unnamed"]) != (211, 7) or \
            r2["open"] != ("named", 211) or c["runs"][0]["open"] is not None:
        fail("a runs line parsed wrong: %r" % (r2,))
    d2 = c["detour"][1]
    if (d2["every"], d2["frames"], d2["calls"], d2["sampled"]) != (16, 450, 10800, 676) or abs(d2["est_ms"] - 0.155) > 1e-9 or \
            d2["modes"]["inj"]["calls"] != 6750 or d2["modes"]["obs"]["pre"] != (3.35, 3.6) or d2["modes"]["inj"]["post"] != (2.7, 2.8) or c["detour"][0]["modes"]["inj"]["pre"] is not None:
        fail("a detour line parsed wrong: %r" % (d2,))
    if [e["join_draws"] for e in eps] != [{"seen": 3100, "relevant": 2925, "views": 4, "signatures": 3}, {"seen": 2100, "relevant": 1800, "views": 3, "signatures": 2},
                                          {"seen": 905, "relevant": 337, "views": 5, "signatures": 3}]:
        fail("the join-draws lines parsed wrong: %r" % ([e["join_draws"] for e in eps],))
    k2 = c["episode_counters"][1]
    if (k2["taken"], k2["of"], k2["triggers"], k2["skipped"], k2["state"], k2["trigger"]) != (3, 10, 5, 2, "idle", None):
        fail("an episodes (counters) line parsed wrong: %r" % (k2,))
    armed = parse_camera_census("[00:00:01.000] vr camera census: episodes windows=1 taken=2/10 triggers=3 skipped=0 state=armed trigger=gui:0>6 armed=100 sample=130\n")
    if armed["episode_counters"][0]["trigger"] != ("gui", "0", "6") or (armed["episode_counters"][0]["armed"], armed["episode_counters"][0]["sample"]) != (100, 130):
        fail("an armed episodes line parsed wrong: %r" % (armed["episode_counters"],))
    # A line cut short or garbled is counted and skipped; a join line of an episode whose header is not there has nowhere to go.
    bad = parse_camera_census("vr camera census: episode frame=5 n=1/10 trigger=zzz armed=1 foot=no gui=- named=0 phase=- calls=1 recorded=1 printed=1 kinds=- callers=-\n"
                              "vr camera census: join ep=9 sig=1 depth=eye eye=0 size=2620x2533 draw=1 draws=1 vs=0x1 ps=0x1 dw=yes b1=- first=- bytes=- rows=- why=no-b1\n"
                              "vr camera census: pass-rows ep=9 frame=5 valid=0 bound=- rows=- axes-match=- how=- nearest=- diff=-\n"
                              "vr camera census: join-rows ep=9 sig=1 rows=[1,2]\n"
                              "vr camera census: join-draws ep=9 seen=1 relevant=1 views=1 signatures=1\n")
    if bad["unparsed"] != 5 or bad["episodes"]:
        fail("a garbled header and lines of an episode with no header were not skipped and counted: %r" % (bad["unparsed"],))

    # ---- the report ----
    rc, out = report(text)
    flat = squash(out)
    want_episodes = [
        "== episodes (3 printed; the last `episodes` line: 3 taken of 10, 5 trigger(s), 2 skipped) ==",
        "2 trigger(s) arrived while an episode was armed (or after the session's ten): counted, never sampled",
        "episode 1/10: frame 31, trigger key-on (armed at frame 1); journal foot=no, GuiFocus 0, naming unnamed, the route was not jittering (phase=-)",
        "14 call(s), 14 recorded, 14 printed; by kind: k1 x2, k3 x6, k5 x6; by caller: +0x594E13 x4, +0x594EAB x4, +0x594FE1 x4, +0x58DE73 x2",
        "k5 +0x594E13 after x1 (calls 9..9) camera 0x241DF6D0BB0",
        "join: 3 signature(s); the per-draw hook was handed 3100 draw(s), 2925 of them into a screen- or eye-sized depth, 4 depth view(s) resolved (a signature is a depth, a vertex "
        "shader and a pixel shader; the first of each depth has its rows read back)",
        "join: 3 signature(s); the per-draw hook was handed 905 draw(s), 337 of them into a screen- or eye-sized depth, 5 depth view(s) resolved",
        "eye 0 2620x2533: first draw 8210 (1432 draw(s)), vs 0x5C36AF051B98B9F1 ps 0xCFE84157BC76E921, depth write yes, b1 0x1EB2E751E20 (5376 bytes, first constant 0)",
        "rows 270..273 equal the composed rows of: camera 0x241DF6D0BB0 (kind 5) caller +0x594E13, +0x594EAB, +0x594FE1, 3 call(s) n=9/10/11, view 0x241DD00E000, tone after",
        "depth write no, b1 0x1EB2E751E20 (5376 bytes, first constant 0); rows not read (skip)",
        "the pass's chosen rows (bound block yes) equal the view axes (as they are) of: camera 0x241DF6D0BB0 (kind 5) caller +0x594E13, +0x594EAB, +0x594FE1, 3 call(s) n=9/10/11",
        "-> the rows are an eye camera's (kind 5)",
        "episode 2/10: frame 1560, trigger gui 0>6 (armed at frame 1530); journal foot=no, GuiFocus 6, naming unnamed",
        "eye 0 2620x2533: first draw 8300 (900 draw(s)), vs 0x5C36AF051B98B9F1 ps 0xCFE84157BC76E921, depth write yes, b1 0x1EB2E751E20 (5376 bytes, first constant 0); rows not read (map)",
        "the pass's chosen rows (bound block no) equal the view axes (transposed) of: camera 0x241DC2E2960 (kind 3) caller +0x594E13, +0x594EAB, +0x594FE1, 5 call(s) n=5/6/7/8/9",
        "-> the chooser STRAYS: the rows equal a kind 3 camera's axes, not an eye camera's (this frame has 6 kind-5 call(s))",
        "episode 3/10: frame 6120, trigger naming named>unnamed (armed at frame 6090); journal foot=yes, GuiFocus unknown, naming unnamed, phase (0.2520, -0.1260) px",
        "screen 5040x2835: first draw 41 (22 draw(s)), vs 0xDFED8E1C9E191BEC ps 0x143AAE0597E2F7BF, depth write yes, b1 0x1EB2E751E20 (5376 bytes, first constant 0)",
        "rows 270..273 equal the composed rows of NO call of this frame: whatever composed them is not at the refresh (or came from another frame)",
        "eye ? 2620x2533: first draw 90 (4 draw(s)), vs 0x11A2B3C4D5E6F708 ps 0xCFE84157BC76E921, depth write yes, b1 not bound; rows not read (no-b1)",
        "the pass's chosen rows: none (valid=0: the pass chose no camera rows this frame: it is off, or nothing was treated)",
        "== reading the episodes (facts for H1, H2 and H3, no verdict) ==",
        "H1 (on-foot maps and menus never name a source): 1 on-foot episode(s) (journal foot=yes), 1 of them unnamed (no draw named the 2D screen's source): episode(s) 3",
        "episode 3: 2 signature(s) drew into a screen-sized depth; the first draw's rows equal no printed call's composed rows",
        "H3 (the cockpit's maps are driven by kind-5 eye cameras' rows): 2 aboard episode(s) (journal foot=no or off)",
        "episode 1 (key-on, GuiFocus 0): eye 0 depth rows equal kind 5 call(s); eye 1 depth rows equal kind 5 call(s); the pass's rows equal kind 5 call(s)' axes",
        "episode 2 (gui 0>6, GuiFocus 6): eye 1 depth rows equal kind 5 call(s); the pass's rows equal kind 3 call(s)' axes",
        "== on-foot naming runs (the `runs` lines summed over 2 5 s window(s); frames the journal says on foot: 413) ==",
        "named 1 0 0 0 0 1 1",
        "unnamed 2 1 0 1 0 0 0",
        "longest named run 211 frame(s), longest unnamed run 7 frame(s)",
        "H2 (the longest unnamed run in an on-foot world stays under 3 frames): 1 unnamed run(s) of 3 frames or more, and 3 of 1 or 2",
        "the run open at the last window: named for 211 frame(s) so far",
        "== the detour's CPU (the observer's two halves, 1 call in 16 timed; 2 `detour` line(s) over 2 5 s window(s)) ==",
        "898 frame(s), 52588 refresh call(s), 3289 timed",
        "estimated ms a frame: mean 0.344 over 2 window line(s) (lowest 0.155, highest 0.533); calls a frame: 58.6",
        "observed calls (the detour did not inject for them): 45838 call(s), 2866 timed; pre half: mean 3.32 us, longest 52 us; post half: mean 2.41 us, longest 2.7 us",
        "injected calls (the route's phase was written for them): 6750 call(s), 423 timed; pre half: mean 4.08 us, longest 61 us; post half: mean 2.7 us, longest 2.8 us",
    ]
    for w in want_episodes:
        if w not in flat:
            fail("the report on the fixture lacks %r:\n%s" % (w, out[out.find("== episodes"):out.find("== stage 2 verdict")]))
            break
    if out.index("== episodes") > out.index("== stage 2 verdict") or "== stage 2 verdict" not in out or "stage 2 verdict: PASS (6 PASS" not in out:
        fail("the episodes come after the stage 2 verdict, or the verdict changed with them in the log:\n%s" % out[-600:])

    def drop(log, prefix):
        return mutate(log, prefix, lambda line: "")

    episode_calls = ("call frame=31 ", "call frame=1560 ", "call frame=6120 ")   # the fixture's three episodes' call lines (no sequence is made of them)

    # A log with none of the new lines is the report it was: no episodes section, no sections of runs and detour, and the summary line without a count of episodes.
    legacy = text
    for prefix in ("episode ", "join ", "join-rows ", "join-more ", "join-draws ", "pass-rows ", "episodes ", "runs ", "detour ") + episode_calls:
        legacy = drop(legacy, prefix)
    rc, out = report(legacy)
    if rc != 0 or "== episodes" in out or "== on-foot naming runs" in out or "== the detour's CPU" in out or "episode(s)" in out.split("\n")[0] or \
            "stage 2 verdict: PASS (6 PASS" not in out or "3 call sequence(s), 8 eye draw(s)" not in out:
        fail("a census log with none of the episode lines (an older census) did not report exactly as before:\n%s" % out)
    # No `episodes` counters line: the section says so (and still prints the episodes).
    _, out = report(drop(text, "episodes "))
    if "no `vr camera census: episodes` line (the window's counters)" not in out or "== episodes (3 printed)" not in out:
        fail("a log with no episode counters line did not say so:\n%s" % out)
    # An episode armed and never printed (the log ends before its frame): the counters line says it, with its trigger and the frame it samples.
    armed_log = text
    for prefix in ("episode ", "join ", "join-rows ", "join-draws ", "pass-rows ") + episode_calls:
        armed_log = drop(armed_log, prefix)
    armed_log += \
        "[12:00:09.000] vr camera census: episodes windows=1 taken=4/10 triggers=6 skipped=2 state=armed trigger=gui:6>0 armed=7000 sample=7030\n"
    _, out = report(armed_log)
    if "4 episode(s) were armed and not printed" not in out or "one is armed now (trigger gui 6>0, armed at frame 7000, samples frame 7030)" not in out or \
            "none: no episode was sampled" not in out:
        fail("an armed episode that never printed was not reported:\n%s" % out)
    # The pass's rows line is missing, or valid=0.
    _, out = report(drop(text, "pass-rows ep=1 "))
    if "episode 1/10" not in out or "the pass's chosen rows: no `pass-rows` line for this episode" not in squash(out):
        fail("an episode with no pass-rows line did not say so:\n%s" % out)
    # The join's hook was never handed a draw (seen=0) is not 'no draw joined': the first says the route's per-draw path never reached the census; the second counts what was seen.
    never = sub(drop(drop(drop(text, "join ep=3 "), "join-rows ep=3 "), "join-more ep=3 "), "join-draws ep=3 seen=905 relevant=337 views=5 signatures=3",
                "join-draws ep=3 seen=0 relevant=0 views=0 signatures=0")
    _, out = report(never)
    part = squash(out)[squash(out).find("episode 3/10"):]
    if "the per-draw hook was handed 0 draw(s), 0 of them into a screen- or eye-sized depth, 0 depth view(s) resolved" not in part or \
            "!! the join's per-draw hook was handed NO draw in the sampled frame" not in part or "none: the hook saw" in part:
        fail("an episode whose join hook never ran was not told from one with no draw joined:\n%s" % part[:1200])
    quiet = sub(drop(drop(drop(text, "join ep=3 "), "join-rows ep=3 "), "join-more ep=3 "), "join-draws ep=3 seen=905 relevant=337 views=5 signatures=3",
                "join-draws ep=3 seen=905 relevant=0 views=5 signatures=0")
    _, out = report(quiet)
    part = squash(out)[squash(out).find("episode 3/10"):]
    if "none: the hook saw 905 draw(s) and none went into a depth of the 2D screen's size or an eye's size" not in part or "NO draw in the sampled frame" in part:
        fail("an episode whose draws saw no screen- or eye-sized depth was not named with its draw count:\n%s" % part[:1200])
    _, out = report(drop(text, "join-draws ep=3 "))
    if "no `join-draws` line: the hook's draw counts are not in this log" not in squash(out):
        fail("an episode with no join-draws line did not say so:\n%s" % out[out.find("episode 3/10"):][:900])
    # A rows line missing: the join says the rows were read and the line is not here (and does not read as 'no call matched').
    _, out = report(drop(text, "join-rows ep=1 sig=1 "))
    if "the rows were read but their `join-rows` line is not in this log" not in out:
        fail("a join signature whose rows line is missing was not named:\n%s" % out)
    # The calls the join matched are not in the log (the DLL's own list names them): said, never 'no call composed them'.
    no_eyes = "\n".join(l for l in text.split("\n") if not (": call frame=31 " in l and " kind=5 " in l))
    _, out = report(no_eyes)
    if "equal the composed rows of call(s) n=9,10,11, none of which is among the printed calls" not in squash(out) or \
            "equal the composed rows of NO call of this frame" in out.split("episode 2/10")[0]:
        fail("matched calls that were not printed were not named by their ordinals, or read as 'no call':\n%s" % out[out.find("== episodes"):out.find("episode 2/10")])
    # The DLL lists an ordinal that is not printed beside printed ones; and the reader's own match disagreeing with the DLL's is flagged.
    more = sub(text, "rows=read match=9,10,11", "rows=read match=9,10,11,99+3")
    _, out = report(more)
    if "the DLL also lists call(s) n=99 and 3 more, not among the printed lines" not in squash(out):
        fail("an ordinal the DLL listed that is not printed (and a count past its list) was not said:\n%s" % out[out.find("== episodes"):out.find("episode 2/10")])
    nudged = mutate(text, "call frame=31 n=10 ", lambda line: re.sub(r"rows=\[([^,\]]+)", lambda m: "rows=[%.9g" % (float(m.group(1)) + 5e-5), line, count=1))
    _, out = report(nudged)
    if "the DLL's match and this reader's disagree on call(s) n=10" not in squash(out):
        fail("a printed call whose rows the reader does not match, though the DLL listed it, was not flagged:\n%s" % out[out.find("== episodes"):out.find("episode 2/10")])
    # The chooser: rows that equal no call's axes name the nearest and how near; rows equal to an eye camera's axes do not say STRAYS.
    _, out = report(sub(text, "axes-match=5,6,7,8,9 how=transpose nearest=5 diff=0.000e+00", "axes-match=- how=identity nearest=15 diff=2.500e-03"))
    if "equal the view axes of NO recorded call; the nearest is call n=15 at a distance of 2.500e-03" not in squash(out) or "STRAYS" in out:
        fail("pass rows that equal no call's axes were not reported with the nearest call:\n%s" % out[out.find("episode 2/10"):out.find("episode 3/10")])
    # No kind-5 call in the frame: a kind-3 match is not a stray (there is no eye camera to stray from).
    solo = sub(text, "kinds=1:4,3:5,5:6", "kinds=1:4,3:5")
    _, out = report(solo)
    if "STRAYS" in out:
        fail("a frame with no kind-5 call reported a stray chooser:\n%s" % out)
    # The call lines are capped (more calls recorded than printed): said once for the episode, with the order the cap keeps; a header that promises more lines than the log holds is flagged.
    _, out = report(sub(text, "calls=14 recorded=14 printed=14", "calls=20 recorded=18 printed=14"))
    part = squash(out).split("episode 2/10")[0]
    if "the call lines are capped at 120 an episode (14 of 18 recorded calls printed)" not in part or "the counts above are of all 20 calls" not in part or \
            "call line(s) of the" in part or "(2 past the buffer: counted, not recorded)" not in part:
        fail("an episode whose calls were capped was not said so:\n%s" % part[:900])
    _, out = report(sub(text, "calls=14 recorded=14 printed=14", "calls=14 recorded=14 printed=20"))
    if "!! 14 call line(s) of the 20 the header says printed are in this log" not in squash(out):
        fail("a header that says more call lines printed than the log holds was not flagged:\n%s" % out[out.find("== episodes"):out.find("episode 2/10")])
    # Runs: no `runs` lines, no section; a log whose longest unnamed run is under 3 says 0 long runs.
    _, out = report(drop(text, "runs "))
    if "== on-foot naming runs" in out or "== the detour's CPU" not in out:
        fail("a log with no runs lines still printed the naming runs, or dropped the detour section:\n%s" % out)
    short = sub(sub(text, "unnamed=1:2,2:1,3:0,4-8:1,9-30:0,31-89:0,90+:0", "unnamed=1:2,2:1,3:0,4-8:0,9-30:0,31-89:0,90+:0"), "longest=named:211,unnamed:7", "longest=named:211,unnamed:2")
    _, out = report(short)
    if "H2 (the longest unnamed run in an on-foot world stays under 3 frames): 0 unnamed run(s) of 3 frames or more, and 3 of 1 or 2" not in squash(out) or \
            "longest named run 211 frame(s), longest unnamed run 2 frame(s)" not in squash(out):
        fail("runs that stay under three frames were not reported as none of 3 or more:\n%s" % out[out.find("== on-foot naming runs"):])
    # A run of exactly three unnamed frames is a long one (the bin of 3 counts): the release the design waits for.
    three = sub(text, "unnamed=1:2,2:1,3:0,4-8:1,9-30:0,31-89:0,90+:0", "unnamed=1:2,2:1,3:1,4-8:0,9-30:0,31-89:0,90+:0")
    _, out = report(three)
    if "H2 (the longest unnamed run in an on-foot world stays under 3 frames): 1 unnamed run(s) of 3 frames or more, and 3 of 1 or 2" not in squash(out):
        fail("an unnamed run of exactly three frames was not counted as one of three or more:\n%s" % out[out.find("== on-foot naming runs"):])
    # Detour: no lines, no section; no timed call at all says nothing was estimated.
    _, out = report(drop(text, "detour "))
    if "== the detour's CPU" in out or "== on-foot naming runs" not in out:
        fail("a log with no detour lines still printed the CPU section, or dropped the runs:\n%s" % out)
    # The mean estimate is weighted by each window's frames: a window of 45 frames counts a tenth of one of 448.
    _, out = report(sub(text, "windows=1 every=16 timed=observer-halves frames=450 ", "windows=1 every=16 timed=observer-halves frames=45 "))
    if not re.search(r"estimated ms a frame: mean 0\.49\d over 2 window line\(s\) \(lowest 0\.155, highest 0\.533\)", squash(out)):
        fail("the detour's mean estimate was not weighted by each window's frames:\n%s" % out[out.find("== the detour's CPU"):])
    untimed = re.sub(r"(vr camera census: detour [^\n]*?) est-ms-frame=\S+", r"\1 est-ms-frame=-", text)
    _, out = report(untimed)
    if "estimated ms a frame: none (no window had both calls and timed calls)" not in out:
        fail("detour lines with no estimate were not said so:\n%s" % out[out.find("== the detour's CPU"):])
    # The episode lines are in the log of a census that never had a legacy sequence (a cockpit-only session): the report still finishes and joins eye draws as it did.
    cockpit_only = text
    for prefix in ("sequence ", "call frame=4 ", "call frame=5 ", "call frame=6 ", "eye=", "eye-geometry "):
        cockpit_only = drop(cockpit_only, prefix)
    rc, out = report(cockpit_only)
    if rc != 0 or "== episodes (3 printed" not in out or "episode 1/10" not in out or "no eye draw was read back" not in out.lower():
        fail("a session with episodes and no on-foot sequence did not report both sides:\n%s" % out[:1500])

    # ---- the command line ----
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        rc = main(["--file", fixture, "--camera-census"])
    if rc != 0 or "== which signal separates the eye cameras (A)-(F) ==" not in buf.getvalue() or \
            "== stage 2 verdict" not in buf.getvalue() or "== episodes (3 printed" not in buf.getvalue():
        fail("--camera-census through main() returned %d:\n%s" % (rc, buf.getvalue()))
    return ok


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
