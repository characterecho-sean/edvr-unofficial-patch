#!/usr/bin/env python3
"""The mutation proof for tools\\freeze_log_test: the rig fails when a rule of the freeze logging is flipped.

The rig (freeze_log_test.cpp) holds the rules both halves of EDVR share for logging long frames (src\\common\\freeze_book.h), the lines the
runtime writes (src\\openxr\\long_cycle_line.h) and, by source text, the glue that calls them (perf_monitor.cpp, native_runtime_host.h,
native_timing.cpp). Every check carries a label "F<case>.<what>". A rig that passes proves little until it is seen to FAIL on a source that
breaks the rule it pins. This tool does that: for each mutation below it copies the source it edits into a temp directory OUTSIDE the repo,
applies one textual edit (or a few that belong together), and runs the rig against that copy, requiring it to fail on a check of the case that
belongs to the rule (a FAIL label starting with one of the mutation's case ids). A mutated HEADER is compiled into a fresh rig; a mutated
GLUE source (the F9 pins) is run through the unmutated rig, which is handed a temp repository root holding the edited copy. Nothing is written
inside the repo; the temp directory is removed at the end.

  python tools\\freeze_log_test\\mutants.py --self-test       text only: every anchor is found exactly once in the file it edits as it is now,
                                                            every case named is in the rig, and build.bat compiles the rig the way this tool
                                                            does (add --build-bat PATH to check another copy)
  python tools\\freeze_log_test\\mutants.py --run [--only a,b] [--jobs N] [--keep]
  python tools\\freeze_log_test\\mutants.py --list
  python tools\\freeze_log_test\\mutants.py --run --dry-run   the plan; writes nothing, starts nothing

--run needs the MSVC toolchain (cl.exe on PATH, or Visual Studio found with vswhere as build.bat does). --self-test runs in build.bat's rig and
is what keeps an edit of a source from silently orphaning a mutation: if an anchor stops matching, the build fails and this file says which.
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
    "book": ROOT / "src" / "common" / "freeze_book.h",
    "line": ROOT / "src" / "openxr" / "long_cycle_line.h",
    "perf": ROOT / "src" / "d3d11" / "perf_monitor.cpp",
    "host": ROOT / "src" / "openxr" / "native_runtime_host.h",
    "timing": ROOT / "src" / "d3d11" / "native_timing.cpp",
}
HEADER_KEYS = ("book", "line")        # compiled into the rig: a mutation of one rebuilds the rig
PIN_KEYS = ("perf", "host", "timing")  # read by the rig as text: a mutation of one is run through the unmutated rig
# Everything the rig's two headers include, laid out as the headers expect to find each other (src\common beside src\openxr).
TREE = [
    ("src/common/freeze_book.h", ROOT / "src" / "common" / "freeze_book.h"),
    ("src/common/native_present_trace.h", ROOT / "src" / "common" / "native_present_trace.h"),
    ("src/common/native_cpu_trace_events.h", ROOT / "src" / "common" / "native_cpu_trace_events.h"),
    ("src/openxr/long_cycle_line.h", ROOT / "src" / "openxr" / "long_cycle_line.h"),
    ("src/openxr/frame_cycle_stats.h", ROOT / "src" / "openxr" / "frame_cycle_stats.h"),
]
RIG = HERE / "freeze_log_test.cpp"
BUILD_BAT = ROOT / "build.bat"
RIG_LABEL = ":rig_freeze_log_test"
CL_FLAGS = ["/nologo", "/O2", "/MT", "/std:c++17", "/EHsc", "/W4", "/DWIN32_LEAN_AND_MEAN", "/DNOMINMAX", "/D_CRT_SECURE_NO_WARNINGS"]
RUN_TIMEOUT = 120.0


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
ALWAYS = "constexpr double kFreezeAlwaysLogMs = 250.0;\n"
EVERY = "constexpr uint64_t kFreezeCountsEveryMs = 5ull * 60ull * 1000ull;\n"
B50 = "    if (ms < 50.0) return kFzUnder50;\n"
B250 = "    if (ms < 250.0) return kFz100To250;\n"
J_GAP = "    if (!(budgetMs > 0.0) || !(gapMs > 2.0 * budgetMs)) return FreezeVerdict::NotLong;\n"
J_FREEZE = "    if (gapMs >= kFreezeAlwaysLogMs) return FreezeVerdict::Freeze;\n"
J_CYCLE = "    if (cycleKnown && !(cycleMs > 2.0 * budgetMs)) return FreezeVerdict::Blip;\n"
W_RULE = "    return v == FreezeVerdict::Freeze || (v == FreezeVerdict::Long && limiterAllows);\n"
REC_BLIP = "        if (o.verdict == FreezeVerdict::Blip) noteBlip(ms);\n"
FZ_UNWRITTEN = "            if (!lineWritten) ++freezesUnwritten;\n"
BLIP_COUNT = "        ++candidates;\n        ++blips;\n"
UNWR_BUCKET = "            ++unwrittenBucket[b];\n"
K_WORST = "    static constexpr unsigned kWorst = 5;\n"
W_TIE = "            if (w.ms > worst_[i].ms) { at = i; break; }\n"
W_REV = "        ++worstRevision;\n"
W_KEEP = "        return ms > worst_[kWorst - 1].ms;\n"
COPY_END = "    dst[n] = 0;\n"
UNWR_LOOP = "        for (unsigned b = 0; b < kFz250To1000; ++b) {\n"
LIM_SEC = "    unsigned perSecond = 4;\n"
LIM_SESSION = "    uint64_t perSession = 400;\n"
LIM_RESET = "            window = 0;\n"
LIM_CHARGE = "        ++window;\n        ++charged;\n"
L_HEAD = '  longCycleAppend(buf,cap,len,"native_long_cycle,");\n'
L_WORST = '"native_long_cycle_worst,rank=%u,of=%u,utc=%s,%s"'
L_SUMMARY = '"%s,count=%llu,logged=%llu,threshold=2x_period,%s"'
P_CALL = "        judgeLongFrame(*ringLast(), budget, gapMs, q);\n"
P_GATE = "    if (budget > 0.0f && gapMs > 2.0 * static_cast<double>(budget)) {\n"
P_FREEZE = "    if (freeze) freezeLine(gapMs, cv, frameNo, freezeNo"   # the call's tail differs between the commits that built it up
P_PERIODIC = '        writeFreezeSummary("periodic", false);\n'
P_OBSERVER = "    CloseObserverRegistration() { g_nativeTimingCloseObserver = &perfMonitorSessionEnd; }\n"
P_DUMP = '    vtableWatchDumpRecent("monitor: LONG FRAME", frameNo);\n'
H_SUMMARY = "    if(tracing)writeLongCycleSummary(nullptr,true);\n"
H_CHARGE = "    if(write&&!freeze)cycleLimiter.charge();\n"
H_RULE = "    const bool write=freezeWritesLine(freeze?FreezeVerdict::Freeze:FreezeVerdict::Long,limiterAllows);\n"
T_STAMP = "    c->lastReturnQpc = end;\n"
T_OBSERVER = "    if (edvr::g_nativeTimingCloseObserver) edvr::g_nativeTimingCloseObserver();\n"

MUTANTS = [
    # ---- F1: buckets and constants ---------------------------------------------------------------------------------------------
    M("bucket-50-inclusive", "F1", "book", [(B50, "    if (ms <= 50.0) return kFzUnder50;\n")], "exactly 50 ms is in the first bucket"),
    M("bucket-250-inclusive", "F1", "book", [(B250, "    if (ms <= 250.0) return kFz100To250;\n")], "exactly 250 ms is not yet in the always-written bucket"),
    M("always-log-300", ("F1", "F2"), "book", [(ALWAYS, "constexpr double kFreezeAlwaysLogMs = 300.0;\n")], "a freeze is 300 ms, not 250"),
    M("counts-every-ten-minutes", "F1", "book", [(EVERY, "constexpr uint64_t kFreezeCountsEveryMs = 10ull * 60ull * 1000ull;\n")], "the counts come every ten minutes"),
    # ---- F2: the judge ---------------------------------------------------------------------------------------------------------
    M("judge-boundary-inclusive", "F2", "book", [(J_GAP, "    if (!(budgetMs > 0.0) || gapMs < 2.0 * budgetMs) return FreezeVerdict::NotLong;\n")],
      "a gap of exactly twice the period is long"),
    M("judge-ignores-the-cycle", "F2", "book", [(J_CYCLE, "")], "every gap over twice the period is long: the field flight's 44 blips spend the cap again"),
    M("judge-cycle-boundary-inclusive", "F2", "book", [(J_CYCLE, "    if (cycleKnown && cycleMs < 2.0 * budgetMs) return FreezeVerdict::Blip;\n")],
      "a cycle of exactly twice the period is a blip"),
    M("judge-unknown-cycle-is-a-blip", "F2", "book", [(J_CYCLE, "    if (!(cycleMs > 2.0 * budgetMs)) return FreezeVerdict::Blip;\n")],
      "a gap whose cycle cannot be read is hidden instead of trusted"),
    M("freeze-needs-the-cycle", "F2", "book", [(J_FREEZE, "    if (gapMs >= kFreezeAlwaysLogMs && cycleKnown && cycleMs > 2.0 * budgetMs) return FreezeVerdict::Freeze;\n")],
      "a 250 ms gap whose cycle was short is not a freeze"),
    M("freeze-boundary-exclusive", "F2", "book", [(J_FREEZE, "    if (gapMs > kFreezeAlwaysLogMs) return FreezeVerdict::Freeze;\n")], "exactly 250 ms is not a freeze"),
    # ---- F3: the write rule ----------------------------------------------------------------------------------------------------
    M("freeze-asks-the-limiter", "F3", "book", [(W_RULE, "    return (v == FreezeVerdict::Freeze && limiterAllows) || (v == FreezeVerdict::Long && limiterAllows);\n")],
      "a freeze is written only when the rate limit allows: the 1858 ms freeze is lost again"),
    M("blips-are-written", "F3", "book", [(W_RULE, "    return v == FreezeVerdict::Freeze || (v != FreezeVerdict::NotLong && limiterAllows);\n")],
      "a blip is written when the limiter is open"),
    M("long-ignores-the-limiter", "F3", "book", [(W_RULE, "    return v == FreezeVerdict::Freeze || v == FreezeVerdict::Long;\n")],
      "every long frame is written, the limiter notwithstanding"),
    # ---- F4: the book's counts -------------------------------------------------------------------------------------------------
    M("record-counts-a-blip-as-long", "F4", "book", [(REC_BLIP, "        if (o.verdict == FreezeVerdict::Blip) noteLong(ms, o.write);\n")], "a blip is counted as a long frame"),
    M("freeze-unwritten-forgotten", "F4", "book", [(FZ_UNWRITTEN, "")], "a freeze that was not written does not show in over_250ms_unwritten"),
    M("blip-is-not-a-candidate", "F4", "book", [(BLIP_COUNT, "        ++blips;\n")], "a blip is not counted among the candidates"),
    M("unwritten-in-the-first-bucket", "F4", "book", [(UNWR_BUCKET, "            ++unwrittenBucket[kFzUnder50];\n")], "every unwritten long frame is counted as under 50 ms"),
    # ---- F5: the worst list ----------------------------------------------------------------------------------------------------
    M("worst-keeps-four", "F5", "book", [(K_WORST, "    static constexpr unsigned kWorst = 4;\n")], "the worst list holds four"),
    M("worst-later-equal-wins", "F5", "book", [(W_TIE, "            if (w.ms >= worst_[i].ms) { at = i; break; }\n")], "of two equal frames the later goes first"),
    M("worst-revision-never-moves", "F5", "book", [(W_REV, "")], "the worst list's revision never changes, so a changed list is never printed again"),
    M("worst-would-keep-equal", "F5", "book", [(W_KEEP, "        return ms >= worst_[kWorst - 1].ms;\n")], "a frame equal to the fifth is built and offered"),
    M("text-copy-not-terminated", "F5", "book", [(COPY_END, "")], "text copied into a field is not terminated"),
    # ---- F6: the counts text ---------------------------------------------------------------------------------------------------
    M("counts-list-every-unwritten-bucket", "F6", "book", [(UNWR_LOOP, "        for (unsigned b = 0; b < kFzBuckets; ++b) {\n")], "the counts name unwritten buckets that cannot be unwritten"),
    # ---- F7: the runtime's rate limit ------------------------------------------------------------------------------------------
    M("limiter-five-a-second", "F7", "book", [(LIM_SEC, "    unsigned perSecond = 5;\n")], "five lines a second"),
    M("limiter-five-hundred", "F7", "book", [(LIM_SESSION, "    uint64_t perSession = 500;\n")], "five hundred a session"),
    M("limiter-window-never-resets", "F7", "book", [(LIM_RESET, "")], "the next second does not open the limiter"),
    M("limiter-charge-is-free", "F7", "book", [(LIM_CHARGE, "")], "a line charged to the limiter costs it nothing"),
    # ---- F8: the runtime's lines -----------------------------------------------------------------------------------------------
    M("long-cycle-head-renamed", "F8", "line", [(L_HEAD, '  longCycleAppend(buf,cap,len,"native_long_cycles,");\n')], "the native_long_cycle line has another name"),
    M("worst-line-renamed", "F8", "line", [(L_WORST, '"native_long_cycle_worse,rank=%u,of=%u,utc=%s,%s"')], "the worst line has another name"),
    M("summary-head-reordered", "F8", "line", [(L_SUMMARY, '"%s,logged=%llu,count=%llu,threshold=2x_period,%s"')], "the summary's count and logged are swapped at its head"),
    # ---- F9: the glue ----------------------------------------------------------------------------------------------------------
    M("glue-judge-never-called", "F9", "perf", [(P_CALL, "        (void)q;\n")], "perfMonitorFrame never asks the judge"),
    M("glue-gap-clipped-at-5-s", "F9", "perf", [(P_GATE, "    if (budget > 0.0f && f.presentMs > 2.0f * budget) {\n")], "the judge is asked by the ring's figure, which is 0 for a gap of 5 s or more"),
    M("glue-no-freeze-line", "F9", "perf", [(P_FREEZE, "    if (false) freezeLine(gapMs, cv, frameNo, freezeNo")], "no FREEZE line is written"),
    M("glue-no-periodic-counts", "F9", "perf", [(P_PERIODIC, "        (void)0;\n")], "the counts are never written while the session runs"),
    M("glue-no-close-observer", "F9", "perf", [(P_OBSERVER, "    CloseObserverRegistration() {}\n")], "the runtime's close does not write the end-of-session lines"),
    M("glue-dump-before-the-judge", "F9", "perf", [(P_DUMP, "")], "no flip-timeline dump for a long frame"),
    M("glue-host-no-summary", "F9", "host", [(H_SUMMARY, "")], "the runtime's close writes no long-cycle summary"),
    M("glue-host-freeze-charges-the-limiter", "F9", "host", [(H_CHARGE, "    if(write)cycleLimiter.charge();\n")], "a freeze is charged to the limiter"),
    M("glue-host-own-write-rule", "F9", "host", [(H_RULE, "    const bool write=limiterAllows;\n")], "the runtime applies its own write rule, not the shared one"),
    M("glue-timing-no-stamp", "F9", "timing", [(T_STAMP, "")], "the wait returns are not stamped"),
    M("glue-timing-no-observer", "F9", "timing", [(T_OBSERVER, "")], "the timing close does not tell the monitor"),
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
    """Every check label the rig printed on a 'FAIL: F<case>.<what>' line (the text up to ' -- ', ' [' or the end of the line); the rig's
    closing 'FAIL: freeze log: n of m checks failed' line is not a label."""
    labels = []
    for line in output.splitlines():
        if line.startswith("FAIL: "):
            label = re.split(r" -- | \[", line[len("FAIL: "):], 1)[0].strip()
            if re.match(r"F\d+\.", label):
                labels.append(label)
    return labels


def rig_cases(rig_text):
    """The case ids the rig's checks are labelled with."""
    return set(re.findall(r'"(F\d+)[.:]', rig_text))


def label_block(bat_text, label):
    """The lines of one build.bat subroutine, continuation lines (a trailing ^) joined; None when the label is absent."""
    lines = bat_text.replace("\r\n", "\n").split("\n")
    start = next((i for i, l in enumerate(lines) if l.strip().lower() == label.lower() or l.lower().startswith(label.lower() + " ")), None)
    if start is None:
        return None
    out, joined = [], ""
    for line in lines[start + 1:]:
        if line.startswith(":") and not line.startswith("::"):
            break
        if line.rstrip().endswith("^"):
            joined += line.rstrip()[:-1] + " "
            continue
        out.append(joined + line)
        joined = ""
    return out


# ---- the toolchain -----------------------------------------------------------------------------------------------------------
class Toolchain:
    """cl.exe and link.exe by absolute path, with the environment they need: this one if cl is on PATH, else the one vcvars64.bat makes."""

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


def build_rig(tc, tree, exe):
    """Compile the rig against the header tree at `tree` (src\\common and src\\openxr inside it); (exit code, output)."""
    cmd = [tc.cl] + CL_FLAGS + ["/I" + str(tree / "src" / "common"), "/I" + str(tree / "src" / "openxr"),
                                 "/Fo" + str(exe.parent) + "\\", "/Fe" + str(exe), str(RIG), "/link", "/INCREMENTAL:NO"]
    done = subprocess.run(cmd, capture_output=True, text=True, env=tc.env, errors="replace", cwd=str(exe.parent))
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


def make_tree(dest, mutated=None):
    """The headers the rig includes, laid out under `dest`; `mutated` maps a FILES key to replacement text."""
    mutated = mutated or {}
    for rel, src in TREE:
        target = dest / rel
        target.parent.mkdir(parents=True, exist_ok=True)
        key = next((k for k in HEADER_KEYS if FILES[k] == src), None)
        if key and key in mutated:
            target.write_text(mutated[key], encoding="utf-8", newline="\n")
        else:
            shutil.copyfile(src, target)


def make_root(dest, mutated):
    """A repository root holding the three glue sources the rig reads as text, `mutated` replacing one of them."""
    for key in PIN_KEYS:
        target = dest / FILES[key].relative_to(ROOT)
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(mutated.get(key, read_source(FILES[key])), encoding="utf-8", newline="\n")


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
        lines.append("  %-40s %-6s caught by %-7s %s" % (m.name, m.file, "/".join(m.caught), m.why))
    lines.append("cl.exe flags: " + " ".join(CL_FLAGS))
    return "\n".join(lines)


def run_all(only=None, jobs=None, keep=False, dry_run=False, verbose=False, out=sys.stdout):
    mutants = select(only)
    if dry_run:
        print(plan_text(mutants), file=out)
        print("dry-run: nothing compiled, started or written", file=out)
        return 0
    tc = Toolchain()
    sources = {k: read_source(p) for k, p in FILES.items()}
    work = Path(tempfile.mkdtemp(prefix="flm_"))
    workers = jobs or min(8, os.cpu_count() or 2)
    try:
        # The control: the rig built from the unmutated headers, run against the real repository, every case.
        control = work / "k"
        make_tree(control)
        exe = control / "rig.exe"
        code, text = build_rig(tc, control, exe)
        if code != 0:
            print("control: nocompile\n" + text.strip()[-1500:], file=out)
            return 1
        outcome, labels, tail = run_rig(exe, ROOT, tc)
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
            if m.file in HEADER_KEYS:
                tree = d / "t"
                make_tree(tree, {m.file: mutated})
                mexe = d / "rig.exe"
                code, text = build_rig(tc, tree, mexe)
                if code != 0:
                    return m, "nocompile", (text.strip().splitlines() or [""])[-1]
                outcome, labels, tail = run_rig(mexe, ROOT, tc)
            else:
                root = d / "r"
                make_root(root, {m.file: mutated})
                outcome, labels, tail = run_rig(exe, root, tc)
            if outcome == "fail":
                if any(l.startswith(c + ".") for l in labels for c in m.caught):
                    if verbose:
                        return m, "caught", "%d failing label(s): %s" % (len(labels), ", ".join(labels))
                    return m, "caught", "%d failing label(s), first %s" % (len(labels), labels[0])
                return m, "OTHER", "failed on %s only" % ", ".join(sorted({l.split(".")[0] for l in labels}))
            return m, {"pass": "SURVIVED", "crash": "CRASH", "timeout": "TIMEOUT"}[outcome], tail

        results = []
        with concurrent.futures.ThreadPoolExecutor(max_workers=workers) as pool:
            for m, verdict, detail in pool.map(lambda im: one(*im), list(enumerate(mutants))):
                results.append((m, verdict, detail))
                print("%-9s %-40s %-7s %s" % (verdict, m.name, "/".join(m.caught), detail if verbose else detail[:150]), file=out)
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
    check(fail_labels("a\nFAIL: F4.flight-freeze -- x [y]\nFAIL: F2.sub\nFAIL: freeze log: 3 of 9\n") == ["F4.flight-freeze", "F2.sub"],
          "fail_labels reads the label of every check's FAIL: line, and not the closing summary")
    check(fail_labels("PASS: 12 freeze log checks\n") == [], "fail_labels reads nothing from a pass")
    check(apply_edits("a b c", [("b", "B")]) == "a B c", "apply_edits applies one edit")
    for bad, why in ((("a b b", [("b", "B")]), "a repeated anchor"), (("a b", [("x", "y")]), "a missing anchor"), (("a b", [("b", "b")]), "a no-op edit")):
        try:
            apply_edits(*bad)
            check(False, "apply_edits refuses " + why)
        except ValueError:
            pass
    joined = label_block(":rig_a\nx ^\n y\nexit /b 0\n:rig_b\nz\n", ":rig_a")
    check(joined is not None and [" ".join(l.split()) for l in joined] == ["x y", "exit /b 0"], "label_block joins continuations and stops at the next label")
    check(rig_cases('check(x, "F1.a: b"); check(y, "F10.c")') == {"F1", "F10"}, "rig_cases reads the case ids off the labels")

    # every mutation against the sources as they are now, and against the rig
    sources = {k: read_source(p) for k, p in FILES.items()}
    rig = read_source(RIG)
    cases = rig_cases(rig)
    names = [m.name for m in MUTANTS]
    check(len(names) == len(set(names)), "mutation names are unique")
    check(len(MUTANTS) >= 30, "the mutation list did not shrink below 30 (%d)" % len(MUTANTS))
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
    check(covered == cases, "every case of the rig has a mutation, and only cases of the rig: %s vs %s" % (sorted(covered), sorted(cases)))
    for rel, src in TREE:
        check(src.is_file(), "%s exists (the header tree the rig is built against)" % rel)

    # build.bat compiles the rig the way this tool does, and runs this tool's self-test
    bat = Path(build_bat).read_bytes().decode("utf-8", errors="replace")
    block = label_block(bat, RIG_LABEL)
    check(block is not None, "build.bat has %s" % RIG_LABEL)
    if block:
        text = "\n".join(block)
        cl = next((l for l in block if l.strip().lower().startswith("cl.exe")), "")
        for flag in CL_FLAGS:
            check(flag in cl, "build.bat's rig compile has %s" % flag)
        check('/I"src\\common"' in cl and '/I"src\\openxr"' in cl, "build.bat's rig finds the headers through /I src\\common and /I src\\openxr (the mutated tree is found the same way)")
        check("tools\\freeze_log_test\\freeze_log_test.cpp" in cl, "build.bat's rig compile has the rig's source")
        check('freeze_log_test.exe" --dry-run' in text and 'freeze_log_test.exe" --self-test "%ROOT%"' in text,
              "build.bat runs the rig's --dry-run and --self-test with the repository root (the F9 pins read the glue's sources from it)")
        check('mutants.py" --self-test' in text, "build.bat runs this tool's self-test")

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
    print("PASS: freeze_log_test mutants.py self-test (%d mutations over %d cases, every anchor found once, build.bat wired)" % (len(MUTANTS), len(covered)))
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--run", action="store_true")
    parser.add_argument("--list", action="store_true")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--only", default="")
    parser.add_argument("--jobs", type=int, default=0)
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
            return run_all(only=args.only, jobs=args.jobs or None, keep=args.keep, dry_run=args.dry_run, verbose=args.verbose)
        except (ValueError, RuntimeError) as error:
            print("error: %s" % error, file=sys.stderr)
            return 2
    parser.print_help()
    return 2


if __name__ == "__main__":
    sys.exit(main())
