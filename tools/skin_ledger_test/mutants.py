#!/usr/bin/env python3
"""The mutation proof for tools\\skin_ledger_test: the rig fails when a rule of the skin ledger is flipped.

The rig (skin_ledger_test.cpp) holds the skin ledger's rules (src\\d3d11\\skin_ledger.h: unarmed is silent and stateless, the 20-frame window, the copy
plan, the numbers, the report, the file) and, by source text, the glue that arms it, feeds it and reports it (exposure_fix.cpp, object_probe.cpp/.h,
build.bat). Every check carries a label "S<case>.<what>". A rig that passes proves little until it is seen to FAIL on a source that breaks the rule it
pins. This tool does that: for each mutation below it copies the source it edits into a temp directory OUTSIDE the repo, applies one textual edit (or a
few that belong together), and runs the rig against that copy, requiring it to fail on a check of the case that belongs to the rule (a FAIL label
starting with one of the mutation's case ids). A mutated HEADER is compiled into a fresh rig; a mutated GLUE source (the S12 pins) is run through the
unmutated rig, which is handed a temp repository root holding the edited copy. Nothing is written inside the repo; the temp directory is removed at
the end.

  python tools\\skin_ledger_test\\mutants.py --self-test       text only: every anchor is found exactly once in the file it edits as it is now,
                                                              every case named is in the rig, and build.bat compiles the rig the way this tool does
  python tools\\skin_ledger_test\\mutants.py --run [--only a,b] [--jobs N] [--keep]
  python tools\\skin_ledger_test\\mutants.py --list
  python tools\\skin_ledger_test\\mutants.py --run --dry-run   the plan; writes nothing, starts nothing

--run needs the MSVC toolchain (cl.exe on PATH, or Visual Studio found with vswhere as build.bat does). --self-test runs in build.bat's rig and is what
keeps an edit of a source from silently orphaning a mutation: if an anchor stops matching, the build fails and this file says which.
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
    "ledger": ROOT / "src" / "d3d11" / "skin_ledger.h",
    "exposure": ROOT / "src" / "d3d11" / "exposure_fix.cpp",
    "probe": ROOT / "src" / "d3d11" / "object_probe.cpp",
    "probeh": ROOT / "src" / "d3d11" / "object_probe.h",
    "bat": ROOT / "build.bat",
}
HEADER_KEYS = ("ledger",)                       # compiled into the rig: a mutation of one rebuilds the rig
PIN_KEYS = ("exposure", "probe", "probeh", "bat")   # read by the rig as text: a mutation of one is run through the unmutated rig
TREE = [("src/d3d11/skin_ledger.h", ROOT / "src" / "d3d11" / "skin_ledger.h")]
INCLUDE_DIRS = ("src/d3d11",)                   # the rig's /I directories under the header tree (build.bat's compile line has the same)
RIG = HERE / "skin_ledger_test.cpp"
BUILD_BAT = ROOT / "build.bat"
RIG_LABEL = ":rig_skin_ledger_test"
RIG_SOURCE_IN_BAT = "tools\\skin_ledger_test\\skin_ledger_test.cpp"
RIG_EXE_IN_BAT = "skin_ledger_test.exe"
CASE_PREFIX = "S"
CL_FLAGS = ["/nologo", "/O2", "/MT", "/std:c++17", "/EHsc", "/W4", "/DWIN32_LEAN_AND_MEAN", "/DNOMINMAX", "/D_CRT_SECURE_NO_WARNINGS"]
LINK_ARGS = []
MIN_MUTANTS = 36
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
A_WINDOW = "bool inWindow(uint32_t f) const { return armed_ && f >= frame0_ && f - frame0_ < kFrames; }\n"
A_KEEP = "constexpr uint32_t kKeepPaletteBytes = 65536u * kRowBytes;"
A_CHAIN = "constexpr uint64_t kChainHash = 0x6FE04AF836BB1DBAull;"
A_CLEARHASH = "constexpr uint64_t kClearHash = 0x7B2A531B72941109ull;"
A_REPORT_TOP = "        if (!armed_ || finished_) return;\n        uint32_t anyDispatch = 0,"
A_VERDICT = "        const char* verdict;\n        char why[256];\n"
A_JOINTS_SHARE = "if (fr.d[k].jointsFrom == k && fr.d[k].info.t2 == info.t2 && info.t2 != 0) d.jointsFrom = k;"
A_BIND_FIND = "if (binds_[k].id == info.t1) plan.bind = static_cast<int>(k);"
A_CAP = "if (fr.d.size() >= kMaxDispatches) {"
A_GAP = "if (jobs[k].first > end) ++s.gaps;"
A_NOTDST = "if (!std::binary_search(dsts.begin(), dsts.end(), b)) ++s.poolNotDst;"
A_ZERO = "                if (count == 0) continue;\n"
A_POOLKNOWN = "s.poolKnown ? \"\" : \"? \", s.poolSkinned,"
A_BROKEN = "} else if (lost || declined || skipped_ || jobsMissing || dataFrames < chainFrames || palFrames == 0) {"
A_DATAOK = "                if (d.jobsStaged && d.jobs.empty()) dataOk = false;\n"
A_NOPALETTE = "dataFrames < chainFrames || palFrames == 0) {"
A_OTHER = "} else if (chainSeen == 0) {"
A_HOW = "verdict, stamp.c_str(), how, why, chainFrames,"
A_FILESTATUS = "kKeepPaletteBytes / kRowBytes, notDst, gaps, overlaps, poolReleases_, foreign_, stamp.c_str(), fileStatus);"
A_FOREIGN_ARG = "poolReleases_, foreign_, stamp.c_str(), fileStatus);"
A_FOREIGN_RET = ("    if (foreign) {\n"
                 "        g_skinForeign.fetch_add(1, std::memory_order_relaxed);\n"
                 "        return;\n"
                 "    }\n")
A_MAGIC = "static const char kMagic[8] = {'E', 'D', 'V', 'R', 'S', 'K', 'N', '1'};"
A_HDR = "u32(1u); u32(kFrames); u32(frame0_); u32(paletteCount_);"
A_HOOK_FOREIGN = "        if (objectProbeLedgerActive()) objectProbeNoteDispatch(self, x, y, z, true);   // the skin ledger: one bool unarmed\n"
A_NOTE_HEAD = ("void objectProbeNoteDispatch(ID3D11DeviceContext* ctx, uint32_t x, uint32_t y, uint32_t z, bool foreign) {\n"
               "    if (!detail::g_objectProbeLedgerOn || !ctx) return;\n")
A_ARM_RESET = "    g_skin.reset();   // its report went out above; a run that never reported is said at shutdown\n"
A_WRITE_CALL = "    writeSkinLedger(dir, \"window closed\");\n"
A_POOL_SWAP = "        std::vector<uint8_t>().swap(g_ledgerPool[i]);\n"
A_WHOLE = "g_palette[i], g_paletteBytes[i], g_paletteBytes[i]);"
A_SHUTDOWN = "    if (g_skin.armed() && !g_skin.finished()) {\n        std::string stamp;"
A_KEEPREAD = "ledgerRead(ctx, c, into, what == 0 ? 0u : kLedgerBonesMax)"
A_RELEASE = "    skinReleaseCopies();        // the in-flight copies go with the pool they were learned on (counted as lost by the run)\n"
A_BAT_VERIFY = "python \"tools\\skin_palette_check.py\" --verify-fixture \"%OBJ%\\skinledger\\fixture\" || exit /b 1\n"
A_SKINFILE_ORDER = "    const bool ok = g_skin.writeFile(path);\n"

MUTANTS = [
    # S1 constants
    M("keep-is-the-old-box", "S1", "ledger", [(A_KEEP, "constexpr uint32_t kKeepPaletteBytes = 1u << 20;")],
      "the kept prefix is a megabyte again: not whole rows, the size of the box that read zeros"),
    M("chain-hash-moved", "S1", "ledger", [(A_CHAIN, "constexpr uint64_t kChainHash = 0x6FE04AF836BB1DBBull;")], "the palette chain's hash is off by one"),
    M("clear-hash-moved", "S1", "ledger", [(A_CLEARHASH, "constexpr uint64_t kClearHash = 0x7B2A531B72941108ull;")], "the identity fill's hash is off by one"),
    # S2 unarmed
    M("window-ignores-armed", "S2", "ledger", [(A_WINDOW, "bool inWindow(uint32_t f) const { return f >= frame0_ && f - frame0_ < kFrames; }\n")],
      "an unarmed ledger has a window (frame 0 is in it) and takes events"),
    # S3 window
    M("window-one-too-long", "S3", "ledger", [(A_WINDOW, "bool inWindow(uint32_t f) const { return armed_ && f >= frame0_ && f - frame0_ <= kFrames; }\n")],
      "frame 120 of a run armed at 100 is still in it"),
    M("window-misses-its-first-frame", "S3", "ledger", [(A_WINDOW, "bool inWindow(uint32_t f) const { return armed_ && f > frame0_ && f - frame0_ < kFrames; }\n")],
      "the arming frame is outside its own window"),
    # S4 plan
    M("joints-staged-every-dispatch", "S4", "ledger", [(A_JOINTS_SHARE, "if (false) d.jointsFrom = k;")], "a second dispatch over the same joints stages them again"),
    M("bind-staged-every-dispatch", "S4", "ledger", [(A_BIND_FIND, "if (false) plan.bind = static_cast<int>(k);")], "the same bind-pose buffer is staged again and again"),
    M("dispatch-cap-gone", "S4", "ledger", [(A_CAP, "if (fr.d.size() >= kMaxDispatches * 4) {")], "a frame keeps more chain dispatches than the cap"),
    # S5 numbers
    M("gap-counted-as-overlap", "S5", "ledger", [(A_GAP, "if (jobs[k].first > end) ++s.overlaps;")], "a late-starting job is called an overlap"),
    M("pool-bases-never-mismatch", "S5", "ledger", [(A_NOTDST, "if (false) ++s.poolNotDst;")], "a t33 base that is no job's dst is not counted"),
    M("empty-job-counted", "S5", "ledger", [(A_ZERO, "")], "a job with no bones takes part in the running sum"),
    # S6 whole run
    M("finished-before-the-lines", "S6", "ledger", [(A_REPORT_TOP, "        if (!armed_ || finished_) return;\n        finished_ = true;\n        uint32_t anyDispatch = 0,")],
      "the finished flag is set before the report has said anything"),
    # S7 fewer frames
    M("report-needs-the-whole-window", "S7", "ledger",
      [(A_REPORT_TOP, "        if (!armed_ || finished_ || frames_[kFrames - 1].dispatches == 0) return;\n        uint32_t anyDispatch = 0,")],
      "a run whose last frame never arrived reports nothing"),
    # S8 never ran
    M("silent-when-nothing-ran", "S8", "ledger", [(A_VERDICT, "        if (anyDispatch == 0) return;\n        const char* verdict;\n        char why[256];\n")],
      "a run that saw no dispatch says nothing after its frame lines: silence read as success"),
    M("other-dispatches-not-told-apart", "S8", "ledger", [(A_OTHER, "} else if (false) {")], "dispatches of other shaders are not told from a chain that ran"),
    # S9 broken
    M("lost-copies-are-fine", "S9", "ledger", [(A_BROKEN, "} else if (declined || skipped_ || jobsMissing || dataFrames < chainFrames || palFrames == 0) {")],
      "a lost copy does not make a run BROKEN"),
    M("no-palette-is-fine", "S9", "ledger", [(A_NOPALETTE, "dataFrames < chainFrames) {")],
      "a chain that ran with no palette ever read back (no pool draw, or its copy never came) is not BROKEN"),
    M("undelivered-job-table-is-fine", "S9", "ledger", [(A_DATAOK, "")], "a staged job table that never arrived does not count against the frame's data"),
    # S10 shutdown
    M("shutdown-says-window-closed", "S10", "ledger", [(A_HOW, "verdict, stamp.c_str(), \"window closed\", why, chainFrames,")],
      "a run the process ended is reported as a window that closed"),
    M("file-status-dropped", "S10", "ledger", [(A_FILESTATUS, "kKeepPaletteBytes / kRowBytes, notDst, gaps, overlaps, poolReleases_, foreign_, stamp.c_str(), \"written\");")],
      "the result claims the file was written whatever became of it"),
    M("foreign-count-not-reported", "S8", "ledger", [(A_FOREIGN_ARG, "poolReleases_, 0u, stamp.c_str(), fileStatus);")],
      "the result does not say how many dispatches were left unread on deferred contexts"),
    M("unknown-pool-said-zero", "S10", "ledger", [(A_POOLKNOWN, "\"\", s.poolSkinned,")], "no pool copy reads as zero skinned bases"),
    # S11 file
    M("file-magic-changed", "S11", "ledger", [(A_MAGIC, "static const char kMagic[8] = {'E', 'D', 'V', 'R', 'S', 'K', 'N', '2'};")], "the file does not start with the magic the reader wants"),
    M("header-fields-swapped", "S11", "ledger", [(A_HDR, "u32(1u); u32(frame0_); u32(kFrames); u32(paletteCount_);")], "the frame count and the first frame are swapped"),
    # S12 glue
    M("hook-ungated", "S12", "exposure", [(A_HOOK_FOREIGN, "        objectProbeNoteDispatch(self, x, y, z, true);\n")],
      "the deferred-context path calls the skin ledger without asking whether a run is armed"),
    M("deferred-dispatch-read", "S12", "probe", [(A_FOREIGN_RET, "")],
      "a dispatch recorded on a deferred context (any thread) goes on to touch the owner thread's ledger state"),
    M("note-gate-gone", "S12", "probe", [(A_NOTE_HEAD, "void objectProbeNoteDispatch(ID3D11DeviceContext* ctx, uint32_t x, uint32_t y, uint32_t z, bool foreign) {\n    if (!ctx) return;\n")],
      "the dispatch note reads the context whether or not a run is armed"),
    M("second-arm", "S12", "probe", [(A_ARM_RESET, "    g_skin.arm(g_ledgerFrame0);\n")], "something other than the eye run's arm arms the skin ledger"),
    M("report-after-pool-release", "S12", "probe", [(A_WRITE_CALL, ""), (A_POOL_SWAP, A_POOL_SWAP + "        if (i == 0) writeSkinLedger(dir, \"window closed\");\n")],
      "the skin report comes after the pool copies it joins its t33 bases to are let go"),
    M("report-before-the-file", "S12", "probe", [(A_SKINFILE_ORDER, "    const bool ok = true;\n")], "the report does not wait for the skin file to be written"),
    M("palette-copy-boxed-again", "S12", "probe", [(A_WHOLE, "g_palette[i], g_paletteBytes[i], kLedgerBonesMax);")],
      "the palette copy is a box of the first megabyte again"),
    M("no-shutdown-report", "S12", "probe", [(A_SHUTDOWN, "    if (false) {\n        std::string stamp;")], "a run armed and never reported is not reported at shutdown"),
    M("readback-not-trimmed", "S12", "probe", [(A_KEEPREAD, "ledgerRead(ctx, c, into)")], "the palette readback keeps the whole 8 MB again"),
    M("staging-not-released", "S12", "probe", [(A_RELEASE, "")], "the skin ledger's staging buffers outlive the ledger's"),
    M("rig-not-in-the-build", "S12", "bat", [(A_BAT_VERIFY, "")], "build.bat does not have the checker read the rig's fixture"),
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
    """Every check label the rig printed on a 'FAIL: <case>.<what>' line (the text up to ' -- ', ' [' or the end of the line); the rig's
    closing 'FAIL: skin ledger: n of m checks failed' line is not a label."""
    labels = []
    for line in output.splitlines():
        if line.startswith("FAIL: "):
            label = re.split(r" -- | \[", line[len("FAIL: "):], 1)[0].strip()
            if re.match(CASE_PREFIX + r"\d+\.", label):
                labels.append(label)
    return labels


def rig_cases(rig_text):
    """The case ids the rig's checks are labelled with."""
    return set(re.findall(r'"(' + CASE_PREFIX + r'\d+)[.:]', rig_text))


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
    """Compile the rig against the header tree at `tree`; (exit code, output)."""
    cmd = [tc.cl] + CL_FLAGS + ["/I" + str(tree / d) for d in INCLUDE_DIRS] + ["/Fo" + str(exe.parent) + "\\", "/Fe" + str(exe), str(RIG), "/link", "/INCREMENTAL:NO"] + LINK_ARGS
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
    """A repository root holding the glue sources the rig reads as text, `mutated` replacing one of them."""
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
        lines.append("  %-40s %-8s caught by %-7s %s" % (m.name, m.file, "/".join(m.caught), m.why))
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
    work = Path(tempfile.mkdtemp(prefix="slm_"))
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
    check(fail_labels("a\nFAIL: S4.flight -- x [y]\nFAIL: S2.sub\nFAIL: skin ledger: 3 of 9\n") == ["S4.flight", "S2.sub"],
          "fail_labels reads the label of every check's FAIL: line, and not the closing summary")
    check(fail_labels("PASS: 12 skin ledger checks\n") == [], "fail_labels reads nothing from a pass")
    check(apply_edits("a b c", [("b", "B")]) == "a B c", "apply_edits applies one edit")
    for bad, why in ((("a b b", [("b", "B")]), "a repeated anchor"), (("a b", [("x", "y")]), "a missing anchor"), (("a b", [("b", "b")]), "a no-op edit")):
        try:
            apply_edits(*bad)
            check(False, "apply_edits refuses " + why)
        except ValueError:
            pass
    joined = label_block(":rig_a\nx ^\n y\nexit /b 0\n:rig_b\nz\n", ":rig_a")
    check(joined is not None and [" ".join(l.split()) for l in joined] == ["x y", "exit /b 0"], "label_block joins continuations and stops at the next label")
    check(rig_cases('check(x, "S1.a: b"); check(y, "S10.c")') == {"S1", "S10"}, "rig_cases reads the case ids off the labels")

    # every mutation against the sources as they are now, and against the rig
    sources = {k: read_source(p) for k, p in FILES.items()}
    rig = read_source(RIG)
    cases = rig_cases(rig)
    names = [m.name for m in MUTANTS]
    check(len(names) == len(set(names)), "mutation names are unique")
    check(len(MUTANTS) >= MIN_MUTANTS, "the mutation list did not shrink below %d (%d)" % (MIN_MUTANTS, len(MUTANTS)))
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
        for d in INCLUDE_DIRS:
            check('/I"%s"' % d.replace("/", "\\") in cl, "build.bat's rig finds the headers through /I %s (the mutated tree is found the same way)" % d)
        check(RIG_SOURCE_IN_BAT in cl, "build.bat's rig compile has the rig's source")
        check(RIG_EXE_IN_BAT + '" --dry-run' in text and RIG_EXE_IN_BAT + '" --self-test "%ROOT%"' in text,
              "build.bat runs the rig's --dry-run and --self-test with the repository root (the S12 pins read the glue's sources from it)")
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
    print("PASS: skin_ledger_test mutants.py self-test (%d mutations over %d cases, every anchor found once, build.bat wired)" % (len(MUTANTS), len(covered)))
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
