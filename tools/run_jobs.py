#!/usr/bin/env python3
"""Run build.bat's independent test rigs concurrently.

build.bat builds the DLLs itself, then hands every `:rig_<label>` subroutine
in its own text to this runner. Each rig is started as a `build.bat --rig
<label>` child -- the child inherits the parent's toolchain environment and
jumps straight to its subroutine -- N at a time, longest first. A rig's output
goes to a log file of its own as the rig writes it (see --exe-dir) and is
printed in one piece under a banner when the rig finishes, so the build log
still reads as if the rigs had run one after another, and a hung rig's partial
output survives it.

  python tools\\run_jobs.py --script build.bat [--jobs N] [--mp N]
                           [--exe-dir build] [--serial a,b,c]... [--quiet d,e]
                           [--after c=p,p]... [--times build\\rig_times.json]
                           [--timeout SECONDS] [--timeout-scale FACTOR]
                           [--dry-run]
  python tools\\run_jobs.py --sweep-exe-dir build [--dry-run]
  python tools\\run_jobs.py --self-test

--exe-dir is the directory the rigs' test exes are built into. Every process
a rig starts from there is told, through EDVR_LOG_DIR and EDVR_LOG_DIR_FOR
(src\\common\\config.cpp), to log under <exe-dir>\\edvr_logs\\<label> instead
of <exe-dir>\\edvr_logs: the proxy DLLs a test loads keep their logs and
crash sentinels there, so two rigs' proxies never read each other's sentinels
and stand down. A child a rig stages in a private directory keeps its own
default, which the rigs that read such a child's log rely on. The rig's own
console output is streamed, as it is written, to a file in that same directory,
runner_output.txt (runner_output_build.txt and runner_output_run.txt for the two
steps of a rig that splits itself), each build overwriting the last.

A rig that stages such a private directory directly under --exe-dir itself
(its own copy of a proxy DLL, its own edvr.ini, its own children), rather
than under its log directory, should name it "<label>-" or "<label>_"
followed by anything of its own choosing: the runner deletes every directory
of a passing rig's own that matches, and keeps -- and names, in that rig's
failure line -- every one that matches a failing rig's, so it can be
diagnosed. No shipped rig does this today; the convention exists because one
used to (tools\\vr_census_bridge_test, tools\\openvr_export_census_test,
tools\\openvr_smoke -- all removed in 1a54e9e with the legacy OpenVR proxy
they tested) and left its output -- build\\census-bridge-*, build\\
export-census-*, build\\vrtest_census_* -- to accumulate forever, since
nothing had ever deleted it. See --sweep-exe-dir for that existing debris,
and for a future rig that forgets the convention or dies mid-run.

--sweep-exe-dir removes stale per-instance directories left under a build
directory by *any* earlier build, independent of --script and any rig
label -- build\\<anything>-<pid>-<tick> or <anything>_<pid>_<tick>, matched
by shape alone (see stale_exe_dirs), never a file, and never one of the
build's own fixed outputs. Run once at the very start of build.bat, before
the DLLs even compile, so a rig deleted since the last build still has its
old output cleaned up. --dry-run lists what it would remove and removes
nothing.

--serial names rigs that must not run at the same time as one another, though
any of them may run beside the rest: rigs whose test processes share state
that --exe-dir cannot separate. Each --serial is one such group; the group is
scheduled as a chain, started early because its members can only follow one
another.

--after declares a one-way dependency: CONSUMER=PRODUCER[,PRODUCER...] holds
CONSUMER back until every PRODUCER has finished successfully -- for a rig
that reads a file another rig's subroutine writes (an export fixture DLL,
say), which is otherwise exactly the thing build.bat's rig block forbids.
Repeat --after for another consumer, or again for the same one to add more
producers. Unlike --serial it is one-way and not about exclusion: PRODUCER
runs alongside whatever else is ready; CONSUMER simply will not start before
PRODUCER succeeds. Neither side may name a --quiet rig: a quiet rig's real
finish happens in a separate pool run only after this one completes with no
failure, so a dependency on or across that boundary could never be
satisfied and would strand the job silently rather than fail it. --after is
checked once, at the plan, not discovered later as a hang: an unknown label,
a --quiet rig on either side, or a cycle (a rig named after itself is one)
is rejected before any rig runs.

--quiet names rigs that must not share the machine with anything: timing tests
that compare wall-clock intervals against tight bounds. They run one at a time
after the parallel group, on an otherwise idle machine, in the order given.

A --serial or --quiet rig usually compiles for far longer than it runs, and
only its runs need holding back. Such a rig may split itself in two steps:

  :rig_x
  if "%EDVR_RIG_STEP%"=="run" goto x_run
  cl.exe ... /Fe"%BUILD%\\x.exe" ...
  if errorlevel 1 ( echo [edvr] ERROR: x build failed & exit /b 1 )
  if "%EDVR_RIG_STEP%"=="build" exit /b 0
  :x_run
  "%BUILD%\\x.exe" --self-test || exit /b 1
  exit /b 0

The runner recognises the exact line `if "%EDVR_RIG_STEP%"=="build" exit /b 0`,
runs the rig once with EDVR_RIG_STEP=build among all the others and once with
EDVR_RIG_STEP=run under its group's rule, and reports the two as "x (build)"
and "x (run)". A rig without that line runs whole under the rule. Run by hand,
with the variable unset, the rig runs whole.

A rig may not link d3d11.lib into an output directly in %BUILD%. build\\d3d11.dll
is EDVR's own proxy, and an exe beside it binds a d3d11.dll import to that proxy
before System32's, so the rig would run under EDVR's hooks by accident. The plan
refuses such a rig, naming its line and its output, before anything compiles
(architecture review 2026-09-29, I-8); a rig that needs a device takes System32's
through src\\common\\system_d3d11.h, or builds its output outside %BUILD%.

--times is where the runner records how long each rig took, and reads it back
next time so the longest rigs start first. A rig with no record is estimated
from how many sources its subroutine compiles.

Every rig runs under a timeout: max(180 s, 3 x the time --times recorded for
it), or 900 s for a rig with no record. A rig still running then has its whole
process tree killed (taskkill /T), is reported as TIMEOUT with the tail of its
output, and fails the build like any other failing rig, rather than hanging the
build silently under the build lock (architecture review 2026-09-29, I-5).
--timeout-scale (environment: EDVR_RIG_TIMEOUT_SCALE) multiplies every timeout,
for a slow or heavily loaded machine; --timeout SECONDS (EDVR_RIG_TIMEOUT)
replaces the rule with that one figure for every rig, and 0 turns timeouts off.

The runner puts itself in a Windows job object that kills its members when the
runner dies (JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE), so a runner that is killed
leaves no compilers or test exes running with files in build\\ open. Where it
cannot (it is already in a job that forbids it) it says so in one line and
carries on.

Nothing a build runs may touch the desktop, and the runner holds it to that.
It watches every process the rigs start with tools\\focus_watch.py and, once the
rigs have finished, fails the build if any of them showed a window, opened a
console, or moved the keyboard focus to a window of its own: whoever is typing
into another window loses the keystrokes. The finding names the exe, the rig
and the chain of parents. (A full build once took the focus a dozen times: the
graphics proxy under test force-foregrounds the window a swap chain is created
on, and every rig that made a hidden window of its own for one was taken for the
game.) The guard prints a line saying how much it saw, so a build log without
one did not run it; where the desktop cannot be watched it says that instead
and the build carries on. The runner also sets its own error mode
(SetErrorMode: no crash, assert or missing-DLL dialog), which every job
inherits, so a rig that faults ends with its exit code instead of a dialog that
takes the focus and waits for a click nobody is there to give.

A failing rig stops new launches. The rigs already running finish and print,
the failed rig's output is printed last so the tail of the build log names the
failure, and the exit code is 1. --dry-run prints the plan and writes nothing.
"""
import argparse
import contextlib
import io
import json
import math
import os
import queue
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

# cmd treats everything after the label's first whitespace as a comment, so
# ":rig_x anything" is still the label rig_x and must still be run.
LABEL = re.compile(r"^:rig_([A-Za-z0-9_]+)(?:\s.*)?$")
SOURCE = re.compile(r'\.cpp"')
BUILD_GUARD = 'if "%EDVR_RIG_STEP%"=="build" exit /b 0'

# What d3d11_in_build looks for in a rig's compiler commands: a cl command that
# links (no /c) d3d11.lib -- after /link, or as a plain input, which cl hands to
# the linker as well -- into a /Fe output that sits directly in %BUILD%.
CL_COMMAND = re.compile(r"^\s*cl(?:\.exe)?\s", re.IGNORECASE)
COMPILE_ONLY = re.compile(r"\s/c(?:\s|$)", re.IGNORECASE)
EXE_OUTPUT = re.compile(r'/Fe:?\s*"?([^"\s]+)"?', re.IGNORECASE)
BUILD_ROOT_OUTPUT = re.compile(r"^%BUILD%\\[^\\/]+$", re.IGNORECASE)
D3D11_LIB = re.compile(r"(?<![\w.-])d3d11\.lib(?![\w.-])", re.IGNORECASE)

# Per-rig timeouts (architecture review 2026-09-29, I-5): a rig that has run
# before may take three times what it took last time, never less than
# TIMEOUT_FLOOR seconds; one with no record may take TIMEOUT_UNRECORDED. Past
# that the runner kills the rig's whole process tree and fails the build like
# any other failing rig, rather than hanging it silently under the build lock.
TIMEOUT_FLOOR = 180.0
TIMEOUT_FACTOR = 3.0
TIMEOUT_UNRECORDED = 900.0
TIMEOUT_EXIT = 124         # the exit code a killed rig is recorded with (timeout(1)'s)
TIMEOUT_TAIL_LINES = 40    # how much of a hung rig's output its report prints
KILL_WAIT = 30             # seconds granted to taskkill, and to the tree to be gone
LOG_FILE = "runner_output" # <exe-dir>\edvr_logs\<label>\runner_output[_<step>].txt

# Job object limit flags and information class (winnt.h) for contain_children.
JOB_KILL_ON_CLOSE = 0x2000      # JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
JOB_BREAKAWAY_OK = 0x0800       # JOB_OBJECT_LIMIT_BREAKAWAY_OK
JOB_EXTENDED_LIMITS = 9         # JobObjectExtendedLimitInformation

# SetErrorMode flags (errhandlingapi.h) that quiet_faults sets for every job.
SEM_FAILCRITICALERRORS = 0x0001   # no "cannot find the DLL" or critical-error box
SEM_NOGPFAULTERRORBOX = 0x0002    # no Windows Error Reporting crash dialog
SEM_NOOPENFILEERRORBOX = 0x8000   # no "cannot open the file" box
QUIET_FAULTS = SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX

# A per-instance exe directory a rig (or a child it stages) made for itself:
# "<anything>-<pid>-<tick>" or "<anything>_<pid>_<tick>", the shape every
# family of debris found under build\ on 2026-09-17 shared regardless of the
# label before it -- census-bridge-ready-6172-97754453,
# export-census-missing-8748-143777687, vrtest_census_8916_151180000. Shape
# alone, not a known label, because --sweep-exe-dir must still find one made
# by a rig deleted since the build that made it.
STALE_EXE_DIR = re.compile(r"^.+[-_]\d+[-_]\d+$")

# Fixed build\ outputs that must never be swept even if a future one's name
# happened to match STALE_EXE_DIR -- belt-and-suspenders alongside the shape
# check and the directories-only, top-level-only rule in stale_exe_dirs.
PROTECTED_EXE_DIR_ENTRIES = {
    "d3d11.dll", "openvr_api.dll", "edvr-installer.exe", "edvr-flat-installer.exe", "gen", "gen-flat", "obj",
    "edvr_logs",
}


class Rig:
    __slots__ = ("label", "line", "sources", "two_step", "group", "after", "d3d11_in_build")

    def __init__(self, label, line, sources=0, two_step=False):
        self.label, self.line, self.sources, self.two_step = label, line, sources, two_step
        self.group = None   # index of its --serial group, if any; set by plan()
        self.after = ()     # labels it must not start before finish; set by plan()
        self.d3d11_in_build = ()   # (line, output) of each link that binds build\d3d11.dll; see d3d11_in_build()

    def __repr__(self):
        return "Rig(%r, %d, %d%s)" % (self.label, self.line, self.sources,
                                      ", two_step" if self.two_step else "")


class Job:
    """One child process: a whole rig, or the build or run step of a rig that
    splits itself. The run step of a serial rig, and a whole serial rig, are
    the jobs the group rule holds back; a build step joins the pool freely."""
    __slots__ = ("rig", "step")

    def __init__(self, rig, step=None):
        self.rig, self.step = rig, step

    @property
    def constrained(self):
        return self.rig.group is not None and self.step != "build"

    @property
    def key(self):
        """The name in the times file: a build step stands for the rig, since
        the rig's whole time stood for it before it was split."""
        return self.rig.label if self.step != "run" else self.rig.label + " (run)"

    @property
    def title(self):
        return self.rig.label if self.step is None else "%s (%s)" % (self.rig.label, self.step)

    def __repr__(self):
        return "Job(%r)" % self.title


def d3d11_in_build(body, first_line):
    """(line, output) for every compiler command in `body` -- a rig's lines,
    the first of them line `first_line` of the script -- that links d3d11.lib
    into an output sitting directly in %BUILD%. build\\d3d11.dll is EDVR's own
    proxy, and a d3d11.dll import of an exe (or a DLL an exe loads) beside it
    resolves to that proxy before System32's: the rig then runs under EDVR's
    hooks by accident (architecture review 2026-09-29, I-8). Such a rig takes
    Windows' own d3d11 through src\\common\\system_d3d11.h and links without
    d3d11.lib, or builds its output elsewhere (%OBJ%\\<name>\\). Commands
    continued with ^ are read as the one command they are."""
    found, joined, begun = [], "", None
    for number, line in enumerate(body, first_line):
        stripped = line.rstrip()
        if begun is None:
            begun = number
        if stripped.endswith("^"):
            joined += stripped[:-1] + " "
            continue
        command, at, joined, begun = joined + stripped, begun, "", None
        if not CL_COMMAND.match(command) or COMPILE_ONLY.search(command):
            continue
        output = EXE_OUTPUT.search(command)
        if output and BUILD_ROOT_OUTPUT.match(output.group(1)) and D3D11_LIB.search(command):
            found.append((at, output.group(1)))
    return tuple(found)


def check_system_d3d11(rigs):
    """Raise ValueError naming every rig whose link binds build\\d3d11.dll (see
    d3d11_in_build), so a rig that would run under EDVR's hooks by accident
    stops the build at the plan, before anything compiles."""
    bound = [(rig, line, output) for rig in rigs for line, output in rig.d3d11_in_build]
    if not bound:
        return
    raise ValueError(
        "%d rig link(s) put d3d11.lib into an output in %%BUILD%%, where the d3d11.dll import binds "
        "build\\d3d11.dll (EDVR's own proxy) instead of System32's and the rig runs under EDVR's hooks "
        "by accident. Create the device through src\\common\\system_d3d11.h and link without d3d11.lib, "
        "or build the output outside %%BUILD%%:\n%s"
        % (len(bound), "\n".join("  :rig_%s, line %d: %s" % (rig.label, line, output)
                                for rig, line, output in bound)))


def parse_rigs(text):
    """Every :rig_<label> in the script, in order, with the number of sources
    its subroutine compiles, whether it honours EDVR_RIG_STEP, and the links
    that would bind build\\d3d11.dll (the text up to the next label)."""
    lines = text.splitlines()
    rigs, seen = [], {}
    for number, line in enumerate(lines, 1):
        match = LABEL.match(line)
        if not match:
            continue
        label = match.group(1)
        if label in seen:
            raise ValueError("duplicate rig label :rig_%s at lines %d and %d"
                             % (label, seen[label], number))
        seen[label] = number
        rigs.append(Rig(label, number))
    if not rigs:
        raise ValueError("the script defines no :rig_<label> subroutines")
    for index, rig in enumerate(rigs):
        end = rigs[index + 1].line - 1 if index + 1 < len(rigs) else len(lines)
        body = lines[rig.line:end]
        rig.sources = len(SOURCE.findall("\n".join(body)))
        rig.two_step = any(line.strip() == BUILD_GUARD for line in body)
        rig.d3d11_in_build = d3d11_in_build(body, rig.line + 1)
    return rigs


def estimate(job, times):
    """Seconds a job is expected to take: the last measurement, else a guess
    (a compile is about a second a file, a run a couple of seconds)."""
    known = times.get(job.key)
    if isinstance(known, (int, float)) and known >= 0:
        return float(known)
    return 2.0 if job.step == "run" else 2.0 + 0.7 * job.rig.sources


def rig_timeout(job, times, scale=1.0, override=None):
    """Seconds `job` may run before the runner kills it: max(TIMEOUT_FLOOR,
    TIMEOUT_FACTOR x its recorded seconds), or TIMEOUT_UNRECORDED when it has
    no usable record, times `scale`. `override` (--timeout) is one figure for
    every job, not scaled; 0 turns timeouts off (None is returned)."""
    if override is not None:
        return float(override) if override > 0 else None
    known = times.get(job.key)
    if (isinstance(known, (int, float)) and not isinstance(known, bool)
            and math.isfinite(known) and known >= 0):
        base = max(TIMEOUT_FLOOR, TIMEOUT_FACTOR * known)
    else:
        base = TIMEOUT_UNRECORDED
    return base * scale


def check_acyclic(after):
    """Raise ValueError naming the loop if `after` (consumer label -> tuple
    of producer labels) has a cycle -- a rig named after itself is one."""
    WHITE, GRAY, BLACK = range(3)
    color = {}

    def visit(label, stack):
        color[label] = GRAY
        stack.append(label)
        for producer in after.get(label, ()):
            state = color.get(producer, WHITE)
            if state == GRAY:
                loop = stack[stack.index(producer):] + [producer]
                raise ValueError("--after has a cycle: %s" % " -> ".join(loop))
            if state == WHITE:
                visit(producer, stack)
        stack.pop()
        color[label] = BLACK

    for label in after:
        if color.get(label, WHITE) == WHITE:
            visit(label, [])


def plan(rigs, quiet, times, serial=(), after=None):
    """The pool, longest expected first, and the quiet jobs in the order
    named. Every quiet, serial or after label must be a rig, and a rig
    belongs to at most one of the serial groups. A serial or quiet rig that
    splits itself puts its build step in the pool and only its run step
    under the rule. A serial group's jobs -- a whole rig, or the build step
    whose run step will join the chain -- are ranked by the whole chain's
    expected length rather than their own: the chain starts first and never
    becomes the tail. `after` is {consumer_label: [producer_label, ...]}
    (see run_group and launchable for how it holds a job back); neither a
    consumer nor a producer may be a --quiet rig, and the whole relation
    must be acyclic -- both raise ValueError here, before anything runs,
    rather than stranding a job that can never become launchable. A rig that
    links d3d11.lib into %BUILD% is refused the same way (check_system_d3d11)."""
    check_system_d3d11(rigs)
    after = {label: tuple(producers) for label, producers in (after or {}).items()}
    by_label = {rig.label: rig for rig in rigs}
    for option, labels in ([("--quiet", quiet)] + [("--serial", group) for group in serial]
                           + [("--after", (consumer,) + producers)
                              for consumer, producers in after.items()]):
        unknown = [label for label in labels if label not in by_label]
        if unknown:
            raise ValueError("%s names rigs the script does not define: %s"
                             % (option, ", ".join(unknown)))
    quiet_set = set(quiet)
    for consumer, producers in after.items():
        named = [label for label in (consumer,) + producers if label in quiet_set]
        if named:
            raise ValueError("--after cannot name a --quiet rig: %s" % ", ".join(named))
    check_acyclic(after)
    group_of = {}
    for index, group in enumerate(serial):
        for label in group:
            if label in group_of:
                raise ValueError("rig %s is named by --serial twice" % label)
            if label in quiet:
                raise ValueError("rig %s is --quiet, which already runs it alone; "
                                 "it cannot also be --serial" % label)
            group_of[label] = index
    for rig in rigs:
        rig.group = group_of.get(rig.label)
        rig.after = after.get(rig.label, ())

    def split(rig):
        return rig.two_step and (rig.group is not None or rig.label in quiet)

    def held(rig):   # the job the group rule or the quiet rule applies to
        return Job(rig, "run" if split(rig) else None)

    pool = [Job(rig, "build" if split(rig) else None)
            for rig in rigs if rig.label not in quiet or split(rig)]
    chain = [sum(estimate(held(by_label[label]), times) for label in group) for group in serial]

    def rank(job):
        own = estimate(job, times)
        return (-chain[job.rig.group] if job.rig.group is not None else -own, -own)

    pool.sort(key=rank)
    return pool, [held(by_label[label]) for label in quiet]


def child_command(script, label):
    """The command line for one rig. cmd strips the outer quotes when the
    first character after /c is a quote, leaving the quoted script path."""
    return 'cmd.exe /d /c ""%s" --rig %s"' % (script, label)


def log_dir(exe_dir, job):
    """Where the processes a rig starts from exe_dir log: a directory of the
    rig's own, so no two rigs' proxies share crash sentinels."""
    return os.path.join(exe_dir, "edvr_logs", job.rig.label)


def child_env(env, job, exe_dir=None):
    """The child's environment: the runner's, plus the step for a split rig
    and, given --exe-dir, the log directory for the rig's processes there."""
    child = dict(env)
    if job.step is not None:
        child["EDVR_RIG_STEP"] = job.step
    if exe_dir is not None:
        child["EDVR_LOG_DIR"] = log_dir(exe_dir, job)
        child["EDVR_LOG_DIR_FOR"] = exe_dir
    return child


def child_process_kwargs():
    """Keep rig command shells from creating or foregrounding a console window."""
    if os.name == "nt":
        return {"creationflags": subprocess.CREATE_NO_WINDOW}
    return {}


def rig_log_path(exe_dir, job):
    """The file a job's console output is streamed to as the rig writes it:
    in the rig's own log directory beside the logs its processes write, one
    file per step of a rig that splits itself. None without --exe-dir."""
    if exe_dir is None:
        return None
    suffix = "" if job.step is None else "_" + job.step
    return os.path.join(log_dir(exe_dir, job), LOG_FILE + suffix + ".txt")


def kill_tree(process):
    """Kill a child and everything it started. taskkill /T walks the parent
    links down from the child, which reaches the compilers and test exes
    under a rig's cmd.exe; process.kill() covers the child itself when
    taskkill is missing or fails. Waits (bounded) for the child to be gone."""
    if os.name == "nt":
        taskkill = os.path.join(os.environ.get("SystemRoot", r"C:\Windows"), "System32", "taskkill.exe")
        try:
            subprocess.run([taskkill, "/T", "/F", "/PID", str(process.pid)],
                           stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL, timeout=KILL_WAIT, **child_process_kwargs())
        except (OSError, subprocess.SubprocessError):
            pass
    try:
        process.kill()
    except OSError:
        pass
    try:
        process.wait(timeout=KILL_WAIT)
    except subprocess.TimeoutExpired:
        pass


def run_rig(title, command, cwd, env, log_path, limit):
    """Run one job's command with its combined output streamed into log_path
    as it is written -- the child holds the file, not a pipe the runner
    reads, so partial output is on disk while it runs and a grandchild that
    outlives its parent cannot keep this call waiting for an end of file. A
    temporary file stands in when log_path is None.

    Past `limit` seconds (None: no limit) the child's whole process tree is
    killed. Returns (exit code, output, limit_hit): limit_hit is `limit` for a
    rig that ran past it, whose exit code is TIMEOUT_EXIT and whose output ends
    with a line saying so and where the whole log is; None otherwise."""
    scratch = log_path is None
    if scratch:
        handle, log_path = tempfile.mkstemp(prefix="edvr-rig-", suffix=".txt")
        os.close(handle)
    limit_hit = None
    try:
        with open(log_path, "wb") as log:
            process = subprocess.Popen(command, cwd=cwd, env=env, stdin=subprocess.DEVNULL,
                                       stdout=log, stderr=subprocess.STDOUT,
                                       **child_process_kwargs())
            try:
                code = process.wait(timeout=limit)
            except subprocess.TimeoutExpired:
                kill_tree(process)
                limit_hit, code = limit, TIMEOUT_EXIT
        with open(log_path, "rb") as log:
            output = log.read()
        if limit_hit is not None:
            output += b"\n" if output and not output.endswith(b"\n") else b""
            output += ("[edvr] TIMEOUT: %s was still running after %d s; the runner killed its "
                       "process tree.%s\n" % (title, limit_hit,
                                              "" if scratch else " Its whole log: %s" % log_path)
                       ).encode("utf-8", "replace")
    finally:
        if scratch:
            try:
                os.unlink(log_path)
            except OSError:
                pass
    return code, output, limit_hit


_JOB = []   # the runner's job object handle, kept open as long as the process lives


def contain_children():
    """Put this process in a Windows job object that kills every process in it
    when the job's last handle closes, which for a handle only this process
    holds is when it ends by any means, a kill included. Children inherit the
    membership, so a runner that dies takes its rigs' cmd.exe, compilers and
    test exes with it instead of leaving them running with files in build\\
    open. A child that asks to break away (a toolchain daemon such as
    mspdbsrv) still may, and is then outside the job as it always was.
    Returns the handle; raises OSError when it cannot be done: not Windows,
    or already in a job that forbids nesting."""
    if os.name != "nt":
        raise OSError("job objects are a Windows feature")
    import ctypes
    from ctypes import wintypes

    class BasicLimits(ctypes.Structure):
        _fields_ = [("PerProcessUserTimeLimit", ctypes.c_longlong),
                    ("PerJobUserTimeLimit", ctypes.c_longlong),
                    ("LimitFlags", wintypes.DWORD),
                    ("MinimumWorkingSetSize", ctypes.c_size_t),
                    ("MaximumWorkingSetSize", ctypes.c_size_t),
                    ("ActiveProcessLimit", wintypes.DWORD),
                    ("Affinity", ctypes.c_size_t),
                    ("PriorityClass", wintypes.DWORD),
                    ("SchedulingClass", wintypes.DWORD)]

    class IoCounters(ctypes.Structure):
        _fields_ = [(name, ctypes.c_ulonglong) for name in (
            "ReadOperationCount", "WriteOperationCount", "OtherOperationCount",
            "ReadTransferCount", "WriteTransferCount", "OtherTransferCount")]

    class ExtendedLimits(ctypes.Structure):
        _fields_ = [("BasicLimitInformation", BasicLimits),
                    ("IoInfo", IoCounters),
                    ("ProcessMemoryLimit", ctypes.c_size_t),
                    ("JobMemoryLimit", ctypes.c_size_t),
                    ("PeakProcessMemoryUsed", ctypes.c_size_t),
                    ("PeakJobMemoryUsed", ctypes.c_size_t)]

    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.CreateJobObjectW.argtypes = [ctypes.c_void_p, wintypes.LPCWSTR]
    kernel.CreateJobObjectW.restype = wintypes.HANDLE
    kernel.SetInformationJobObject.argtypes = [wintypes.HANDLE, ctypes.c_int, ctypes.c_void_p, wintypes.DWORD]
    kernel.SetInformationJobObject.restype = wintypes.BOOL
    kernel.AssignProcessToJobObject.argtypes = [wintypes.HANDLE, wintypes.HANDLE]
    kernel.AssignProcessToJobObject.restype = wintypes.BOOL
    kernel.GetCurrentProcess.restype = wintypes.HANDLE
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    kernel.CloseHandle.restype = wintypes.BOOL
    job = kernel.CreateJobObjectW(None, None)
    if not job:
        raise ctypes.WinError(ctypes.get_last_error())
    limits = ExtendedLimits()
    limits.BasicLimitInformation.LimitFlags = JOB_KILL_ON_CLOSE | JOB_BREAKAWAY_OK
    if not kernel.SetInformationJobObject(job, JOB_EXTENDED_LIMITS, ctypes.byref(limits),
                                          ctypes.sizeof(limits)):
        error = ctypes.get_last_error()
        kernel.CloseHandle(job)
        raise ctypes.WinError(error)
    if not kernel.AssignProcessToJobObject(job, kernel.GetCurrentProcess()):
        error = ctypes.get_last_error()
        kernel.CloseHandle(job)
        raise ctypes.WinError(error)
    _JOB.append(job)
    return job


def get_error_mode():
    """This process's error mode (kernel32 GetErrorMode); 0 off Windows."""
    if os.name != "nt":
        return 0
    import ctypes
    kernel = ctypes.WinDLL("kernel32")
    kernel.GetErrorMode.restype = ctypes.c_uint
    return kernel.GetErrorMode()


def set_error_mode(mode):
    """kernel32 SetErrorMode(mode): the mode it replaced; 0 off Windows."""
    if os.name != "nt":
        return 0
    import ctypes
    kernel = ctypes.WinDLL("kernel32")
    kernel.SetErrorMode.argtypes = [ctypes.c_uint]
    kernel.SetErrorMode.restype = ctypes.c_uint
    return kernel.SetErrorMode(mode)


def quiet_faults():
    """Tell Windows to open no dialog when a process of this build crashes,
    asserts or cannot load a DLL: the process ends with its exit code and the
    rig fails in its log. A dialog takes the focus from whoever is typing and
    holds the build until a click nobody is there to give (or the rig's
    timeout, which kills it). The mode is inherited by every process this one
    starts, the rigs' compilers and test exes among them, unless a child asks
    for the default one, which none of ours does. Returns the mode it replaced,
    for a caller that means to put it back."""
    previous = get_error_mode()
    set_error_mode(previous | QUIET_FAULTS)
    return previous


def focus_guard():
    """A running focus_watch.FocusWatch over every process this one starts."""
    import focus_watch
    return focus_watch.FocusWatch(os.getpid()).start()


def start_focus_guard(focus, out):
    """The watch `focus()` starts; None for no guard, and None with a line
    saying so where the desktop cannot be watched (not Windows, no window
    station). A focus_watch.py that will not even import is not that: it
    raises, and the build fails."""
    if focus is None:
        return None
    try:
        return focus()
    except ImportError:
        raise
    except Exception as error:
        emit(out, "[edvr] NOTE: focus guard: this build is NOT being watched (%s: %s)\n"
             % (type(error).__name__, error))
        return None


def finish_focus_guard(watch, out):
    """Stop the watch and print what it saw, one [edvr] line at a time. Its
    findings; [] when there was no watch."""
    if watch is None:
        return []
    findings, text = watch.finish()
    for line in text.splitlines():
        emit(out, "[edvr] %s\n" % line)
    return findings


def focus_error(findings):
    """The line that ends a build the guard failed: who, and what to read."""
    exes = sorted({finding.culprit[1] for finding in findings})
    jobs = sorted({finding.job or "(main flow)" for finding in findings})
    return ("[edvr] ERROR: focus guard: %d finding(s) by %s, in %s: a job of this build put a window on the "
            "desktop or took the focus from whoever is typing. The list above says which and how; "
            "tools\\focus_watch.py finds them again.\n" % (len(findings), ", ".join(exes), ", ".join(jobs)))


def spawner(script, root, env, exe_dir=None, timeout_for=None):
    """spawn(job) for run_group: runs the job's `build.bat --rig` child and
    returns (exit code, output, limit_hit) as run_rig does, under the
    timeout timeout_for(job) names (a function of the job; None: none)."""
    def spawn(job):
        if exe_dir is not None:
            # Log::open and the sentinel create one level; this is two.
            os.makedirs(log_dir(exe_dir, job), exist_ok=True)
        limit = timeout_for(job) if timeout_for is not None else None
        return run_rig(job.title, child_command(script, job.rig.label), str(root),
                       child_env(env, job, exe_dir), rig_log_path(exe_dir, job), limit)
    return spawn


def owns_exe_dir_entry(name, label, all_labels):
    """True when name (a top-level entry directly under exe_dir) is label's
    own private directory, by the "<label>-" / "<label>_" convention
    (see job_exe_dirs), and no other rig's label is an equally valid but
    longer match -- so a rig whose label happens to prefix another rig's
    never claims that rig's directory."""
    if not (name.startswith(label) and len(name) > len(label) and name[len(label)] in "-_"):
        return False
    return not any(other != label and len(other) > len(label) and name.startswith(other)
                   and len(name) > len(other) and name[len(other)] in "-_"
                   for other in all_labels)


def job_exe_dirs(exe_dir, job, all_labels):
    """Directories directly under exe_dir that job's rig staged for itself
    there (its own copy of a proxy DLL, its own edvr.ini, its own children),
    named by the "<label>-"/"<label>_" convention described in this module's
    docstring. Never a file, and never a directory another rig's longer
    label also matches."""
    label = job.rig.label
    try:
        names = os.listdir(exe_dir)
    except OSError:
        return []
    return sorted(os.path.join(exe_dir, name) for name in names
                  if owns_exe_dir_entry(name, label, all_labels)
                  and os.path.isdir(os.path.join(exe_dir, name)))


def cleanup_job_exe_dirs(exe_dir, job, all_labels, passed):
    """job_exe_dirs(exe_dir, job, all_labels): removed once the rig passes,
    left in place -- and returned, to name in that rig's failure line --
    once it fails, so the directory can be diagnosed."""
    dirs = job_exe_dirs(exe_dir, job, all_labels)
    if not passed:
        return tuple(dirs)
    for path in dirs:
        shutil.rmtree(path, ignore_errors=True)
    return ()


def stale_exe_dirs(exe_dir):
    """Per-instance exe directories left under exe_dir by any earlier build:
    directories only, directly under exe_dir, matching STALE_EXE_DIR and not
    in PROTECTED_EXE_DIR_ENTRIES. Matched by shape alone, not by any rig
    label the current script defines, so debris from a rig removed since the
    build that made it is still found. Returns an empty list, not an error,
    when exe_dir does not exist."""
    try:
        names = os.listdir(exe_dir)
    except OSError:
        return []
    stale = []
    for name in names:
        if name in PROTECTED_EXE_DIR_ENTRIES or not STALE_EXE_DIR.match(name):
            continue
        path = os.path.join(exe_dir, name)
        if os.path.isdir(path) and not os.path.islink(path):
            stale.append(path)
    return sorted(stale)


def sweep_exe_dir(exe_dir, dry_run, out):
    """Remove stale_exe_dirs(exe_dir); --dry-run lists them and removes
    nothing. Returns the number removed (or, under --dry-run, the number
    that would be)."""
    stale = stale_exe_dirs(exe_dir)
    if not stale:
        emit(out, "[edvr] no stale exe directories under %s\n" % exe_dir)
        return 0
    if dry_run:
        emit(out, "[edvr] dry run: would remove %d stale exe director%s under %s:\n"
             % (len(stale), "y" if len(stale) == 1 else "ies", exe_dir))
        for path in stale:
            emit(out, "    %s\n" % path)
        return len(stale)
    removed = 0
    for path in stale:
        try:
            shutil.rmtree(path)
            removed += 1
        except OSError as error:
            emit(out, "[edvr] NOTE: could not remove %s: %s\n" % (path, error))
    emit(out, "[edvr] removed %d stale exe director%s under %s\n"
         % (removed, "y" if removed == 1 else "ies", exe_dir))
    return removed


def emit(out, text):
    out.write(text.encode("utf-8", "replace"))


def tail_lines(output, count):
    """(the last `count` lines of output, how many earlier lines were left out)."""
    lines = output.splitlines(keepends=True)
    if len(lines) <= count:
        return output, 0
    return b"".join(lines[-count:]), len(lines) - count


def report(out, job, code, seconds, output, kept=(), timeout=None):
    """One job's banner and output. A job that ran past its timeout (its
    `timeout` is the limit it hit) is banner-ed TIMEOUT and prints only the
    tail of its output: the rest is in its log file, which the last line of
    that output names."""
    if timeout is not None:
        extra = ", TIMEOUT (limit %d s)" % timeout
    else:
        extra = "" if code == 0 else ", exit code %d" % code
    if kept:
        extra += ", kept " + ", ".join(kept)
    emit(out, "[edvr] --- %s: %.1f s%s ---\n" % (job.title, seconds, extra))
    if timeout is not None:
        output, left_out = tail_lines(output, TIMEOUT_TAIL_LINES)
        if left_out:
            emit(out, "[edvr] (%d earlier lines of its output not shown)\n" % left_out)
    out.write(output)
    if output and not output.endswith(b"\n"):
        out.write(b"\n")
    out.flush()


def launchable(pending, busy, done):
    """The first pending job that no group rule holds back and whose --after
    producers have all finished (given in `done`, a set of rig labels);
    None when every pending job is waiting on its group or a producer."""
    for job in pending:
        if (not job.constrained or job.rig.group not in busy) and done.issuperset(job.rig.after):
            return job
    return None


def run_group(order, jobs, spawn, out, clock=time.monotonic, exe_dir=None, labels=(), done=None):
    """Start spawn(job) for the jobs in order, at most jobs at a time, never
    two held-back jobs of one serial group, and never a job before every rig
    named in its --after has finished. A serial rig's run step is queued, at
    the front, the moment its build step succeeds. Each result is printed as
    it completes, except failures, which are held and printed after the
    group drains. Given exe_dir, each job's own job_exe_dirs are cleaned up
    (cleanup_job_exe_dirs) the moment it finishes, pass or fail. `done` is
    the set of rig labels that have fully finished (an --after producer
    counts once its last step succeeds, so a split rig's build step does not
    count); pass one to seed it with labels finished before this call, or
    omit it for a fresh set -- a fresh set is correct whenever no pending
    job's --after can point outside this call's own order, which plan()
    guarantees by refusing a --quiet rig on either side. Returns (results,
    failures) where each entry is (job, code, seconds, output, kept, timeout)
    in completion order; kept is empty except for a failed job with a
    directory of its own, and timeout is the limit a job was killed for
    running past, else None. spawn returns (code, output), or (code, output,
    timeout) as run_rig does; one that raises fails its job with the error as
    its output, since a worker that died without reporting would leave the
    loop below waiting for it forever."""
    if done is None:
        done = set()
    outcomes = queue.Queue()

    def worker(job):
        started = clock()
        try:
            code, output, *rest = spawn(job)
        except Exception as error:
            code, rest = 1, []
            output = ("[edvr] ERROR: could not run %s: %s: %s\n"
                      % (job.title, type(error).__name__, error)).encode("utf-8", "replace")
        timeout = rest[0] if rest else None
        kept = cleanup_job_exe_dirs(exe_dir, job, labels, code == 0) if exe_dir is not None else ()
        outcomes.put((job, code, clock() - started, output, kept, timeout))

    pending, running, busy, results, failures = list(order), 0, set(), [], []
    while pending or running:
        while pending and running < jobs and not failures:
            job = launchable(pending, busy, done)
            if job is None:
                break
            pending.remove(job)
            if job.constrained:
                busy.add(job.rig.group)
            threading.Thread(target=worker, args=(job,), daemon=True).start()
            running += 1
        if not running:
            break
        job, code, seconds, output, kept, timeout = outcomes.get()
        running -= 1
        if job.constrained:
            busy.discard(job.rig.group)
        results.append((job, code, seconds, output, kept, timeout))
        if code == 0:
            report(out, job, code, seconds, output)
            if job.step == "build" and job.rig.group is not None:
                pending.insert(0, Job(job.rig, "run"))
            else:
                done.add(job.rig.label)
            continue
        failures.append((job, code, seconds, output, kept, timeout))
        if len(failures) == 1:
            note = "" if not kept else " (kept %s)" % ", ".join(kept)
            what = ("TIMED OUT after %d s" % timeout if timeout is not None
                    else "failed (exit code %d)" % code)
            emit(out, "[edvr] %s %s%s; no more rigs start, %d still running\n"
                 % (job.title, what, note, running))
            out.flush()
    for entry in failures:
        report(out, *entry)
    return results, failures


def load_times(path):
    if path is None or not path.is_file():
        return {}
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
        return data if isinstance(data, dict) else {}
    except (OSError, ValueError):
        return {}


def save_times(path, times, results):
    merged = dict(times)
    merged.update({job.key: round(seconds, 2) for job, code, seconds, *_ in results if code == 0})
    path.write_text(json.dumps(merged, indent=1, sort_keys=True) + "\n", encoding="utf-8")


def wrap(labels, indent="       "):
    lines, current = [], indent
    for label in labels:
        if len(current) + len(label) + 1 > 96 and current.strip():
            lines.append(current)
            current = indent
        current += " " + label
    if current.strip():
        lines.append(current)
    return "\n".join(lines)


def describe(pool, quiet, times, serial=(), after=None):
    split = {job.rig.label for job in pool if job.step == "build"}

    def name(label):
        return label + ("(run)" if label in split else "")

    text = "[edvr] parallel, longest expected first:\n"
    text += wrap("%s~%.0fs" % (job.title.replace(" ", ""), estimate(job, times))
                 for job in pool) + "\n"
    for group in serial:
        text += "[edvr] one at a time among themselves: " + " ".join(name(label) for label in group) + "\n"
    for consumer, producers in (after or {}).items():
        text += "[edvr] %s waits for: %s\n" % (name(consumer), " ".join(name(label) for label in producers))
    if quiet:
        text += "[edvr] quiet, one at a time afterwards: " + " ".join(name(job.rig.label) for job in quiet) + "\n"
    return text


def describe_timeouts(scale=1.0, override=None):
    if override is not None:
        return "rig timeouts are off" if override <= 0 else \
            "each rig is killed if it runs past %g s" % override
    return ("each rig is killed if it runs past max(%g s, %gx its last time), %g s with no "
            "record%s" % (TIMEOUT_FLOOR, TIMEOUT_FACTOR, TIMEOUT_UNRECORDED,
                          "" if scale == 1.0 else ", all times %g" % scale))


def run(script, jobs, mp, quiet, times_path, dry_run, out, spawn=None, clock=time.monotonic,
        serial=(), exe_dir=None, after=None, timeout_scale=1.0, timeout_override=None, focus=None):
    """Run the rigs; the exit code. `focus`, when given, starts the watch that
    holds every job to leaving the desktop alone (focus_guard is the real one):
    a finding fails the run even when every rig passed."""
    text = script.read_text(encoding="utf-8", errors="replace")
    rigs = parse_rigs(text)
    labels = tuple(rig.label for rig in rigs)
    times = load_times(times_path)
    pool, later = plan(rigs, quiet, times, serial, after)
    mp_flag = "/MP%d" % mp if mp else "/MP"
    emit(out, "[edvr] %d rigs from %s: %d jobs in the pool, %d at a time (CL=%s), %d quiet\n"
         % (len(rigs), script.name, len(pool), jobs, mp_flag, len(later)))
    emit(out, describe(pool, later, times, serial, after))
    emit(out, "[edvr] %s\n" % describe_timeouts(timeout_scale, timeout_override))
    if exe_dir is not None:
        emit(out, "[edvr] each rig's processes log under %s\n" % os.path.join(exe_dir, "edvr_logs", "<rig>"))
        emit(out, "[edvr] each rig's console output streams to %s\n"
             % os.path.join(exe_dir, "edvr_logs", "<rig>", LOG_FILE + ".txt"))
    out.flush()
    if dry_run:
        emit(out, "[edvr] dry run: wrote nothing.\n")
        out.flush()
        return 0
    quiet_spawn = spawn
    if spawn is None:
        env = dict(os.environ)
        env["CL"] = mp_flag

        def timeout_for(job):
            return rig_timeout(job, times, timeout_scale, timeout_override)

        spawn = spawner(script, script.parent, env, exe_dir, timeout_for)
        # A quiet rig has the machine to itself, so its compiles may use every core.
        quiet_spawn = spawner(script, script.parent, dict(env, CL="/MP"), exe_dir, timeout_for)
    watch = start_focus_guard(focus, out)
    try:
        started = clock()
        results, failures = run_group(pool, jobs, spawn, out, clock, exe_dir=exe_dir, labels=labels)
        pool_seconds = clock() - started
        summed = sum(seconds for _, _, seconds, *_ in results)
        quiet_results, quiet_failures, quiet_seconds = [], [], 0.0
        if not failures:
            started = clock()
            quiet_results, quiet_failures = run_group(later, 1, quiet_spawn, out, clock,
                                                       exe_dir=exe_dir, labels=labels)
            quiet_seconds = clock() - started
    finally:
        # Also when a group raised: the watch's threads must not outlive the run.
        findings = finish_focus_guard(watch, out)
    results += quiet_results
    failures += quiet_failures
    if times_path is not None:
        try:
            save_times(times_path, times, results)
        except OSError as error:
            emit(out, "[edvr] NOTE: could not record rig times in %s: %s\n" % (times_path, error))
    if failures:
        job, code, timeout = failures[0][0], failures[0][1], failures[0][5]
        what = ("TIMED OUT after %d s" % timeout if timeout is not None
                else "failed with exit code %d" % code)
        emit(out, "[edvr] ERROR: %s %s (%d of %d jobs ran)\n"
             % (job.title, what, len(results), len(pool) + len(later)))
        out.flush()
        return 1
    emit(out, "[edvr] the pool of %d jobs took %.1f s (%.1f s summed, %d at a time); "
              "%d quiet took %.1f s\n" % (len(pool), pool_seconds, summed, jobs, len(later), quiet_seconds))
    longest = sorted(results, key=lambda entry: -entry[2])[:5]
    emit(out, "[edvr] longest: " + ", ".join("%s %.1f s" % (job.title, seconds)
                                             for job, _, seconds, *_ in longest) + "\n")
    if findings:
        emit(out, focus_error(findings))
        out.flush()
        return 1
    out.flush()
    return 0


SAMPLE = """@echo off
setlocal enabledelayedexpansion
echo main flow
exit /b 0

:run_rig
call :rig_%EDVR_RIG%
exit /b 0

:rig_alpha
cl.exe /Fe"x.exe" "tools\\alpha\\alpha.cpp" "src\\common\\log.cpp"
"x.exe" || exit /b 1
exit /b 0

:rig_beta
if "%EDVR_RIG_STEP%"=="run" goto beta_run
cl.exe /Fe"y.exe" "tools\\beta\\beta.cpp"
if "%EDVR_RIG_STEP%"=="build" exit /b 0
:beta_run
"y.exe" || exit /b 1
exit /b 0

:rig_gamma trailing words are a comment to cmd
for %%T in (one two) do (
    cl.exe /Fe"%%T.exe" "tools\\%%T\\%%T.cpp" "a.cpp" "b.cpp" "c.cpp"
)
exit /b 0

:rig_timing
"timing.exe" || exit /b 1
exit /b 0
"""


def self_test():
    failures = []

    def check(condition, why):
        if not condition:
            failures.append(why)

    def titles(jobs):
        return [job.title for job in jobs]

    rigs = parse_rigs(SAMPLE)
    alpha, beta, gamma, timing = rigs
    check([rig.label for rig in rigs] == ["alpha", "beta", "gamma", "timing"],
          "labels in script order, a commented label included: %r" % rigs)
    check([rig.sources for rig in rigs] == [2, 1, 4, 0], "sources counted per subroutine: %r" % rigs)
    check([rig.two_step for rig in rigs] == [False, True, False, False],
          "the rig with the build guard is the one that splits: %r" % rigs)
    check(alpha.line == 10, "label line numbers are 1-based: %r" % rigs)
    for bad, why in ((SAMPLE + "\n:rig_beta\nexit /b 0\n", "duplicate labels are an error"),
                     ("@echo off\nexit /b 0\n", "a script without rigs is an error")):
        try:
            parse_rigs(bad)
            check(False, why)
        except ValueError:
            pass

    # A rig that links d3d11.lib into an output directly in %BUILD% binds
    # build\d3d11.dll, EDVR's own proxy, and is refused at the plan (I-8).
    def script_of(*body):
        return "\n".join(["@echo off", "exit /b 0", ":run_rig", "call :rig_%EDVR_RIG%", "exit /b 0", ""]
                         + list(body)) + "\n"

    def refusal(*body):
        """The text of the ValueError plan() raises for a script of these rig
        lines, or None when the plan accepts it."""
        try:
            plan(parse_rigs(script_of(*body)), [], {})
        except ValueError as error:
            return str(error)
        return None

    check(all(rig.d3d11_in_build == () for rig in rigs) and refusal(":rig_ok", "exit /b 0") is None,
          "a script with no such link plans as before")
    one = 'cl.exe /nologo /Fe"%BUILD%\\one.exe" "tools\\one\\one.cpp" /link /INCREMENTAL:NO d3d11.lib dxgi.lib'
    text = refusal(":rig_one", one, "exit /b 0")
    line = script_of(":rig_one", one, "exit /b 0").splitlines().index(one) + 1
    check(text is not None and ":rig_one, line %d: %%BUILD%%\\one.exe" % line in text
          and "system_d3d11.h" in text and "System32" in text,
          "a d3d11.lib link into %%BUILD%% is refused, naming the rig, its line and its output: %r" % text)
    check(parse_rigs(script_of(":rig_one", one))[0].d3d11_in_build == ((line, "%BUILD%\\one.exe"),),
          "parse_rigs records the line and the output")
    continued = ["cl.exe /nologo /O2 ^", '    /Fo"%OBJ%\\two\\" /Fe"%BUILD%\\two.exe" "tools\\two\\two.cpp" ^',
                 "    /link /INCREMENTAL:NO kernel32.lib user32.lib d3d11.lib dxgi.lib", "exit /b 0"]
    text = refusal(":rig_two", *continued)
    line = script_of(":rig_two", *continued).splitlines().index(continued[0]) + 1
    check(text is not None and ":rig_two, line %d:" % line in text,
          "a command continued with ^ is one command, reported at its first line: %r" % text)
    text = refusal(":rig_three", "for %%T in (a b) do (", '    cl.exe /Fe"%BUILD%\\three_%%T.exe" "t.cpp" /link d3d11.lib', ")",
                   "exit /b 0")
    check(text is not None and ":rig_three" in text and "%BUILD%\\three_%%T.exe" in text,
          "a compile inside a for loop is read too: %r" % text)
    check(refusal(":rig_four", 'CL.EXE /FE"%build%\\four.exe" "t.cpp" /LINK D3D11.LIB', "exit /b 0") is not None
          and refusal(":rig_four", 'cl.exe /Fe:"%BUILD%\\four.exe" "t.cpp" /link "C:\\sdk\\um\\x64\\d3d11.lib"',
                      "exit /b 0") is not None,
          "case does not matter, and neither does a path before the library's name")
    check(refusal(":rig_five", 'cl.exe /Fe"%BUILD%\\five.exe" "t.cpp" d3d11.lib /link kernel32.lib', "exit /b 0")
          is not None
          and refusal(":rig_five", 'cl.exe /Fe"%BUILD%\\five.exe" "t.cpp" d3d11.lib', "exit /b 0") is not None,
          "a plain d3d11.lib input, which cl hands to the linker too, is read as well")
    text = refusal(":rig_a", one, ":rig_b", one.replace("one.exe", "b.exe"), "exit /b 0")
    check(text is not None and ":rig_a," in text and ":rig_b," in text and text.startswith("2 rig link(s)"),
          "every offending rig is named: %r" % text)
    for why, body in (
            ("an output in a subdirectory of %BUILD% or in %OBJ%",
             ['cl.exe /Fe"%OBJ%\\x\\x.exe" "t.cpp" /link d3d11.lib', 'cl.exe /Fe"%BUILD%\\sub\\x.exe" "t.cpp" /link d3d11.lib']),
            ("no d3d11.lib on the link line", ['cl.exe /Fe"%BUILD%\\x.exe" "t.cpp" /link dxgi.lib d3dcompiler.lib']),
            ("another library whose name ends the same",
             ['cl.exe /Fe"%BUILD%\\x.exe" "t.cpp" /link xd3d11.lib my-d3d11.lib.txt']),
            ("d3d11.lib named only in a comment, a compile-only (/c) command or a file name",
             ["REM this rig used to link d3d11.lib", 'cl.exe /c /Fe"%BUILD%\\x.exe" "t.cpp" d3d11.lib',
              'cl.exe /Fe"%BUILD%\\y.exe" "tools\\d3d11.lib.cpp" /DD3D11_LIB=1 /link kernel32.lib'])):
        check(refusal(":rig_ok", *body, "exit /b 0") is None, "not refused: %s" % why)
    with tempfile.TemporaryDirectory() as scratch:
        script = Path(scratch) / "build.bat"
        script.write_text(script_of(":rig_one", one, "exit /b 0"), encoding="utf-8")
        out = io.TextIOWrapper(io.BytesIO(), encoding="utf-8", write_through=True)   # main writes to .buffer too
        with contextlib.redirect_stdout(out):
            code = main(["--script", str(script), "--dry-run"])
        printed = out.buffer.getvalue().decode("utf-8")
        check(code == 1 and printed.startswith("[edvr] ERROR: 1 rig link(s) put d3d11.lib") and ":rig_one" in printed,
              "the runner refuses the script even for a dry run, exit 1: %r %r" % (code, printed))

    pool, later = plan(rigs, ["timing"], {})
    check(titles(pool) == ["gamma", "alpha", "beta"], "unknown rigs order by source count: %r" % pool)
    check(titles(later) == ["timing"], "quiet rigs keep their own order")
    pool, _ = plan(rigs, [], {"beta": 30.0, "alpha": 1.5, "gamma": "junk"})
    check(titles(pool) == ["beta", "gamma", "timing", "alpha"],
          "recorded seconds outrank the guess, junk falls back: %r" % pool)
    try:
        plan(rigs, ["timing", "nope"], {})
        check(False, "an unknown quiet label is an error")
    except ValueError as error:
        check("nope" in str(error), "the unknown quiet label is named")

    # A serial group of whole rigs is ranked as a chain: alpha+gamma = 2.0 s
    # outranks timing's 1.5 s though either member alone would not.
    seconds = {"alpha": 1.0, "beta": 1.0, "gamma": 1.0, "timing": 1.5}
    pool, _ = plan(rigs, [], seconds, serial=[["alpha", "gamma"]])
    check(titles(pool) == ["alpha", "gamma", "timing", "beta"],
          "a serial group is ranked by its whole chain: %r" % pool)
    check(alpha.group == 0 and gamma.group == 0 and timing.group is None,
          "plan marks each rig with its serial group")
    # A serial rig that splits itself: only its run step (guessed at 2 s)
    # counts towards the chain, but its build step is ranked with the chain
    # too, since the chain cannot get to that run step before it.
    pool, later = plan(rigs, [], seconds, serial=[["alpha", "beta"]])
    check(titles(pool) == ["alpha", "beta (build)", "timing", "gamma"] and not later,
          "a split serial rig contributes its build step to the pool: %r" % pool)
    check(pool[0].constrained and not pool[1].constrained,
          "a whole serial rig is held back, a build step is not")
    pool, later = plan(rigs, ["beta"], seconds)
    check(titles(pool) == ["timing", "alpha", "beta (build)", "gamma"] and titles(later) == ["beta (run)"],
          "a split quiet rig compiles in the pool and runs alone afterwards: %r %r" % (pool, later))
    check(not beta.group and not later[0].constrained, "a quiet rig belongs to no group")
    pool, _ = plan(rigs, [], seconds, serial=[["beta"]])
    check(estimate(Job(beta, "run"), {"beta (run)": 9.0}) == 9.0
          and estimate(Job(beta, "run"), {"beta": 9.0}) == 2.0
          and estimate(Job(beta, "build"), {"beta": 9.0}) == 9.0,
          "a run step has its own record; a build step takes the rig's")
    for quiet, serial, named, why in (
            (["timing"], [["timing", "alpha"]], "timing", "a quiet rig cannot also be serial"),
            ([], [["alpha"], ["beta", "alpha"]], "alpha", "a rig in two serial groups is an error"),
            ([], [["nope"]], "nope", "an unknown serial label is an error")):
        try:
            plan(rigs, quiet, {}, serial=serial)
            check(False, why)
        except ValueError as error:
            check(named in str(error), "the offending serial label is named: %s" % error)

    # --after: a one-way dependency, unlike --serial's mutual exclusion --
    # only the consumer is held back, and only by name, not by rank.
    pool, _ = plan(rigs, [], seconds, after={"gamma": ["alpha", "beta"]})
    check(gamma.after == ("alpha", "beta") and alpha.after == () == beta.after and gamma.group is None,
          "plan records --after on the consumer only, joining no serial group: %r" % (gamma.after,))
    check({job.rig.label for job in pool} == {"alpha", "beta", "gamma", "timing"},
          "a consumer still joins the pool like any other rig: %r" % pool)
    for after, named, why in (
            ({"nope": ["alpha"]}, "nope", "an unknown consumer is an error"),
            ({"alpha": ["nope"]}, "nope", "an unknown producer is an error"),
            ({"alpha": ["alpha"]}, "alpha", "a rig named --after itself is a cycle"),
            ({"alpha": ["beta"], "beta": ["alpha"]}, "alpha", "a two-rig cycle is an error")):
        try:
            plan(rigs, [], {}, after=after)
            check(False, why)
        except ValueError as error:
            check(named in str(error), "the offending label is named: %s" % error)
    try:
        plan(rigs, ["alpha"], {}, after={"gamma": ["alpha"]})
        check(False, "a --quiet rig cannot be an --after producer")
    except ValueError as error:
        check("alpha" in str(error), "the quiet producer is named: %s" % error)
    try:
        plan(rigs, ["gamma"], {}, after={"gamma": ["alpha"]})
        check(False, "a --quiet rig cannot be an --after consumer")
    except ValueError as error:
        check("gamma" in str(error), "the quiet consumer is named: %s" % error)

    text = describe(pool, [], seconds, after={"gamma": ["alpha", "beta"]})
    check("gamma waits for: alpha beta" in text, "describe names a consumer's producers: %r" % text)

    check(child_command(Path(r"C:\x y\build.bat"), "alpha") == r'cmd.exe /d /c ""C:\x y\build.bat" --rig alpha"',
          "child command quotes the script for cmd /c")
    expected_process_kwargs = ({"creationflags": subprocess.CREATE_NO_WINDOW}
                                if os.name == "nt" else {})
    check(child_process_kwargs() == expected_process_kwargs,
          "rig command shells use CREATE_NO_WINDOW on Windows: %r" % child_process_kwargs())
    env = {"CL": "/MP4"}
    check(child_env(env, Job(alpha)) == env and "EDVR_RIG_STEP" not in child_env(env, Job(alpha)),
          "a whole rig's child gets the runner's environment as it is")
    check(child_env(env, Job(beta, "run")) == {"CL": "/MP4", "EDVR_RIG_STEP": "run"}
          and child_env(env, Job(beta, "build"))["EDVR_RIG_STEP"] == "build",
          "a step's child is told which step it is")
    exe_dir = os.path.join("C:" + os.sep, "repo", "build")
    with_logs = child_env(env, Job(beta, "run"), exe_dir)
    check(with_logs["EDVR_LOG_DIR"] == os.path.join(exe_dir, "edvr_logs", "beta")
          and with_logs["EDVR_LOG_DIR_FOR"] == exe_dir and with_logs["EDVR_RIG_STEP"] == "run"
          and "EDVR_LOG_DIR" not in child_env(env, Job(beta, "run")),
          "given --exe-dir, a child's processes there get the rig's own log directory: %r" % with_logs)
    check(child_env(env, Job(alpha), exe_dir)["EDVR_LOG_DIR"] == os.path.join(exe_dir, "edvr_logs", "alpha")
          and "EDVR_RIG_STEP" not in child_env(env, Job(alpha), exe_dir),
          "a whole rig's child gets the log directory too, and still no step")

    # plan() marks the rigs with their serial groups; the direct run_group
    # tests below want none.
    plan(rigs, [], {})

    # A fake spawner: records start order and the peak concurrency, fails the
    # rig named "alpha" at once, and makes the others take a moment. The
    # moment is not a sleep to be outwaited: the others wait until a second
    # rig is up (an event, so a runner that never runs two at once is caught
    # without a clock) and then linger briefly, which is all a runner that
    # starts one rig too many needs to be seen doing it.
    lock = threading.Lock()
    starts, active, peak = [], [0], [0]
    two_up = threading.Event()

    def fake_spawn(job):
        with lock:
            starts.append(job.title)
            active[0] += 1
            peak[0] = max(peak[0], active[0])
            if active[0] >= 2:
                two_up.set()
        if job.rig.label != "alpha":
            two_up.wait(2.0)
            time.sleep(0.02)
        with lock:
            active[0] -= 1
        return (7, b"alpha says no") if job.rig.label == "alpha" else (0, b"%s ok" % job.rig.label.encode())

    out = io.BytesIO()
    results, failed = run_group([Job(beta), Job(gamma), Job(timing)], 2, fake_spawn, out)
    check(not failed and len(results) == 3, "every rig ran: %r" % results)
    check(starts == ["beta", "gamma", "timing"], "rigs start in plan order: %r" % starts)
    check(peak[0] == 2, "never more than --jobs rigs at once: %d" % peak[0])
    text = out.getvalue().decode()
    check(text.count("[edvr] --- ") == 3 and "beta ok\n" in text and "gamma ok\n" in text,
          "each rig prints one banner and its whole output: %r" % text)

    starts.clear()
    out = io.BytesIO()
    results, failed = run_group([Job(alpha), Job(beta), Job(gamma), Job(timing)], 2, fake_spawn, out)
    check([job.title for job, *_ in failed] == ["alpha"], "the failing rig is reported: %r" % failed)
    check(starts == ["alpha", "beta"], "a failure stops new launches; running rigs finish: %r" % starts)
    text = out.getvalue().decode()
    check(text.index("beta ok") < text.index("alpha says no") and "exit code 7" in text,
          "the failed rig's output comes last and names its exit code: %r" % text)

    # Held-back jobs of one group never overlap, the slot they cannot use goes
    # to another job, and the next one starts the moment the group frees. With
    # two jobs: gamma (whole, held back) and beta's build step start together;
    # beta's run step is queued the moment its build ends but must wait for
    # gamma, so alpha and then timing take the slot; beta's run step starts as
    # gamma ends.
    starts.clear()
    together, held = [False], [0]
    timing_up = threading.Event()

    def serial_spawn(job):
        with lock:
            starts.append(job.title)
            if job.constrained:
                held[0] += 1
                together[0] |= held[0] > 1
        if job.rig.label == "timing":
            timing_up.set()
        if job.rig.label == "gamma":
            # gamma outlasts everything that can run beside it: until timing
            # has started (2 s at most, which only a broken runner spends)
            timing_up.wait(2.0)
        with lock:
            if job.constrained:
                held[0] -= 1
        return 0, b""

    pool, _ = plan(rigs, [], {}, serial=[["beta", "gamma"]])
    check(titles(pool) == ["gamma", "beta (build)", "alpha", "timing"],
          "the chain leads, the split member's build step with it: %r" % pool)
    results, failed = run_group(pool, 2, serial_spawn, io.BytesIO())
    check(not failed and len(results) == 5 and not together[0],
          "two held-back jobs of one serial group never run together: %r" % results)
    check(starts == ["gamma", "beta (build)", "alpha", "timing", "beta (run)"],
          "a waiting run step yields its slot and starts when its group frees: %r" % starts)

    # --after through run_group: gamma must wait for alpha's real finish, not
    # merely for a launch slot -- proven by giving gamma the longest estimate
    # (so ranking alone would start it first) and --jobs wide enough that
    # nothing but the dependency could be holding it back.
    seconds2 = {"alpha": 1.0, "beta": 1.0, "gamma": 9.0, "timing": 1.0}
    pool, _ = plan(rigs, [], seconds2, after={"gamma": ["alpha"]})
    check(titles(pool) == ["gamma", "alpha", "beta", "timing"],
          "the consumer ranks by its own estimate, first here despite --after: %r" % pool)

    starts.clear()
    alpha_done, order_ok = [False], [True]
    gamma_up = threading.Event()

    def after_spawn(job):
        with lock:
            starts.append(job.title)
            if job.rig.label == "gamma" and not alpha_done[0]:
                order_ok[0] = False
        if job.rig.label == "gamma":
            gamma_up.set()
        if job.rig.label == "alpha":
            # alpha lingers until gamma has started, if it is going to: a
            # runner that lets gamma go early shows it at once, and a correct
            # one leaves gamma waiting out the linger (0.05 s, not a hazard)
            gamma_up.wait(0.05)
            with lock:
                alpha_done[0] = True
        return 0, b""

    results, failed = run_group(pool, 4, after_spawn, io.BytesIO())
    check(not failed and len(results) == 4 and order_ok[0],
          "gamma never starts before alpha finishes, though it ranks first and jobs=4 "
          "leaves it a free slot from the start: %r" % starts)

    # A consumer whose producer fails never gets the chance to start: once
    # anything has failed, run_group stops launching -- the same rule that
    # already left an ordinary pending job unstarted, just now also covering
    # the one --after was specifically added to hold back.
    starts.clear()

    def failing_after_spawn(job):
        with lock:
            starts.append(job.title)
        return (3, b"alpha broke") if job.rig.label == "alpha" else (0, b"")

    results, failed = run_group(pool, 4, failing_after_spawn, io.BytesIO())
    check([job.title for job, *_ in failed] == ["alpha"] and "gamma" not in starts,
          "gamma never starts once its producer has failed: %r" % starts)

    with tempfile.TemporaryDirectory() as scratch:
        script = Path(scratch) / "build.bat"
        script.write_text(SAMPLE, encoding="utf-8")
        times = Path(scratch) / "rig_times.json"
        out = io.BytesIO()
        code = run(script, 3, 2, ["timing"], times, True, out)
        check(code == 0 and not times.exists() and b"wrote nothing" in out.getvalue(),
              "dry run plans without writing: %r" % out.getvalue())
        check(b"gamma~5s alpha~3s beta~3s" in out.getvalue(), "dry run prints the plan: %r" % out.getvalue())
        out = io.BytesIO()
        code = run(script, 3, 2, ["timing"], times, True, out, exe_dir=scratch)
        check(code == 0 and not os.path.exists(os.path.join(scratch, "edvr_logs"))
              and os.path.join(scratch, "edvr_logs", "<rig>").encode() in out.getvalue(),
              "a dry run names the rigs' log directories and makes none: %r" % out.getvalue())
        if os.name == "nt":
            spawner(script, script.parent, dict(os.environ), scratch)(Job(timing))
            check(os.path.isdir(os.path.join(scratch, "edvr_logs", "timing")),
                  "a spawn makes the rig's log directory, both levels of it")
        out = io.BytesIO()
        code = run(script, 3, 2, ["timing"], times, True, out, serial=[["beta", "gamma"]])
        check(code == 0 and not times.exists()
              and b"one at a time among themselves: beta(run) gamma" in out.getvalue()
              and b"gamma~5s beta(build)~3s alpha~3s" in out.getvalue(),
              "dry run prints the serial groups, the chain leads, split members are marked: %r"
              % out.getvalue())
        out = io.BytesIO()
        code = run(script, 3, 2, ["timing"], times, True, out, after={"gamma": ["alpha"]})
        check(code == 0 and b"gamma waits for: alpha" in out.getvalue(),
              "run() threads --after through to plan() and describe(): %r" % out.getvalue())
        starts.clear()
        out = io.BytesIO()
        code = run(script, 3, 2, ["timing"], times, False, out,
                   spawn=lambda job: (0, b"ran " + job.title.encode()))
        check(code == 0 and times.is_file(), "a full run records the times")
        recorded = load_times(times)
        check(set(recorded) == {"alpha", "beta", "gamma", "timing"} and all(v >= 0 for v in recorded.values()),
              "every rig's seconds are recorded: %r" % recorded)
        check(b"[edvr] longest:" in out.getvalue() and b"1 quiet took" in out.getvalue(),
              "the summary names the longest rigs and the quiet group: %r" % out.getvalue())
        out = io.BytesIO()
        steps = []
        code = run(script, 3, 2, ["beta"], times, False, out,
                   spawn=lambda job: (steps.append((job.rig.label, job.step)), (0, b""))[1],
                   serial=[["alpha", "gamma"]])
        check(code == 0 and steps.count(("beta", "build")) == 1 and steps.count(("beta", "run")) == 1
              and steps[-1] == ("beta", "run") and ("beta", None) not in steps,
              "a split quiet rig is spawned twice, its run step last and alone: %r" % steps)
        text = out.getvalue()
        check(b"--- beta (build):" in text and b"--- beta (run):" in text
              and b"quiet, one at a time afterwards: beta(run)" in text,
              "each step gets its own banner: %r" % text)
        recorded = load_times(times)
        check("beta (run)" in recorded and "beta" in recorded and set(recorded) > {"alpha", "gamma"},
              "each step's seconds are recorded under its own name: %r" % recorded)
        times.write_text("not json", encoding="utf-8")
        check(load_times(times) == {}, "a corrupt times file is ignored, not fatal")
        merged_from = {"old": 4.0}
        save_times(times, merged_from, [(Job(alpha), 0, 1.25, b""), (Job(beta), 3, 9.0, b"")])
        check(load_times(times) == {"old": 4.0, "alpha": 1.25}, "saving merges and skips failures")
        out = io.BytesIO()
        code = run(script, 3, 2, [], times, False, out,
                   spawn=lambda job: (0, b"") if job.rig.label != "beta" else (5, b"beta broke"))
        check(code == 1 and b"ERROR: beta failed with exit code 5" in out.getvalue(),
              "a failing rig fails the run and the tail names it: %r" % out.getvalue())
        recorded = load_times(times)
        check("beta" not in recorded and recorded["old"] == 4.0 and "gamma" in recorded,
              "a failed rig does not update its time, the others do: %r" % recorded)
        out = io.BytesIO()
        steps.clear()
        code = run(script, 3, 2, [], times, False, out, serial=[["beta"]],
                   spawn=lambda job: (steps.append(job.step), (5, b"no") if job.step == "build" else (0, b""))[1])
        check(code == 1 and "run" not in steps and b"ERROR: beta (build) failed" in out.getvalue(),
              "a failed build step never gets its run step: %r %r" % (steps, out.getvalue()))

        # --- the focus guard: what run() does with a watch and what it finds ---
        import focus_watch

        def took_the_focus(job, exe="openxr_present_test.exe"):
            window = {"hwnd": 7, "pid": 41, "exe": exe, "cls": "Static", "title": "EDVR hidden Present fixture",
                      "rect": [0, 0, 64, 64]}
            return focus_watch.Finding("foreground", 4.0, window, (41, exe),
                                       ["%s(41)" % exe, "cmd.exe(40)", "python.exe(1)"], job)

        class FakeWatch:
            """A watch that was told what it saw, and counts its finishes."""
            def __init__(self, findings=()):
                self.findings, self.finished = list(findings), 0

            def finish(self):
                self.finished += 1
                return list(self.findings), (
                    focus_watch.summarize(self.findings, 3.0) + "\nfocus_watch: hooks saw 1 show, 0 console and "
                    "2 foreground event(s)" if self.findings else
                    "focus_watch: no window was shown, no console opened and the foreground never moved to a "
                    "window of the tree in 3 s (hooks saw 0 show, 0 console and 0 foreground event(s))")

        def all_ok(job):
            return 0, b"ran " + job.title.encode()

        calm = FakeWatch()
        out = io.BytesIO()
        code = run(script, 3, 2, [], times, False, out, spawn=all_ok, focus=lambda: calm)
        text = out.getvalue().decode()
        check(code == 0 and calm.finished == 1 and "[edvr] focus_watch: no window was shown" in text
              and text.index("focus_watch: no window") < text.index("[edvr] the pool of"),
              "a watch that saw nothing passes the run, is stopped once, and its line is in the build log: %r" % text)
        out = io.BytesIO()
        code = run(script, 3, 2, [], times, False, out, spawn=all_ok)
        check(code == 0 and b"focus" not in out.getvalue(),
              "with no focus argument nothing is watched and nothing is said about it: %r" % out.getvalue())

        taken = FakeWatch([took_the_focus("alpha"), took_the_focus("beta", "openxr_module_test.exe")])
        out = io.BytesIO()
        if times.exists():
            times.unlink()
        code = run(script, 3, 2, [], times, False, out, spawn=all_ok, focus=lambda: taken)
        text = out.getvalue().decode()
        check(code == 1 and taken.finished == 1 and "2 finding(s) in 2 group(s)" in text
              and "in job alpha" in text and "in job beta" in text
              and text.rstrip().splitlines()[-1].startswith("[edvr] ERROR: focus guard: 2 finding(s) by "
                                                            "openxr_module_test.exe, openxr_present_test.exe, in alpha, beta"),
              "a job that took the focus fails a run in which every rig passed, and the last line says who: %r"
              % text[-900:])
        check(set(load_times(times)) >= {"alpha", "beta", "gamma"},
              "the rigs' times are still recorded when the guard fails the run: %r" % load_times(times))

        taken = FakeWatch([took_the_focus("gamma")])
        out = io.BytesIO()
        code = run(script, 3, 2, [], times, False, out, focus=lambda: taken,
                   spawn=lambda job: (0, b"") if job.rig.label != "beta" else (5, b"beta broke"))
        text = out.getvalue().decode()
        lines = text.rstrip().splitlines()
        check(code == 1 and "1 finding(s) in 1 group(s)" in text and lines[-1].startswith("[edvr] ERROR: beta failed")
              and text.index("1 finding(s) in 1 group(s)") < text.index("ERROR: beta failed"),
              "when a rig fails as well, the guard's list is in the log and the failed rig is still the last line: %r"
              % text[-700:])

        def no_desktop():
            raise OSError("no window station")

        out = io.BytesIO()
        code = run(script, 3, 2, [], times, False, out, spawn=all_ok, focus=no_desktop)
        text = out.getvalue().decode()
        check(code == 0 and "NOTE: focus guard: this build is NOT being watched (OSError: no window station)" in text,
              "where the desktop cannot be watched the run says so and carries on: %r" % text)

        def not_importable():
            raise ImportError("No module named focus_watch")

        try:
            run(script, 3, 2, [], times, False, io.BytesIO(), spawn=all_ok, focus=not_importable)
            check(False, "a guard that cannot even be imported must fail the run, not be noted and skipped")
        except ImportError:
            pass

        def never_started():
            raise AssertionError("a dry run starts no watch")

        check(run(script, 3, 2, [], times, True, io.BytesIO(), focus=never_started) == 0,
              "a dry run starts no watch")

        # main() hands run() the real guard: without it this whole block guards nothing.
        wired = {}
        real_run = globals()["run"]
        globals()["run"] = lambda *args, **kwargs: wired.update(kwargs) or 0
        try:
            main(["--script", str(script), "--dry-run"])
        finally:
            globals()["run"] = real_run
        check(wired.get("focus") is focus_guard, "main() passes run() the focus guard: %r" % wired.get("focus"))

    # owns_exe_dir_entry / job_exe_dirs / cleanup_job_exe_dirs: the
    # "<label>-" / "<label>_" convention documented for a rig that stages a
    # private directory directly under --exe-dir. No shipped rig does this
    # today (see the module docstring for the one that used to); these
    # fixtures are the only exercise the convention gets.
    check(owns_exe_dir_entry("beta-1-2", "beta", ["alpha", "beta"]), "hyphen convention")
    check(owns_exe_dir_entry("beta_1_2", "beta", ["alpha", "beta"]), "underscore convention")
    check(not owns_exe_dir_entry("betaextra-1-2", "beta", ["alpha", "beta"]),
          "no separator right after the label is not a match")
    check(not owns_exe_dir_entry("beta", "beta", ["beta"]), "the bare label with nothing after it is not a match")
    check(not owns_exe_dir_entry("beta_extra_1_2", "beta", ["beta", "beta_extra"])
          and owns_exe_dir_entry("beta_extra_1_2", "beta_extra", ["beta", "beta_extra"]),
          "a directory goes to the longer, more specific label when one label prefixes another")

    with tempfile.TemporaryDirectory() as exe_dir:
        os.makedirs(os.path.join(exe_dir, "beta-1-2"))
        os.makedirs(os.path.join(exe_dir, "gamma-5-6"))
        Path(exe_dir, "beta-not-a-dir").write_text("x", encoding="utf-8")
        found = job_exe_dirs(exe_dir, Job(beta), ["alpha", "beta", "gamma"])
        check(found == [os.path.join(exe_dir, "beta-1-2")],
              "job_exe_dirs finds only its own rig's directories, never a file: %r" % found)

        kept = cleanup_job_exe_dirs(exe_dir, Job(beta), ["alpha", "beta", "gamma"], passed=False)
        check(kept == (os.path.join(exe_dir, "beta-1-2"),) and os.path.isdir(kept[0]),
              "a failed job's own directory is kept and returned: %r" % (kept,))
        check(cleanup_job_exe_dirs(exe_dir, Job(beta), ["alpha", "beta", "gamma"], passed=True) == ()
              and not os.path.exists(os.path.join(exe_dir, "beta-1-2")),
              "a passed job's own directory is removed")
        check(os.path.isdir(os.path.join(exe_dir, "gamma-5-6")), "another rig's directory is untouched")

    # End to end through run_group: a fake spawn that stages its own rig's
    # private directory, exercised both ways.
    with tempfile.TemporaryDirectory() as exe_dir:
        def staging_spawn(job):
            os.makedirs(os.path.join(exe_dir, "%s-1-2" % job.rig.label))
            return (0, b"ok") if job.rig.label != "alpha" else (3, b"alpha broke")

        out = io.BytesIO()
        _, failed = run_group([Job(beta)], 1, staging_spawn, out, exe_dir=exe_dir, labels=["beta"])
        check(not failed and not os.path.exists(os.path.join(exe_dir, "beta-1-2")),
              "run_group removes a passing job's own staged directory")

        out = io.BytesIO()
        _, failed = run_group([Job(alpha)], 1, staging_spawn, out, exe_dir=exe_dir, labels=["alpha"])
        alpha_dir = os.path.join(exe_dir, "alpha-1-2")
        check(len(failed) == 1 and failed[0][4] == (alpha_dir,) and os.path.isdir(alpha_dir),
              "run_group keeps a failing job's own staged directory and returns its path: %r" % (failed,))
        check(("kept " + alpha_dir).encode() in out.getvalue(),
              "the failure line names the kept directory: %r" % out.getvalue())

    # stale_exe_dirs / sweep_exe_dir: shape alone, no rig label involved --
    # the debris a since-deleted rig leaves has no rig left to claim it by
    # the label convention above, which is exactly why a separate,
    # label-blind sweep exists.
    with tempfile.TemporaryDirectory() as build_dir:
        stale_names = ["census-bridge-ready-6172-97754453", "vrtest_census_8916_151180000"]
        for name in stale_names:
            os.makedirs(os.path.join(build_dir, name))
        for name in ("edvr_logs", "obj", "gen", "vrtest_probe_matrix", "vrtest2"):
            os.makedirs(os.path.join(build_dir, name))
        Path(build_dir, "d3d11.dll").write_text("x", encoding="utf-8")
        # Same shape as the stale directories, but a file: never a candidate.
        Path(build_dir, "looks-like-1-2").write_text("x", encoding="utf-8")

        stale = stale_exe_dirs(build_dir)
        check(stale == sorted(os.path.join(build_dir, name) for name in stale_names),
              "stale_exe_dirs matches the hyphen and underscore shapes and nothing else: %r" % stale)
        check(stale_exe_dirs(os.path.join(build_dir, "missing")) == [],
              "a missing exe_dir is not an error, just nothing stale")

        out = io.BytesIO()
        would_remove = sweep_exe_dir(build_dir, True, out)
        check(would_remove == 2 and all(os.path.isdir(path) for path in stale)
              and b"dry run" in out.getvalue(),
              "--dry-run counts the stale directories and removes nothing: %r" % out.getvalue())

        removed = sweep_exe_dir(build_dir, False, io.BytesIO())
        check(removed == 2 and not any(os.path.exists(path) for path in stale)
              and os.path.isdir(os.path.join(build_dir, "obj"))
              and os.path.isfile(os.path.join(build_dir, "d3d11.dll"))
              and os.path.isfile(os.path.join(build_dir, "looks-like-1-2")),
              "a real sweep removes only the stale directories, leaving files and fixed outputs alone")
        check(sweep_exe_dir(build_dir, False, io.BytesIO()) == 0, "a second sweep finds nothing left to remove")

    self_test_timeouts(check)

    if failures:
        for why in failures:
            print("FAIL run_jobs: %s" % why)
        return 1
    print("run_jobs: self-test passed")
    return 0


@contextlib.contextmanager
def scratch_dir(prefix):
    """A temporary directory that is removed on the way out even when a process
    a failing test left behind still holds a file in it, which
    tempfile.TemporaryDirectory would turn into a traceback hiding the failure."""
    path = tempfile.mkdtemp(prefix=prefix)
    try:
        yield path
    finally:
        shutil.rmtree(path, ignore_errors=True)


def self_test_timeouts(check):
    """The per-rig timeout, the rig log, the tree kill and the job object
    (architecture review 2026-09-29, I-5). Everything with a real process is
    Windows-only; the timings below are seconds and generous, and no
    assertion depends on how fast the machine is beyond the kill bounds."""
    rigs = parse_rigs(SAMPLE)
    alpha, beta, gamma, timing = rigs
    plan(rigs, [], {})

    def within(seconds, function, *args, **kwargs):
        """(finished, result): a regression that hangs must fail the test, not stall the build."""
        box = []

        def call():
            try:
                box.append(function(*args, **kwargs))
            except BaseException as error:      # reported below, not lost with the thread
                box.append(error)
        thread = threading.Thread(target=call, daemon=True)
        thread.start()
        thread.join(seconds)
        return (True, box[0]) if box else (False, None)

    # --- rig_timeout: max(180 s, 3x the record), 900 s without one ----------
    known = {"alpha": 100.0, "beta": 10.0, "gamma": "junk", "timing": True, "beta (run)": 1000.0}
    check(rig_timeout(Job(alpha), known) == 300.0, "a rig that took 100 s may take 300 s: %r" % rig_timeout(Job(alpha), known))
    check(rig_timeout(Job(beta), known) == 180.0, "a quick rig still gets the 180 s floor")
    check(rig_timeout(Job(gamma), known) == 900.0 and rig_timeout(Job(timing), known) == 900.0
          and rig_timeout(Job(alpha), {}) == 900.0,
          "no usable record (none, junk, a boolean): 900 s")
    for bad in (float("inf"), float("nan"), -5.0):
        check(rig_timeout(Job(alpha), {"alpha": bad}) == 900.0, "a record of %r is not a usable record" % bad)
    check(rig_timeout(Job(beta, "run"), known) == 3000.0 and rig_timeout(Job(beta, "build"), known) == 180.0,
          "a rig's build and run steps are timed by their own records")
    check(rig_timeout(Job(alpha), known, scale=2.0) == 600.0 and rig_timeout(Job(gamma), known, scale=0.5) == 450.0,
          "scale multiplies whichever timeout applies")
    check(rig_timeout(Job(alpha), known, scale=2.0, override=50) == 50.0
          and rig_timeout(Job(gamma), known, override=0) is None,
          "an override is one figure for every rig, unscaled; 0 turns timeouts off")
    text = describe_timeouts()
    check("max(180 s, 3x its last time), 900 s with no record" in text and "all times" not in text
          and "all times 2" in describe_timeouts(2.0) and describe_timeouts(1.0, 0) == "rig timeouts are off"
          and "past 60 s" in describe_timeouts(1.0, 60), "the header line says what the timeouts are: %r" % text)

    def env_of(name, value, function, *args):
        old = os.environ.get(name)
        if value is None:
            os.environ.pop(name, None)
        else:
            os.environ[name] = value
        try:
            return function(*args)
        except ValueError as error:
            return error
        finally:
            if old is None:
                os.environ.pop(name, None)
            else:
                os.environ[name] = old

    check(env_of("EDVR_RIG_TIMEOUT", None, env_number, "EDVR_RIG_TIMEOUT") is None
          and env_of("EDVR_RIG_TIMEOUT", "  ", env_number, "EDVR_RIG_TIMEOUT", 1.0) == 1.0
          and env_of("EDVR_RIG_TIMEOUT", " 2.5 ", env_number, "EDVR_RIG_TIMEOUT") == 2.5
          and env_of("EDVR_RIG_TIMEOUT", "0", env_number, "EDVR_RIG_TIMEOUT") == 0.0,
          "the environment supplies the timeout figures")
    for bad in ("soon", "-1", "inf", "nan"):
        got = env_of("EDVR_RIG_TIMEOUT", bad, env_number, "EDVR_RIG_TIMEOUT")
        check(isinstance(got, ValueError) and "EDVR_RIG_TIMEOUT" in str(got),
              "%r is rejected, naming the variable: %r" % (bad, got))
    check(rig_log_path(None, Job(beta)) is None
          and rig_log_path(r"C:\b", Job(beta)) == os.path.join(r"C:\b", "edvr_logs", "beta", "runner_output.txt")
          and rig_log_path(r"C:\b", Job(beta, "build")).endswith("runner_output_build.txt")
          and rig_log_path(r"C:\b", Job(beta, "run")).endswith("runner_output_run.txt"),
          "a rig's console log sits in its log directory, one file per step")

    # --- what a timeout looks like in the report, without processes ----------
    many = b"".join(b"line %d\n" % n for n in range(100)) + b"[edvr] TIMEOUT: alpha was still running\n"

    def hung_alpha(job):
        return (TIMEOUT_EXIT, many, 5.0) if job.rig.label == "alpha" else (0, b"beta ok")

    out = io.BytesIO()
    results, failed = run_group([Job(alpha), Job(beta)], 2, hung_alpha, out)
    text = out.getvalue().decode()
    check(len(failed) == 1 and failed[0][0].title == "alpha" and failed[0][5] == 5.0
          and "alpha TIMED OUT after 5 s" in text and "--- alpha:" in text and "TIMEOUT (limit 5 s)" in text
          and "exit code" not in text,
          "a timed-out rig fails the group and is named TIMEOUT, not by an exit code: %r" % text)
    check("line 99\n" in text and "line 61\n" in text and "line 60\n" not in text
          and "(61 earlier lines of its output not shown)" in text and "was still running" in text
          and text.index("beta ok") < text.index("TIMEOUT (limit"),
          "a hung rig prints only the tail of its output, after the rigs that finished: %r" % text[-400:])
    with tempfile.TemporaryDirectory() as scratch:
        script = Path(scratch) / "build.bat"
        script.write_text(SAMPLE, encoding="utf-8")
        times = Path(scratch) / "rig_times.json"
        out = io.BytesIO()
        code = run(script, 3, 2, [], times, False, out,
                   spawn=lambda job: (TIMEOUT_EXIT, b"stuck", 7.0) if job.rig.label == "beta" else (0, b""))
        check(code == 1 and b"ERROR: beta TIMED OUT after 7 s" in out.getvalue()
              and b"failed with exit code" not in out.getvalue()
              and "beta" not in load_times(times) and "gamma" in load_times(times),
              "run() fails on a timeout, says so in its last line, and records no time for that rig: %r"
              % out.getvalue())
        out = io.BytesIO()
        code = run(script, 3, 2, ["timing"], times, True, out, timeout_override=42.0)
        check(code == 0 and b"each rig is killed if it runs past 42 s" in out.getvalue(),
              "a dry run says what the timeouts are: %r" % out.getvalue())
        with contextlib.redirect_stderr(io.StringIO()):
            for args in (["--timeout-scale", "0"], ["--timeout", "-1"]):
                try:
                    main(["--script", str(script)] + args)
                    check(False, "%r must be rejected" % args)
                except SystemExit as error:
                    check(error.code == 2, "%r is a usage error" % args)

    # --- a spawn that raises must fail its job, not hang the group ------------
    def raising(job):
        raise RuntimeError("could not start")

    finished, got = within(20, run_group, [Job(alpha)], 1, raising, io.BytesIO())
    check(finished and not isinstance(got, BaseException) and len(got[1]) == 1
          and got[1][0][1] == 1 and b"could not start" in got[1][0][3],
          "a spawn that raises fails its job with the error as output, and the group returns: %r" % (got,))

    if os.name != "nt":
        print("run_jobs: NOTE: not Windows; the process cases (tree kill, job object) are skipped")
        return

    tools_dir = os.path.dirname(os.path.abspath(__file__))

    # How these cases tell a process tree that is alive from one that is gone,
    # without waiting out a quiet spell: the hung rig's grandchild (a ping)
    # appends to a heartbeat file through its shell's redirection, and Windows
    # will not rename a file some process holds open without sharing it for
    # delete. The file is held while the tree lives and free the moment its
    # last member is gone.
    def held(path):
        probe = path + ".probe"
        try:
            os.rename(path, probe)
        except PermissionError:
            return True
        except FileNotFoundError:
            return False
        try:
            os.rename(probe, path)
        except OSError:
            pass
        return False

    def released(path, limit=15.0):
        """True once no process holds `path` open any more, i.e. the tree
        that was appending to it is gone; waits up to `limit` seconds."""
        deadline = time.monotonic() + limit
        while True:
            if not held(path):
                return True
            if time.monotonic() >= deadline:
                return False
            time.sleep(0.01)

    def heartbeat_starts(path, limit=10.0):
        """True once the file at `path` has something in it -- the grandchild
        has started -- waiting up to `limit` seconds (0: look once)."""
        deadline = time.monotonic() + limit
        while True:
            if os.path.exists(path) and os.path.getsize(path) > 0:
                return True
            if time.monotonic() >= deadline:
                return False
            time.sleep(0.01)

    with scratch_dir("edvr-run-jobs-") as scratch:
        root = Path(scratch)

        # --- the focus guard on real processes ------------------------------------
        # A rig that puts a window on the desktop fails the run; one that does
        # not, passes it. The window is 1x1 and off the desktop, shown without
        # activating: a visible top-level window to Windows, nothing to a person,
        # and it takes no focus.
        import focus_watch
        watched = root / "focus-guard"
        watched.mkdir()
        window_script = watched / "window.py"
        window_script.write_text(focus_watch._CHILD_WINDOW, encoding="utf-8")

        def guard_script(name, *labels):
            bodies = {"calm": ["echo calm"],
                      "showy": ['"%s" "%s" visible 2.5' % (sys.executable, window_script)]}
            lines = ["@echo off", 'if "%~1"=="--rig" goto run_rig', "exit /b 0", ":run_rig",
                     "call :rig_%~2", "exit /b %errorlevel%"]
            for label in labels:
                lines += [":rig_" + label] + bodies[label] + ["exit /b 0"]
            path = watched / name
            path.write_text("\r\n".join(lines) + "\r\n", encoding="ascii")
            return path

        # The window is the watch's own probe (its title says so), which only a watch
        # under test counts: the guard the build really runs, and any monitor
        # watching the build from outside, ignore it.
        def counting_guard():
            return focus_watch.FocusWatch(os.getpid(), count_probes=True).start()

        out = io.BytesIO()
        code = run(guard_script("both.bat", "calm", "showy"), 2, 2, [], None, False, out,
                   timeout_override=120.0, focus=counting_guard)
        text = out.getvalue().decode("utf-8", "replace")
        check(code == 1 and "window by python.exe" in text and "in job showy" in text and "in job calm" not in text
              and text.rstrip().splitlines()[-1].startswith("[edvr] ERROR: focus guard: 1 finding(s) by python.exe, in showy"),
              "a rig whose process shows a window fails the run, and the guard names the exe and the rig: %r"
              % text[-900:])
        real = focus_guard()
        try:
            check(isinstance(real, focus_watch.FocusWatch) and real.root == os.getpid() and not real.count_probes,
                  "the guard the build really runs watches this process's children and ignores the probe window")
        finally:
            real.stop()
        out = io.BytesIO()
        code = run(guard_script("calm.bat", "calm"), 2, 2, [], None, False, out,
                   timeout_override=120.0, focus=focus_guard)
        text = out.getvalue().decode("utf-8", "replace")
        check(code == 0 and "[edvr] focus_watch: no window was shown" in text and "ERROR" not in text,
              "a rig that leaves the desktop alone passes, and the log says the guard looked: %r" % text[-600:])

        # A job inherits the runner's error mode, which is how a crash opens no dialog.
        before_mode = get_error_mode()
        mode = 0
        try:
            quiet_faults()
            probe = subprocess.run([sys.executable, "-c", "import ctypes; print(ctypes.WinDLL('kernel32').GetErrorMode())"],
                                   stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, text=True, timeout=60,
                                   **child_process_kwargs())
            mode = int(probe.stdout.strip() or 0)
        finally:
            set_error_mode(before_mode)
        check(mode & QUIET_FAULTS == QUIET_FAULTS,
              "a job of the runner starts with crash, assert and missing-DLL dialogs off: %#x" % mode)
        check(get_error_mode() == before_mode, "the case put the error mode back")

        def write_script(folder, name, labels, heart):
            """A stand-in for build.bat in `folder`: `--rig <label>` runs
            :rig_<label>. The hung rig sits in a ping, three processes down
            from the runner (rig cmd.exe, an inner cmd.exe, ping.exe), which
            appends a line a second to `heart`. It says "finished hang" only
            if the ping ran to its end. taskkill /T ends a tree leaf first, so
            a rig whose ping was killed can run one more line before it is
            killed itself (seen under load): that line must not be a print,
            which is what the errorlevel test is for -- a killed ping exits 1."""
            bodies = {
                "ok": ["echo before from ok", "echo finished ok 1>&2", "exit /b 0"],
                "fail": ["echo before from fail", "exit /b 5"],
                "hang": ["echo before the hang", 'cmd /d /c ping -n 300 127.0.0.1 >> "' + heart + '"',
                         "if errorlevel 1 exit /b 1", "echo finished hang", "exit /b 0"],
            }
            lines = ["@echo off", 'if "%~1"=="--rig" goto run_rig', "exit /b 0", ":run_rig",
                     "call :rig_%~2", "exit /b %errorlevel%"]
            for label in labels:
                lines += [":rig_" + label] + bodies[label]
            path = folder / name
            path.write_text("\r\n".join(lines) + "\r\n", encoding="ascii")
            return path

        # A runner that dies takes its children with it (the job object). The
        # stand-in runner is started first, so its start-up (it imports this
        # module) is spent while the cases below run; it is judged after them.
        # Its heartbeat file has a directory of its own: the case that lists
        # `scratch` must not see it appear.
        os.mkdir(os.path.join(scratch, "job-object"))
        heart2 = os.path.join(scratch, "job-object", "heartbeat.txt")
        fake_runner = (
            "import subprocess, sys, time\n"
            "sys.path.insert(0, sys.argv[1])\n"
            "import run_jobs\n"
            "try:\n"
            "    run_jobs.contain_children()\n"
            "except OSError as error:\n"
            "    print('uncontained', error.winerror, error, flush=True)\n"
            "else:\n"
            "    print('contained', flush=True)\n"
            "subprocess.Popen('cmd /d /c ping -n 300 127.0.0.1 >> \"' + sys.argv[2] + '\"',\n"
            "                 stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)\n"
            "time.sleep(300)\n")
        runner = subprocess.Popen([sys.executable, "-c", fake_runner, tools_dir, heart2],
                                  stdout=subprocess.PIPE, text=True)
        try:
            script = write_script(root, "build.bat", ("ok", "fail", "hang"), os.path.join(scratch, "unused.txt"))
            ok, fail = Job(Rig("ok", 1)), Job(Rig("fail", 1))
            env = dict(os.environ)

            # Normal jobs are unaffected: output merged, exit codes carried, the
            # log file holds exactly what the report prints, nothing marks them.
            spawn = spawner(script, root, env, scratch, lambda job: 120.0)
            code, output, hit = spawn(ok)
            log = rig_log_path(scratch, ok)
            check(code == 0 and hit is None and b"before from ok" in output and b"finished ok" in output,
                  "a normal rig runs as before, stderr merged into its output: %r %r" % (code, output))
            check(os.path.isfile(log) and Path(log).read_bytes() == output,
                  "its output is the same bytes in its log file: %s" % log)
            code, output, hit = spawn(fail)
            check(code == 5 and hit is None and b"before from fail" in output,
                  "a failing rig still fails with its own exit code: %r %r" % (code, output))
            before = sorted(os.listdir(scratch))
            code, output, hit = spawner(script, root, env, None, None)(ok)
            check(code == 0 and b"finished ok" in output and hit is None and sorted(os.listdir(scratch)) == before,
                  "without --exe-dir or a timeout the output is still captured, leaving no file: %r" % output)

            # The whole path: run() -> rig_timeout -> spawner -> run_rig -> kill_tree.
            # The rig that hangs is killed at the timeout, one second here: a rig
            # that gets going takes ~40 ms. A machine so loaded that the hung rig
            # did not get going (its first line streamed, its grandchild started)
            # or the rig beside it did not finish within the second has shown
            # nothing about the kill, so the case is run again with a limit
            # neither can miss; the checks read the last run.
            def hung_pool(limit):
                folder = root / ("pool-%d" % limit)
                folder.mkdir()
                heart = str(folder / "heartbeat.txt")
                pool_script = write_script(folder, "pool.bat", ("ok", "hang"), heart)
                hang = Job(Rig("hang", 1))
                times = folder / "rig_times.json"
                out = io.BytesIO()
                outcome = []
                worker = threading.Thread(
                    target=lambda: outcome.append(run(pool_script, 2, 2, [], times, False, out,
                                                      exe_dir=str(folder), timeout_override=float(limit))),
                    daemon=True)
                begun = time.monotonic()
                worker.start()
                hang_log = rig_log_path(str(folder), hang)
                streamed = False
                while worker.is_alive():
                    if os.path.isfile(hang_log) and b"before the hang" in Path(hang_log).read_bytes():
                        streamed = worker.is_alive()
                        break
                    time.sleep(0.01)
                worker.join(limit + KILL_WAIT + 30)
                return {"limit": limit, "heart": heart, "log": hang_log, "streamed": streamed,
                        "outcome": outcome, "alive": worker.is_alive(), "took": time.monotonic() - begun,
                        "text": out.getvalue().decode("utf-8", "replace"),
                        "recorded": load_times(times)}

            seen = hung_pool(1)
            if not (seen["streamed"] and heartbeat_starts(seen["heart"], 0) and "ok" in seen["recorded"]):
                seen = hung_pool(4)
            limit, hang_log = seen["limit"], seen["log"]
            check(seen["streamed"], "a rig's output is in its log file while the rig is still running")
            check(seen["outcome"] == [1] and not seen["alive"],
                  "a hung rig fails the run instead of hanging it: %r" % (seen["outcome"],))
            text = seen["text"]
            check("--- hang:" in text and "TIMEOUT (limit %d s)" % limit in text
                  and "hang TIMED OUT after %d s" % limit in text
                  and "ERROR: hang TIMED OUT after %d s" % limit in text and "before the hang" in text
                  and "finished hang" not in text and "Its whole log: " + hang_log in text
                  and "--- ok:" in text and "before from ok" in text,
                  "the report names the TIMEOUT, prints the partial output and the log, and the other rig ran: %r"
                  % text[-900:])
            check(seen["took"] < limit + 12.0, "killed and reported within a bound: %.1f s" % seen["took"])
            check(heartbeat_starts(seen["heart"]), "the rig's grandchild had started before the kill")
            check(released(seen["heart"]), "the rig's whole process tree is gone after the timeout")
            check(os.path.isfile(hang_log) and b"before the hang" in Path(hang_log).read_bytes()
                  and b"finished hang" not in Path(hang_log).read_bytes(),
                  "the hung rig's partial output survives in its log")
            recorded = seen["recorded"]
            check("hang" not in recorded and "ok" in recorded,
                  "a killed rig records no time; the rig that passed does: %r" % recorded)

            # The stand-in runner, started above, is judged now.
            first = runner.stdout.readline().strip()
            if first.startswith("uncontained 5 "):
                # ERROR_ACCESS_DENIED: an enclosing job forbids nesting. The runner
                # says one line and carries on; the guarantee cannot be tested here.
                print("run_jobs: NOTE: this process may not join a job object here (%s); "
                      "the orphan-protection case is skipped" % first)
            else:
                check(first == "contained", "the runner joins a job object: %r" % first)
                if first == "contained":
                    check(heartbeat_starts(heart2), "the stand-in runner's child is running")
                    check(held(heart2), "and holds its heartbeat file open, which is how the tree is told from a dead one")
                    runner.kill()
                    runner.wait(30)
                    check(released(heart2),
                          "when the runner is killed the job object takes its children with it")
        finally:
            runner.kill()
            runner.wait(30)
            runner.stdout.close()


def env_number(name, default=None):
    """The non-negative number in environment variable `name`, `default` when
    it is unset or empty; ValueError, naming it, when it is anything else."""
    text = os.environ.get(name, "").strip()
    if not text:
        return default
    try:
        value = float(text)
    except ValueError:
        value = None
    if value is None or not math.isfinite(value) or value < 0:
        raise ValueError("%s must be a number of 0 or more, not %r" % (name, text))
    return value


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--script", type=Path, help="the batch file whose :rig_ subroutines to run")
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 1,
                        help="rigs to run at once (default: every logical core)")
    parser.add_argument("--mp", type=int, default=None,
                        help="the /MP count each parallel rig's compiles get (default: 4, so "
                             "--jobs rigs start at most four compilers each; every core when "
                             "--jobs is 1). A --quiet rig runs alone and always gets every core.")
    parser.add_argument("--exe-dir", type=Path, default=None, metavar="DIR",
                        help="the directory the test exes are built into; each rig's processes "
                             "from there log under DIR\\edvr_logs\\<rig> (EDVR_LOG_DIR and "
                             "EDVR_LOG_DIR_FOR, read by src\\common\\config.cpp)")
    parser.add_argument("--serial", action="append", default=[], metavar="LABELS",
                        help="comma-separated rigs that never run at the same time as one "
                             "another (repeat for another such group)")
    parser.add_argument("--quiet", default="", help="comma-separated rigs to run alone afterwards")
    parser.add_argument("--after", action="append", default=[], metavar="CONSUMER=PRODUCERS",
                        help="CONSUMER does not start until every comma-separated PRODUCER has "
                             "finished (repeat for another consumer, or the same one again to "
                             "add producers); neither side may be --quiet")
    parser.add_argument("--times", type=Path, default=None, help="where rig durations are recorded")
    parser.add_argument("--timeout", type=float, default=None, metavar="SECONDS",
                        help="kill any rig still running after this long, replacing the "
                             "default rule (max(180 s, 3x its recorded time), 900 s with no "
                             "record); 0 turns timeouts off (environment: EDVR_RIG_TIMEOUT)")
    parser.add_argument("--timeout-scale", type=float, default=None, metavar="FACTOR",
                        help="multiply every rig's timeout by this, for a slow or heavily "
                             "loaded machine (environment: EDVR_RIG_TIMEOUT_SCALE)")
    parser.add_argument("--dry-run", action="store_true", help="print the plan, write nothing "
                                                                "(or, with --sweep-exe-dir, remove nothing)")
    parser.add_argument("--sweep-exe-dir", type=Path, default=None, metavar="DIR",
                        help="remove stale per-instance exe directories left under DIR by any "
                             "earlier build (see stale_exe_dirs), then exit; independent of "
                             "--script. --dry-run lists them and removes nothing")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args(argv)
    if args.self_test:
        return self_test()
    if args.sweep_exe_dir is not None:
        if not args.sweep_exe_dir.is_dir():
            parser.error("--sweep-exe-dir must name an existing directory")
        sweep_exe_dir(os.path.abspath(str(args.sweep_exe_dir)), args.dry_run, sys.stdout.buffer)
        return 0
    if args.script is None or not args.script.is_file():
        parser.error("--script must name an existing batch file")
    if args.jobs < 1:
        parser.error("--jobs must be at least 1")
    mp = args.mp if args.mp is not None else (0 if args.jobs == 1 else 4)
    quiet = [label for label in args.quiet.split(",") if label]
    serial = [[label for label in group.split(",") if label] for group in args.serial]
    after = {}
    for spec in args.after:
        consumer, sep, producers = spec.partition("=")
        producer_labels = [label for label in producers.split(",") if label]
        if not sep or not consumer or not producer_labels:
            parser.error("--after must be CONSUMER=PRODUCER[,PRODUCER...]: %r" % spec)
        after.setdefault(consumer, []).extend(producer_labels)
    exe_dir = None
    if args.exe_dir is not None:
        if not args.exe_dir.is_dir():
            parser.error("--exe-dir must name an existing directory")
        # Absolute but not resolved: the exes compare it with the path they
        # were started by, and a junction resolved away would never match.
        exe_dir = os.path.abspath(str(args.exe_dir))
    try:
        override = args.timeout if args.timeout is not None else env_number("EDVR_RIG_TIMEOUT")
        scale = (args.timeout_scale if args.timeout_scale is not None
                 else env_number("EDVR_RIG_TIMEOUT_SCALE", 1.0))
    except ValueError as error:
        parser.error(str(error))
    if override is not None and override < 0:
        parser.error("--timeout must be 0 or more")
    if not scale > 0:
        parser.error("--timeout-scale must be more than 0")
    if not args.dry_run:
        try:
            contain_children()
        except OSError as error:
            emit(sys.stdout.buffer, "[edvr] NOTE: the runner could not join a job object (%s); if it "
                                    "is killed, the rigs it started will not be.\n" % error)
            sys.stdout.buffer.flush()
        if os.name == "nt":
            quiet_faults()
            emit(sys.stdout.buffer, "[edvr] the jobs run with crash, assert and missing-DLL dialogs off "
                                    "(error mode 0x%X, inherited)\n" % get_error_mode())
            sys.stdout.buffer.flush()
    try:
        return run(args.script.resolve(), args.jobs, mp, quiet, args.times, args.dry_run,
                   sys.stdout.buffer, serial=serial, exe_dir=exe_dir, after=after,
                   timeout_scale=scale, timeout_override=override, focus=focus_guard)
    except ValueError as error:
        print("[edvr] ERROR: %s" % error)
        return 1


if __name__ == "__main__":
    sys.exit(main())
