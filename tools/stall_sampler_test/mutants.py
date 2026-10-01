#!/usr/bin/env python3
"""The mutation proof for tools\\stall_sampler_test: the rig fails when a rule of the stall sampler is flipped.

The rig (stall_sampler_test.cpp) runs the REAL src\\common\\stall_sampler.h against threads it blocks in places it knows, and reads that header
and src\\d3d11\\stall_watch.cpp as text for the shape of the code (no injection API, the stopped window straight-line, the walk and the log
after the resume). Every check carries a label "S<case>.<what>". A rig that passes proves little until it is seen to FAIL on a source that
breaks the rule it pins -- and here the rules include "the thread that was stopped is always resumed", so a mutation that skips a resume
leaves a thread suspended inside the rig: the rig must say so, and the tool must not hang on it. For each mutation below this tool copies the
two sources into a temp directory OUTSIDE the repo, applies one textual edit (or a few that belong together), compiles the rig against the
copy of the header (when the header changed), runs it with that copy as the repository root, and requires it to fail on a check of the case
that belongs to the rule (a FAIL label starting with one of the mutation's case ids). Nothing is written inside the repo.

  python tools\\stall_sampler_test\\mutants.py --self-test       text only: every anchor is found exactly once in the file it edits as it is
                                                               now, every case named is in the rig, and build.bat builds the rig the way
                                                               this tool does (add --build-bat PATH to check another copy)
  python tools\\stall_sampler_test\\mutants.py --run [--only a,b] [--jobs N] [--keep]
  python tools\\stall_sampler_test\\mutants.py --list
  python tools\\stall_sampler_test\\mutants.py --run --dry-run   the plan; writes nothing, starts nothing

--run needs the MSVC toolchain. The rig holds wall-clock intervals against real threads (about six seconds a run), so --jobs defaults to 1:
a mutation run beside another would be judged by a machine that is not quiet. --self-test runs in build.bat's rig and keeps an edit of a source
from silently orphaning a mutation.
"""
import argparse
import concurrent.futures
import io
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
FILES = {
    "hdr": ROOT / "src" / "common" / "stall_sampler.h",
    "glue": ROOT / "src" / "d3d11" / "stall_watch.cpp",
}
RIG = HERE / "stall_sampler_test.cpp"
DLL = HERE / "stall_target_dll.cpp"
BUILD_BAT = ROOT / "build.bat"
RIG_LABEL = ":rig_stall_sampler_test"
CL_FLAGS = ["/nologo", "/O2", "/MT", "/std:c++17", "/EHsc", "/W4", "/DWIN32_LEAN_AND_MEAN", "/DNOMINMAX", "/D_CRT_SECURE_NO_WARNINGS"]
RUN_TIMEOUT = 90.0


class Mutant:
    def __init__(self, name, caught, file, edits, why):
        self.name = name
        self.caught = tuple([caught] if isinstance(caught, str) else caught)   # case ids whose checks must report it
        self.file = file                                                        # a key of FILES
        self.edits = list(edits)                                                # (old, new) pairs, applied in order
        self.why = why


def M(name, caught, file, edits, why):
    return Mutant(name, caught, file, edits, why)


# ---- the anchors: text of the production sources, verbatim (the self-test finds each exactly once) ------------------------------
AT = "constexpr uint32_t kSampleAtMs[kSamplesPerEpisode] = {150, 500, 1000};"
BURST = "double burst = 6.0;"
REFILL = "double refillMs = 2000.0;"
CAP = "uint32_t sessionEpisodes = 200;"
SKIP_STALE = "        while (taken_ + 1 < kSamplesPerEpisode && ageMs >= static_cast<double>(cfg_.at[taken_ + 1])) ++taken_;\n"
ALLOWED = "bool allowed = started_;"
LATE = "d.lateStart = index > 0;"
SKIPPED = "                        skippedCounted_ = true;\n"
NEW_BEAT = "            taken_ = 0;\n            started_ = false;\n"
RESUME = "    const DWORD back = ResumeThread(thread);\n"
BOUNDS = "        if (from >= stackLo && from + sizeof(uint64_t) <= stackHi) {\n"
REBASE_FIRST = "    detail::rebaseAll(c, from, to, copy);\n    while (n < max) {\n"
REBASE_RBP = "    rebase(c.Rbp, from, to, copy);\n"
REBASE_STEP = "        detail::rebaseAll(c, from, to, copy);\n        // A frame larger than what is left of the copy"
OWNER = "        if (!ownerFound && !r.at[i].system) {"
SYSTEM_LOOP = "        if (equalsNoCase(name, n)) return true;\n"
RVA = "r.rva = static_cast<uint32_t>(pc - reinterpret_cast<uintptr_t>(module));"
EDVR = "r.edvr = module == thisModule();"
OWNER_TEXT = 'appendText(buf, cap, len, "; owner ");'
EDVR_NO = 'appendText(buf, cap, len, "; EDVR code on the stack: no.");'
SUSPEND_NAME = 'case CaptureStatus::SuspendFailed: return "suspend_failed";'
RANGE_NOTE = '        appendText(buf, cap, len, " (the stack pointer was outside the thread\'s stack: one frame only)");\n'
REGISTER = "        if (tid != registeredTid_.load(std::memory_order_relaxed)) registerThisThread(tid);\n"
SINK = "        try {\n            if (sink_) sink_(r, user_);\n        } catch (...) {\n        }\n"
CAPTURE_CALL = "            captureThread(s.handle, s.lo, s.hi, capture_);   // the ONLY stopped window\n"
NAME_CALL = "            nameCapture(capture_, r);                         // resumed by now: walk and name\n"
RIGHTS = "THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION, FALSE, 0))"
STOP = "    void stop() noexcept {\n"
GETCTX = "    const BOOL haveContext = GetThreadContext(thread, &out.context);\n"
GLUE_LOG = '    Log::get().note("%s", line);\n'
GLUE_START = "void startOnce() {\n"

MUTANTS = [
    # ---- S1: the policy --------------------------------------------------------------------------------------------------------
    M("first-sample-at-200", "S1", "hdr", [(AT, "constexpr uint32_t kSampleAtMs[kSamplesPerEpisode] = {200, 500, 1000};")], "the first sample waits for 200 ms"),
    M("burst-of-seven", "S1", "hdr", [(BURST, "double burst = 7.0;")], "seven stalls may start at once"),
    M("refill-every-second", "S1", "hdr", [(REFILL, "double refillMs = 1000.0;")], "a stall may start every second"),
    M("session-cap-300", "S1", "hdr", [(CAP, "uint32_t sessionEpisodes = 300;")], "three hundred stalls a session"),
    M("late-look-takes-every-sample", "S1", "hdr", [(SKIP_STALE, "")], "a look that comes late takes the three samples it missed, one after another"),
    M("later-samples-need-a-token", "S1", "hdr", [(ALLOWED, "bool allowed = false;")], "an episode that has started gets no second sample"),
    M("late-start-not-flagged", "S1", "hdr", [(LATE, "d.lateStart = false;")], "a stall that starts late is not marked as having started late"),
    M("refused-counted-every-threshold", "S1", "hdr", [(SKIPPED, "")], "a refused stall is counted once per threshold, not once"),
    M("new-beat-keeps-the-episode", "S1", "hdr", [(NEW_BEAT, "            started_ = false;\n")], "a new Present does not start a new episode"),
    # ---- S4: the resume, and what is stopped -----------------------------------------------------------------------------------
    M("no-resume-without-registers", ("S4", "S7"), "hdr", [(RESUME, "    const DWORD back = haveContext ? ResumeThread(thread) : 0;\n")],
      "a thread whose registers could not be read stays stopped"),
    M("no-resume-without-a-copy", ("S4", "S7"), "hdr", [(RESUME, "    const DWORD back = copied ? ResumeThread(thread) : 0;\n")],
      "a thread whose stack pointer was outside the stack stays stopped"),
    M("no-stack-bounds-check", "S4", "hdr", [(BOUNDS, "        if (true) {\n")], "the copy does not check the stack pointer against the thread's stack"),
    M("early-return-in-the-window", ("S4", "S7"), "hdr", [(GETCTX, GETCTX + "    if (!haveContext) return false;\n")], "a failed register read returns while the thread is stopped"),
    M("work-in-the-window", "S7", "hdr", [(GETCTX, GETCTX + "    Sleep(0);\n")], "a call to Sleep between the stop and the resume"),
    # ---- S3, S5: capture, walk and naming --------------------------------------------------------------------------------------
    M("walk-without-the-copy", ("S3", "S5"), "hdr", [(REBASE_FIRST, "    while (n < max) {\n")], "the walk reads the live stack, not the copy"),
    M("walk-leaves-rbp-alone", ("S3", "S5"), "hdr", [(REBASE_RBP, "")], "a saved frame pointer is not rebased into the copy"),
    M("walk-no-rebase-after-a-step", ("S3", "S5"), "hdr", [(REBASE_STEP, "        // A frame larger than what is left of the copy")], "registers popped by an unwind step are not rebased"),
    M("owner-is-the-top-frame", "S3", "hdr", [(OWNER, "        if (!ownerFound) {")], "the owner is always the innermost frame, an OS wait included"),
    M("no-module-is-an-os-module", "S3", "hdr", [(SYSTEM_LOOP, "        if (equalsNoCase(name, n)) return false;\n")], "no image is an OS wait image"),
    M("rva-is-the-address", "S3", "hdr", [(RVA, "r.rva = static_cast<uint32_t>(pc);")], "the address is printed whole, not as an offset into its module"),
    M("edvr-never-flagged", "S3", "hdr", [(EDVR, "r.edvr = false;")], "EDVR's own code is never named as such"),
    # ---- S2: the line ----------------------------------------------------------------------------------------------------------
    M("line-says-owned", "S2", "hdr", [(OWNER_TEXT, 'appendText(buf, cap, len, "; owned ");')], "the owner clause is worded differently"),
    M("line-says-edvr-yes", "S2", "hdr", [(EDVR_NO, 'appendText(buf, cap, len, "; EDVR code on the stack: yes.");')], "a stack with no EDVR code says it has some"),
    M("failed-reason-renamed", "S2", "hdr", [(SUSPEND_NAME, 'case CaptureStatus::SuspendFailed: return "suspend-failed";')], "a failed suspend has another name"),
    M("range-note-dropped", "S2", "hdr", [(RANGE_NOTE, "")], "a stack pointer outside the stack is not said"),
    # ---- S6, S8: the watchdog --------------------------------------------------------------------------------------------------
    M("beat-never-registers", "S6", "hdr", [(REGISTER, "")], "the render thread never registers its handle"),
    M("sink-never-called", "S6", "hdr", [(SINK, "")], "a sample is taken and nobody is told"),
    M("walk-before-the-stop", ("S8", "S6"), "hdr", [(CAPTURE_CALL, ""), (NAME_CALL, "            nameCapture(capture_, r);\n            captureThread(s.handle, s.lo, s.hi, capture_);\n")],
      "the walk is made before the thread is stopped"),
    M("glue-no-log", "S8", "glue", [(GLUE_LOG, "")], "the glue's sink does not write the log"),
    # ---- S7: the shape ---------------------------------------------------------------------------------------------------------
    M("rights-too-wide", "S7", "hdr", [(RIGHTS, "THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION, FALSE, 0))")],
      "the watched thread's handle may also set its context"),
    M("header-opens-a-thread", "S7", "hdr", [(STOP, STOP + "        OpenThread(THREAD_ALL_ACCESS, FALSE, 0);\n")], "the sampler opens a thread by id"),
    M("glue-opens-a-thread", "S7", "glue", [(GLUE_START, GLUE_START + "    OpenThread(THREAD_ALL_ACCESS, FALSE, 0);\n")], "the glue opens a thread by id"),
]


# ---- applying an edit ----------------------------------------------------------------------------------------------------
def apply_edits(text, edits, name="?"):
    """The text with each (old, new) applied in order; each `old` must occur exactly once in the text as it stands then."""
    for old, new in edits:
        count = text.count(old)
        if count != 1:
            raise ValueError("mutation %s: an anchor occurs %d times (want 1): %r" % (name, count, old[:90]))
        if old == new:
            raise ValueError("mutation %s: an edit changes nothing: %r" % (name, old[:90]))
        text = text.replace(old, new)
    return text


def read_source(path):
    return path.read_bytes().decode("utf-8").replace("\r\n", "\n")


def fail_labels(output):
    """Every check label the rig printed on a 'FAIL: S<case>.<what>' line (the text up to ' -- ', ' [' or the end of the line); the rig's
    closing 'FAIL: stall sampler: n of m checks failed' line is not a label."""
    labels = []
    for line in output.splitlines():
        if line.startswith("FAIL: "):
            label = re.split(r" -- | \[", line[len("FAIL: "):], 1)[0].strip()
            if re.match(r"S\d+\.", label):
                labels.append(label)
    return labels


def rig_cases(rig_text):
    """The case ids the rig's checks are labelled with."""
    return set(re.findall(r'"(S\d+)[.:]', rig_text))


def label_block(bat_text, label):
    """The lines of one build.bat rig, continuation lines (a trailing ^) joined, from its :rig_ label to the next :rig_ label; None when the
    label is absent. (A rig written in two steps has a label of its own inside, which does not end it.)"""
    lines = bat_text.replace("\r\n", "\n").split("\n")
    start = next((i for i, l in enumerate(lines) if l.strip().lower() == label.lower() or l.lower().startswith(label.lower() + " ")), None)
    if start is None:
        return None
    out, joined = [], ""
    for line in lines[start + 1:]:
        if line.lower().startswith(":rig_"):
            break
        if line.rstrip().endswith("^"):
            joined += line.rstrip()[:-1] + " "
            continue
        out.append(joined + line)
        joined = ""
    return out


# ---- the toolchain -----------------------------------------------------------------------------------------------------------
class Toolchain:
    """cl.exe by absolute path, with the environment it needs: this one if cl is on PATH, else the one vcvars64.bat makes."""

    def __init__(self):
        found = shutil.which("cl.exe")
        if found:
            self.env = os.environ.copy()
        else:
            vswhere = Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")) / "Microsoft Visual Studio" / "Installer" / "vswhere.exe"
            vs = subprocess.run([str(vswhere), "-latest", "-products", "*", "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
                                 "-property", "installationPath"], capture_output=True, text=True).stdout.strip()
            bat = Path(vs) / "VC" / "Auxiliary" / "Build" / "vcvars64.bat"
            if not vs or not bat.is_file():
                raise RuntimeError("cl.exe is not on PATH and no Visual Studio with the x64 C++ tools was found")
            dump = subprocess.run('cmd.exe /s /c ""%s" >nul && set"' % bat, capture_output=True, text=True).stdout
            self.env = {}
            for line in dump.splitlines():
                if "=" in line:
                    key, value = line.split("=", 1)
                    self.env[key] = value
            path = next((v for k, v in self.env.items() if k.upper() == "PATH"), "")
            found = shutil.which("cl.exe", path=path)
            if not found:
                raise RuntimeError("vcvars64.bat did not put cl.exe on PATH")
        self.cl = str(found)


def build_dll(tc, outdir):
    cmd = [tc.cl, "/nologo", "/O2", "/MT", "/LD", "/W4", "/DWIN32_LEAN_AND_MEAN", "/DNOMINMAX", "/D_CRT_SECURE_NO_WARNINGS",
           "/Fo" + str(outdir) + "\\", "/Fe" + str(outdir / "stall_target.dll"), str(DLL), "/link", "/INCREMENTAL:NO"]
    done = subprocess.run(cmd, capture_output=True, text=True, env=tc.env, errors="replace", cwd=str(outdir))
    return done.returncode, done.stdout + done.stderr


def build_rig(tc, include_dir, outdir):
    """Compile the rig against the header found in include_dir; (exit code, output)."""
    cmd = [tc.cl] + CL_FLAGS + ["/I" + str(include_dir), "/Fo" + str(outdir) + "\\", "/Fe" + str(outdir / "stall_sampler_test.exe"), str(RIG),
                                 "/link", "/INCREMENTAL:NO"]
    done = subprocess.run(cmd, capture_output=True, text=True, env=tc.env, errors="replace", cwd=str(outdir))
    return done.returncode, done.stdout + done.stderr


def run_rig(exe, root, tc):
    """(outcome, labels, tail) of one run of the rig against the repository root `root`: 'pass', 'fail', 'crash', 'timeout'."""
    try:
        done = subprocess.run([str(exe), "--self-test", str(root)], capture_output=True, text=True, env=tc.env, errors="replace", timeout=RUN_TIMEOUT)
    except subprocess.TimeoutExpired:
        return "timeout", [], "no result within %d s" % RUN_TIMEOUT
    out = done.stdout + "\n" + done.stderr
    if done.returncode == 0:
        return "pass", [], ""
    labels = fail_labels(out)
    if not labels:
        return "crash", [], "exit 0x%08X" % (done.returncode & 0xFFFFFFFF)
    return "fail", labels, ""


def make_root(dest, mutated):
    """A repository root holding the two sources the rig reads as text and includes, `mutated` replacing the text of one of them."""
    for key, path in FILES.items():
        target = dest / path.relative_to(ROOT)
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(mutated.get(key, read_source(path)), encoding="utf-8", newline="\n")


# ---- the run -----------------------------------------------------------------------------------------------------------------------
def select(only):
    if not only:
        return MUTANTS
    wanted = [n for n in only.split(",") if n]
    unknown = [n for n in wanted if n not in {m.name for m in MUTANTS}]
    if unknown:
        raise ValueError("no such mutation: " + ", ".join(unknown))
    return [m for m in MUTANTS if m.name in wanted]


def plan_text(mutants):
    lines = ["%d mutation(s); each runs the rig against an edited copy of one source in a temp directory outside the repo and must fail on a check of the cases named:" % len(mutants)]
    for m in mutants:
        lines.append("  %-34s %-5s caught by %-7s %s" % (m.name, m.file, "/".join(m.caught), m.why))
    lines.append("cl.exe flags: " + " ".join(CL_FLAGS))
    return "\n".join(lines)


def run_all(only=None, jobs=1, keep=False, dry_run=False, verbose=False, out=sys.stdout):
    mutants = select(only)
    if dry_run:
        print(plan_text(mutants), file=out)
        print("dry-run: nothing compiled, started or written", file=out)
        return 0
    tc = Toolchain()
    sources = {k: read_source(p) for k, p in FILES.items()}
    work = Path(tempfile.mkdtemp(prefix="ssm_"))
    try:
        # The control: the rig built from the unmutated header, run against the real repository, every case.
        control = work / "k"
        control.mkdir()
        code, text = build_dll(tc, control)
        if code != 0:
            print("control: nocompile stall_target.dll\n" + text.strip()[-1500:], file=out)
            return 1
        code, text = build_rig(tc, ROOT / "src" / "common", control)
        if code != 0:
            print("control: nocompile\n" + text.strip()[-1500:], file=out)
            return 1
        outcome, labels, tail = run_rig(control / "stall_sampler_test.exe", ROOT, tc)
        print("control (the unmutated sources, every case): %s %s" % (outcome, tail or " ".join(labels[:3])), file=out)
        if outcome != "pass":
            print("the rig does not pass on the unmutated sources when built this way; nothing below means anything", file=out)
            return 1

        def one(index, m):
            try:
                mutated = apply_edits(sources[m.file], m.edits, m.name)
            except ValueError as error:
                return m, "badedit", str(error)
            d = work / ("m%02d" % index)
            d.mkdir()
            root = d / "r"
            make_root(root, {m.file: mutated})
            if m.file == "hdr":
                mdir = d / "x"
                mdir.mkdir()
                shutil.copyfile(control / "stall_target.dll", mdir / "stall_target.dll")
                code, text = build_rig(tc, root / "src" / "common", mdir)
                if code != 0:
                    return m, "nocompile", (text.strip().splitlines() or [""])[-1]
                outcome, labels, tail = run_rig(mdir / "stall_sampler_test.exe", root, tc)
            else:
                outcome, labels, tail = run_rig(control / "stall_sampler_test.exe", root, tc)
            if outcome == "fail":
                if any(l.startswith(c + ".") for l in labels for c in m.caught):
                    if verbose:
                        return m, "caught", "%d failing label(s): %s" % (len(labels), ", ".join(labels))
                    return m, "caught", "%d failing label(s), first %s" % (len(labels), labels[0])
                return m, "OTHER", "failed on %s only" % ", ".join(sorted({l.split(".")[0] for l in labels}))
            return m, {"pass": "SURVIVED", "crash": "CRASH", "timeout": "TIMEOUT"}[outcome], tail

        results = []
        with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, jobs)) as pool:
            for m, verdict, detail in pool.map(lambda im: one(*im), list(enumerate(mutants))):
                results.append((m, verdict, detail))
                print("%-9s %-34s %-7s %s" % (verdict, m.name, "/".join(m.caught), detail if verbose else detail[:150]), file=out)
                out.flush()
        bad = [r for r in results if r[1] != "caught"]
        print("%d mutation(s): %d caught by their own case, %d not" % (len(results), len(results) - len(bad), len(bad)), file=out)
        for m, verdict, detail in bad:
            print("  NOT CAUGHT: %s (%s): wanted %s, got %s %s" % (m.name, m.why, "/".join(m.caught), verdict, detail[:120]), file=out)
        return 0 if not bad else 1
    finally:
        if keep:
            print("kept: %s" % work, file=out)
        else:
            shutil.rmtree(work, ignore_errors=True)


# ---- the self-test ------------------------------------------------------------------------------------------------------------------
def self_test(build_bat=BUILD_BAT):
    failures = []

    def check(ok, what):
        if not ok:
            failures.append(what)

    # the tool's own pieces
    check(fail_labels("a\nFAIL: S4.many -- x [y]\nFAIL: S2.sub\nFAIL: stall sampler: 3 of 9\n") == ["S4.many", "S2.sub"],
          "fail_labels reads the label of every check's FAIL: line, and not the closing summary")
    check(fail_labels("PASS: 12 stall sampler checks\n") == [], "fail_labels reads nothing from a pass")
    check(apply_edits("a b c", [("b", "B")]) == "a B c", "apply_edits applies one edit")
    for bad, why in ((("a b b", [("b", "B")]), "a repeated anchor"), (("a b", [("x", "y")]), "a missing anchor"), (("a b", [("b", "b")]), "a no-op edit")):
        try:
            apply_edits(*bad)
            check(False, "apply_edits refuses " + why)
        except ValueError:
            pass
    joined = label_block(":rig_a\nx ^\n y\n:a_run\nexit /b 0\n:rig_b\nz\n", ":rig_a")
    check(joined is not None and [" ".join(l.split()) for l in joined] == ["x y", ":a_run", "exit /b 0"],
          "label_block joins continuations, runs through a rig's own run label and stops at the next :rig_ label")
    check(rig_cases('check(x, "S1.a: b"); check(y, "S10.c")') == {"S1", "S10"}, "rig_cases reads the case ids off the labels")

    # every mutation against the sources as they are now, and against the rig
    sources = {k: read_source(p) for k, p in FILES.items()}
    rig = read_source(RIG)
    cases = rig_cases(rig)
    names = [m.name for m in MUTANTS]
    check(len(names) == len(set(names)), "mutation names are unique")
    check(len(MUTANTS) >= 25, "the mutation list did not shrink below 25 (%d)" % len(MUTANTS))
    for m in MUTANTS:
        check(m.file in FILES, "%s edits %s, which this tool does not know" % (m.name, m.file))
        if m.file in FILES:
            try:
                mutated = apply_edits(sources[m.file], m.edits, m.name)
                check(mutated != sources[m.file], "%s changes the source" % m.name)
            except ValueError as error:
                failures.append(str(error))
        for c in m.caught:
            check(c in cases, "%s: the rig has no case %s" % (m.name, c))
    covered = {c for m in MUTANTS for c in m.caught}
    check(covered >= cases - {"S9"}, "every case of the rig but the quiet-thread one (S9, which a mutation cannot make louder) has a mutation: %s vs %s" % (sorted(covered), sorted(cases)))
    check(DLL.is_file(), "the rig's second DLL source exists")

    # build.bat builds the rig the way this tool does, runs it alone, and runs this tool's self-test
    bat = Path(build_bat).read_bytes().decode("utf-8", errors="replace")
    block = label_block(bat, RIG_LABEL)
    check(block is not None, "build.bat has %s" % RIG_LABEL)
    if block:
        text = "\n".join(block)
        cls = [l for l in block if l.strip().lower().startswith("cl.exe")]
        check(len(cls) == 2, "build.bat's rig has two compiles, the DLL and the exe (%d)" % len(cls))
        exe_cl = next((l for l in cls if "stall_sampler_test.cpp" in l), "")
        dll_cl = next((l for l in cls if "stall_target_dll.cpp" in l), "")
        for flag in CL_FLAGS:
            check(flag in exe_cl, "build.bat's rig compile has %s" % flag)
        check('/I"src\\common"' in exe_cl, "build.bat's rig finds the header through /I src\\common (the mutated copy is found the same way)")
        check("/LD" in dll_cl and "stall_target.dll" in dll_cl, "build.bat builds stall_target.dll with /LD next to the exe")
        check('stall_sampler_test.exe" --dry-run' in text and 'stall_sampler_test.exe" --self-test "%ROOT%"' in text,
              "build.bat runs the rig's --dry-run and --self-test with the repository root (the S7 and S8 pins read the sources from it)")
        check('mutants.py" --self-test' in text, "build.bat runs this tool's self-test")
        check('if "%EDVR_RIG_STEP%"=="build" exit /b 0' in text and "goto stall_sampler_test_run" in text,
              "build.bat splits the rig into a compile step and a run step, so the run can be held for a quiet machine")
    check("stall_sampler_test" in "".join(l for l in bat.replace("\r\n", "\n").split("\n") if "--quiet" in l),
          "build.bat names the rig in run_jobs.py's --quiet list: it holds wall-clock intervals and must run alone")

    # --dry-run starts nothing and writes nothing
    calls = []
    real_mkdtemp, real_run = tempfile.mkdtemp, subprocess.run
    tempfile.mkdtemp = lambda *a, **k: calls.append("mkdtemp") or real_mkdtemp(*a, **k)
    subprocess.run = lambda *a, **k: calls.append("subprocess") or real_run(*a, **k)
    try:
        sink = io.StringIO()
        code = run_all(dry_run=True, out=sink)
    finally:
        tempfile.mkdtemp, subprocess.run = real_mkdtemp, real_run
    check(code == 0 and not calls and "dry-run" in sink.getvalue(), "--dry-run starts no process and makes no directory (saw %s)" % calls)
    try:
        select("no-such-mutation")
        check(False, "--only names an unknown mutation")
    except ValueError:
        pass

    if failures:
        for f in failures:
            print("FAIL: " + f, file=sys.stderr)
        return 1
    print("PASS: stall_sampler_test mutants.py self-test (%d mutations over %d cases, every anchor found once, build.bat wired)" % (len(MUTANTS), len(covered)))
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--run", action="store_true")
    parser.add_argument("--list", action="store_true")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--only", default="")
    parser.add_argument("--jobs", type=int, default=1)
    parser.add_argument("--keep", action="store_true", help="leave the temp directory (printed) for a look at a mutant")
    parser.add_argument("--verbose", action="store_true", help="print every label a mutant made the rig fail on, not just the first")
    parser.add_argument("--build-bat", default=str(BUILD_BAT), help="the build.bat the self-test reads the rig's label from")
    args = parser.parse_args(argv)
    if args.self_test:
        return self_test(args.build_bat)
    if args.list:
        print(plan_text(MUTANTS))
        return 0
    if args.run:
        try:
            return run_all(only=args.only, jobs=args.jobs, keep=args.keep, dry_run=args.dry_run, verbose=args.verbose)
        except (ValueError, RuntimeError) as error:
            print("error: %s" % error, file=sys.stderr)
            return 2
    parser.print_help()
    return 2


if __name__ == "__main__":
    sys.exit(main())
