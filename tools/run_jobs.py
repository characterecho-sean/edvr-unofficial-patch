#!/usr/bin/env python3
"""Run build.bat's independent test rigs concurrently.

build.bat builds the DLLs itself, then hands every `:rig_<label>` subroutine
in its own text to this runner. Each rig is started as a `build.bat --rig
<label>` child -- the child inherits the parent's toolchain environment and
jumps straight to its subroutine -- N at a time, longest first. A rig's output
is held until it finishes and printed in one piece under a banner, so the log
still reads as if the rigs had run one after another.

  python tools\\run_jobs.py --script build.bat [--jobs N] [--mp N]
                           [--serial a,b,c]... [--quiet d,e]
                           [--times build\\rig_times.json] [--dry-run]
  python tools\\run_jobs.py --self-test

--serial names rigs that must not run at the same time as one another, though
any of them may run beside the rest: rigs whose test processes load the same
proxy DLL from the same directory, say, and so share its crash sentinels. Each
--serial is one such group; the group is scheduled as a chain, started early
because its members can only follow one another.

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


class Rig:
    __slots__ = ("label", "line", "sources", "two_step", "group")

    def __init__(self, label, line, sources=0, two_step=False):
        self.label, self.line, self.sources, self.two_step = label, line, sources, two_step
        self.group = None   # index of its --serial group, if any; set by plan()

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


def plan(rigs, quiet, times, serial=()):
    """The pool, longest expected first, and the quiet jobs in the order
    named. Every quiet or serial label must be a rig, and a rig belongs to at
    most one of the serial groups. A serial or quiet rig that splits itself
    puts its build step in the pool and only its run step under the rule. A
    serial group's jobs -- a whole rig, or the build step whose run step will
    join the chain -- are ranked by the whole chain's expected length rather
    than their own: the chain starts first and never becomes the tail."""
    by_label = {rig.label: rig for rig in rigs}
    for option, labels in [("--quiet", quiet)] + [("--serial", group) for group in serial]:
        unknown = [label for label in labels if label not in by_label]
        if unknown:
            raise ValueError("%s names rigs the script does not define: %s"
                             % (option, ", ".join(unknown)))
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


def child_env(env, job):
    """The child's environment: the runner's, plus the step for a split rig."""
    return env if job.step is None else dict(env, EDVR_RIG_STEP=job.step)


def spawner(script, root, env):
    def spawn(job):
        completed = subprocess.run(child_command(script, job.rig.label), cwd=str(root),
                                   env=child_env(env, job), stdin=subprocess.DEVNULL,
                                   stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        return completed.returncode, completed.stdout
    return spawn


def emit(out, text):
    out.write(text.encode("utf-8", "replace"))


def report(out, job, code, seconds, output):
    emit(out, "[edvr] --- %s: %.1f s%s ---\n" % (job.title, seconds,
                                                "" if code == 0 else ", exit code %d" % code))
    out.write(output)
    if output and not output.endswith(b"\n"):
        out.write(b"\n")
    out.flush()


def launchable(pending, busy):
    """The first pending job that no group rule holds back; None when every
    pending job is waiting on its group."""
    for job in pending:
        if not job.constrained or job.rig.group not in busy:
            return job
    return None


def run_group(order, jobs, spawn, out, clock=time.monotonic):
    """Start spawn(job) for the jobs in order, at most jobs at a time and
    never two held-back jobs of one serial group. A serial rig's run step is
    queued, at the front, the moment its build step succeeds. Each result is
    printed as it completes, except failures, which are held and printed
    after the group drains. Returns (results, failures) where each entry is
    (job, code, seconds, output) in completion order."""
    done = queue.Queue()

    def worker(job):
        started = clock()
        code, output = spawn(job)
        done.put((job, code, clock() - started, output))

    pending, running, busy, results, failures = list(order), 0, set(), [], []
    while pending or running:
        while pending and running < jobs and not failures:
            job = launchable(pending, busy)
            if job is None:
                break
            pending.remove(job)
            if job.constrained:
                busy.add(job.rig.group)
            threading.Thread(target=worker, args=(job,), daemon=True).start()
            running += 1
        if not running:
            break
        job, code, seconds, output = done.get()
        running -= 1
        if job.constrained:
            busy.discard(job.rig.group)
        results.append((job, code, seconds, output))
        if code == 0:
            report(out, job, code, seconds, output)
            if job.step == "build" and job.rig.group is not None:
                pending.insert(0, Job(job.rig, "run"))
            continue
        failures.append((job, code, seconds, output))
        if len(failures) == 1:
            emit(out, "[edvr] %s failed (exit code %d); no more rigs start, %d still running\n"
                 % (job.title, code, running))
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
    merged.update({job.key: round(seconds, 2) for job, code, seconds, _ in results if code == 0})
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


def describe(pool, quiet, times, serial=()):
    split = {job.rig.label for job in pool if job.step == "build"}

    def name(label):
        return label + ("(run)" if label in split else "")

    text = "[edvr] parallel, longest expected first:\n"
    text += wrap("%s~%.0fs" % (job.title.replace(" ", ""), estimate(job, times))
                 for job in pool) + "\n"
    for group in serial:
        text += "[edvr] one at a time among themselves: " + " ".join(name(label) for label in group) + "\n"
    if quiet:
        text += "[edvr] quiet, one at a time afterwards: " + " ".join(name(job.rig.label) for job in quiet) + "\n"
    return text


def run(script, jobs, mp, quiet, times_path, dry_run, out, spawn=None, clock=time.monotonic,
        serial=()):
    text = script.read_text(encoding="utf-8", errors="replace")
    rigs = parse_rigs(text)
    times = load_times(times_path)
    pool, later = plan(rigs, quiet, times, serial)
    mp_flag = "/MP%d" % mp if mp else "/MP"
    emit(out, "[edvr] %d rigs from %s: %d jobs in the pool, %d at a time (CL=%s), %d quiet\n"
         % (len(rigs), script.name, len(pool), jobs, mp_flag, len(later)))
    emit(out, describe(pool, later, times, serial))
    out.flush()
    if dry_run:
        emit(out, "[edvr] dry run: wrote nothing.\n")
        out.flush()
        return 0
    quiet_spawn = spawn
    if spawn is None:
        env = dict(os.environ)
        env["CL"] = mp_flag
        spawn = spawner(script, script.parent, env)
        # A quiet rig has the machine to itself, so its compiles may use every core.
        quiet_spawn = spawner(script, script.parent, dict(env, CL="/MP"))
    started = clock()
    results, failures = run_group(pool, jobs, spawn, out, clock)
    pool_seconds = clock() - started
    summed = sum(seconds for _, _, seconds, _ in results)
    quiet_results, quiet_failures, quiet_seconds = [], [], 0.0
    if not failures:
        started = clock()
        quiet_results, quiet_failures = run_group(later, 1, quiet_spawn, out, clock)
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
                                             for job, _, seconds, _ in longest) + "\n")
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

    check(child_command(Path(r"C:\x y\build.bat"), "alpha") == r'cmd.exe /d /c ""C:\x y\build.bat" --rig alpha"',
          "child command quotes the script for cmd /c")
    env = {"CL": "/MP4"}
    check(child_env(env, Job(alpha)) == env and "EDVR_RIG_STEP" not in child_env(env, Job(alpha)),
          "a whole rig's child gets the runner's environment as it is")
    check(child_env(env, Job(beta, "run")) == {"CL": "/MP4", "EDVR_RIG_STEP": "run"}
          and child_env(env, Job(beta, "build"))["EDVR_RIG_STEP"] == "build",
          "a step's child is told which step it is")

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
        code = run(script, 3, 2, ["timing"], times, True, out, serial=[["beta", "gamma"]])
        check(code == 0 and not times.exists()
              and b"one at a time among themselves: beta(run) gamma" in out.getvalue()
              and b"gamma~5s beta(build)~3s alpha~3s" in out.getvalue(),
              "dry run prints the serial groups, the chain leads, split members are marked: %r"
              % out.getvalue())
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
    parser.add_argument("--serial", action="append", default=[], metavar="LABELS",
                        help="comma-separated rigs that never run at the same time as one "
                             "another (repeat for another such group)")
    parser.add_argument("--quiet", default="", help="comma-separated rigs to run alone afterwards")
    parser.add_argument("--times", type=Path, default=None, help="where rig durations are recorded")
    parser.add_argument("--dry-run", action="store_true", help="print the plan, write nothing")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args(argv)
    if args.self_test:
        return self_test()
    if args.script is None or not args.script.is_file():
        parser.error("--script must name an existing batch file")
    if args.jobs < 1:
        parser.error("--jobs must be at least 1")
    mp = args.mp if args.mp is not None else (0 if args.jobs == 1 else 4)
    quiet = [label for label in args.quiet.split(",") if label]
    serial = [[label for label in group.split(",") if label] for group in args.serial]
    try:
        return run(args.script.resolve(), args.jobs, mp, quiet, args.times, args.dry_run,
                   sys.stdout.buffer, serial=serial)
    except ValueError as error:
        print("[edvr] ERROR: %s" % error)
        return 1


if __name__ == "__main__":
    sys.exit(main())
