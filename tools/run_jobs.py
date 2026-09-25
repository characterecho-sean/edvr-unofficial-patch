#!/usr/bin/env python3
"""Run build.bat's independent test rigs concurrently.

build.bat builds the DLLs itself, then hands every `:rig_<label>` subroutine
in its own text to this runner. Each rig is started as a `build.bat --rig
<label>` child -- the child inherits the parent's toolchain environment and
jumps straight to its subroutine -- N at a time, longest first. A rig's output
is held until it finishes and printed in one piece under a banner, so the log
still reads as if the rigs had run one after another.

  python tools\\run_jobs.py --script build.bat [--jobs N] [--mp N]
                           [--exe-dir build] [--serial a,b,c]... [--quiet d,e]
                           [--after c=p,p]... [--times build\\rig_times.json]
                           [--dry-run]
  python tools\\run_jobs.py --sweep-exe-dir build [--dry-run]
  python tools\\run_jobs.py --self-test

--exe-dir is the directory the rigs' test exes are built into. Every process
a rig starts from there is told, through EDVR_LOG_DIR and EDVR_LOG_DIR_FOR
(src\\common\\config.cpp), to log under <exe-dir>\\edvr_logs\\<label> instead
of <exe-dir>\\edvr_logs: the proxy DLLs a test loads keep their logs and
crash sentinels there, so two rigs' proxies never read each other's sentinels
and stand down. A child a rig stages in a private directory keeps its own
default, which the rigs that read such a child's log rely on.

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

--times is where the runner records how long each rig took, and reads it back
next time so the longest rigs start first. A rig with no record is estimated
from how many sources its subroutine compiles.

A failing rig stops new launches. The rigs already running finish and print,
the failed rig's output is printed last so the tail of the build log names the
failure, and the exit code is 1. --dry-run prints the plan and writes nothing.
"""
import argparse
import io
import json
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
    __slots__ = ("label", "line", "sources", "two_step", "group", "after")

    def __init__(self, label, line, sources=0, two_step=False):
        self.label, self.line, self.sources, self.two_step = label, line, sources, two_step
        self.group = None   # index of its --serial group, if any; set by plan()
        self.after = ()     # labels it must not start before finish; set by plan()

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


def parse_rigs(text):
    """Every :rig_<label> in the script, in order, with the number of sources
    its subroutine compiles and whether it honours EDVR_RIG_STEP (the text up
    to the next label)."""
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
    return rigs


def estimate(job, times):
    """Seconds a job is expected to take: the last measurement, else a guess
    (a compile is about a second a file, a run a couple of seconds)."""
    known = times.get(job.key)
    if isinstance(known, (int, float)) and known >= 0:
        return float(known)
    return 2.0 if job.step == "run" else 2.0 + 0.7 * job.rig.sources


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
    rather than stranding a job that can never become launchable."""
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


def spawner(script, root, env, exe_dir=None):
    def spawn(job):
        if exe_dir is not None:
            # Log::open and the sentinel create one level; this is two.
            os.makedirs(log_dir(exe_dir, job), exist_ok=True)
        completed = subprocess.run(child_command(script, job.rig.label), cwd=str(root),
                                   env=child_env(env, job, exe_dir), stdin=subprocess.DEVNULL,
                                   stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                   **child_process_kwargs())
        return completed.returncode, completed.stdout
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


def report(out, job, code, seconds, output, kept=()):
    extra = "" if code == 0 else ", exit code %d" % code
    if kept:
        extra += ", kept " + ", ".join(kept)
    emit(out, "[edvr] --- %s: %.1f s%s ---\n" % (job.title, seconds, extra))
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
    failures) where each entry is (job, code, seconds, output, kept) in
    completion order; kept is empty except for a failed job with a directory
    of its own."""
    if done is None:
        done = set()
    outcomes = queue.Queue()

    def worker(job):
        started = clock()
        code, output = spawn(job)
        kept = cleanup_job_exe_dirs(exe_dir, job, labels, code == 0) if exe_dir is not None else ()
        outcomes.put((job, code, clock() - started, output, kept))

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
        job, code, seconds, output, kept = outcomes.get()
        running -= 1
        if job.constrained:
            busy.discard(job.rig.group)
        results.append((job, code, seconds, output, kept))
        if code == 0:
            report(out, job, code, seconds, output)
            if job.step == "build" and job.rig.group is not None:
                pending.insert(0, Job(job.rig, "run"))
            else:
                done.add(job.rig.label)
            continue
        failures.append((job, code, seconds, output, kept))
        if len(failures) == 1:
            note = "" if not kept else " (kept %s)" % ", ".join(kept)
            emit(out, "[edvr] %s failed (exit code %d)%s; no more rigs start, %d still running\n"
                 % (job.title, code, note, running))
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


def run(script, jobs, mp, quiet, times_path, dry_run, out, spawn=None, clock=time.monotonic,
        serial=(), exe_dir=None, after=None):
    text = script.read_text(encoding="utf-8", errors="replace")
    rigs = parse_rigs(text)
    labels = tuple(rig.label for rig in rigs)
    times = load_times(times_path)
    pool, later = plan(rigs, quiet, times, serial, after)
    mp_flag = "/MP%d" % mp if mp else "/MP"
    emit(out, "[edvr] %d rigs from %s: %d jobs in the pool, %d at a time (CL=%s), %d quiet\n"
         % (len(rigs), script.name, len(pool), jobs, mp_flag, len(later)))
    emit(out, describe(pool, later, times, serial, after))
    if exe_dir is not None:
        emit(out, "[edvr] each rig's processes log under %s\n" % os.path.join(exe_dir, "edvr_logs", "<rig>"))
    out.flush()
    if dry_run:
        emit(out, "[edvr] dry run: wrote nothing.\n")
        out.flush()
        return 0
    quiet_spawn = spawn
    if spawn is None:
        env = dict(os.environ)
        env["CL"] = mp_flag
        spawn = spawner(script, script.parent, env, exe_dir)
        # A quiet rig has the machine to itself, so its compiles may use every core.
        quiet_spawn = spawner(script, script.parent, dict(env, CL="/MP"), exe_dir)
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
    results += quiet_results
    failures += quiet_failures
    if times_path is not None:
        try:
            save_times(times_path, times, results)
        except OSError as error:
            emit(out, "[edvr] NOTE: could not record rig times in %s: %s\n" % (times_path, error))
    if failures:
        job, code = failures[0][0], failures[0][1]
        emit(out, "[edvr] ERROR: %s failed with exit code %d (%d of %d jobs ran)\n"
             % (job.title, code, len(results), len(pool) + len(later)))
        out.flush()
        return 1
    emit(out, "[edvr] the pool of %d jobs took %.1f s (%.1f s summed, %d at a time); "
              "%d quiet took %.1f s\n" % (len(pool), pool_seconds, summed, jobs, len(later), quiet_seconds))
    longest = sorted(results, key=lambda entry: -entry[2])[:5]
    emit(out, "[edvr] longest: " + ", ".join("%s %.1f s" % (job.title, seconds)
                                             for job, _, seconds, *_ in longest) + "\n")
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
    # rig named "alpha" at once, and makes the others take a moment.
    lock = threading.Lock()
    starts, active, peak = [], [0], [0]

    def fake_spawn(job):
        with lock:
            starts.append(job.title)
            active[0] += 1
            peak[0] = max(peak[0], active[0])
        time.sleep(0.2 if job.rig.label != "alpha" else 0.0)
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
    sleeps = {"gamma": 0.4, "alpha": 0.2}

    def serial_spawn(job):
        with lock:
            starts.append(job.title)
            if job.constrained:
                held[0] += 1
                together[0] |= held[0] > 1
        time.sleep(sleeps.get(job.rig.label, 0.05))
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

    def after_spawn(job):
        with lock:
            starts.append(job.title)
            if job.rig.label == "gamma" and not alpha_done[0]:
                order_ok[0] = False
        time.sleep(0.15 if job.rig.label == "alpha" else 0.0)
        if job.rig.label == "alpha":
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

    if failures:
        for why in failures:
            print("FAIL run_jobs: %s" % why)
        return 1
    print("run_jobs: self-test passed")
    return 0


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
        return run(args.script.resolve(), args.jobs, mp, quiet, args.times, args.dry_run,
                   sys.stdout.buffer, serial=serial, exe_dir=exe_dir, after=after)
    except ValueError as error:
        print("[edvr] ERROR: %s" % error)
        return 1


if __name__ == "__main__":
    sys.exit(main())
