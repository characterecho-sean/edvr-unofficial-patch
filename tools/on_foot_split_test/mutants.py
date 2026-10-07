#!/usr/bin/env python3
"""The mutation proof for tools\\on_foot_split_test: the rig fails when a rule of the pure headers is flipped.

The rig (on_foot_split_test.cpp) pins the commander's on-foot verdict and the split the shader is given (src\\common\\temporal_mode.h,
rules R1 and R3) and the seat Status.json's flags make (src\\d3d11\\journal_watch.h, rule R2). A rig that passes proves little until it is
seen to FAIL on a header that breaks the rule it pins. This tool does that: for each mutation below it copies the two production headers
into a temp directory OUTSIDE the repo, applies one textual edit, compiles the rig against that copy (the temp directory first on the
include path, the repo's headers behind it for the shader's source), runs it from the repository root, and requires the rig to fail on the
check that belongs to the rule: the first line it prints is "FAIL R<n><letter>. ..." and the label must be one the mutation names.
Nothing is written inside the repo; the temp directory is removed at the end. The shader's own mutations (six, the gate, the comparison,
the far plane, the two translations, the decision code) and the source pins' controls run inside the rig.

  python tools\\on_foot_split_test\\mutants.py --self-test       text only: every anchor is found exactly once in its header as it is now,
                                                               every label named is in the rig, and build.bat compiles the rig the way
                                                               this tool does
  python tools\\on_foot_split_test\\mutants.py --run [--only a,b] [--jobs N] [--keep]
  python tools\\on_foot_split_test\\mutants.py --list
  python tools\\on_foot_split_test\\mutants.py --run --dry-run    the plan; writes nothing, starts nothing

--run needs the MSVC toolchain (cl.exe on PATH, or Visual Studio found with vswhere as build.bat does) and takes under a minute.
--self-test runs in build.bat's rig and is what keeps an edit of a header from silently orphaning a mutation: if an anchor stops matching,
the build fails and this file says which.
"""
import argparse
import concurrent.futures
import importlib.util
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
HEADERS = {
    "mode": ROOT / "src" / "common" / "temporal_mode.h",
    "journal": ROOT / "src" / "d3d11" / "journal_watch.h",
}
RIG = HERE / "on_foot_split_test.cpp"
BUILD_BAT = ROOT / "build.bat"
RIG_LABEL = ":rig_on_foot_split_test"

# How build.bat compiles the rig; --self-test checks that the label still says the same.
CL_FLAGS = ["/nologo", "/O2", "/MT", "/std:c++17", "/EHsc", "/W4", "/DWIN32_LEAN_AND_MEAN", "/DNOMINMAX", "/D_CRT_SECURE_NO_WARNINGS", "/DUNICODE", "/D_UNICODE"]
LINK = ["/link", "/INCREMENTAL:NO", "d3d11.lib", "d3dcompiler.lib", "dxguid.lib"]
RUN_TIMEOUT = 120.0


class Mutant:
    def __init__(self, name, header, caught, edits, why):
        self.name = name
        self.header = header                                              # "mode" or "journal"
        self.caught = tuple([caught] if isinstance(caught, str) else caught)   # label prefixes that count as caught by its rule
        self.edits = list(edits)                                          # (old, new) pairs, applied in order
        self.why = why


def M(name, header, caught, edits, why):
    return Mutant(name, header, caught, edits, why)


# ---- the anchors: text of the production headers, verbatim (the self-test finds each exactly once) -------------------------------
WATCHING = "    if (!f.watching) return TemporalFootWhy::NotWatching;\n"
GAMEPLAY = "    if (!f.gameplay) return TemporalFootWhy::NoGameplay;\n"
UNKNOWN = "    if (!f.known || !f.vehicleKnown) return TemporalFootWhy::Unknown;\n"
STALE = "    if (f.sampleAgeMs > kTemporalFootStaleMs) return TemporalFootWhy::Stale;\n"
SEATED = "    if (f.seated) return TemporalFootWhy::Seated;\n"
NOT_ON_FOOT = "    if (!f.onFoot) return TemporalFootWhy::NotOnFoot;\n"
YES = "inline bool temporalOnFoot(const TemporalFoot& f) { return temporalFootVerdict(f) == TemporalFootWhy::OnFoot; }\n"
ON_FOOT_SPLIT = "inline constexpr float kTemporalOnFootSplitMetres = 0.001f;\n"
STALE_MS = "inline constexpr unsigned kTemporalFootStaleMs = 5000;\n"
SPLIT = "    return onFoot && configured > 0.0f ? kTemporalOnFootSplitMetres : configured;\n"
GAP_MS = "inline constexpr unsigned kTemporalFootGapMs = 1000;\n"
TRACK_RESTART = "        if (!started || readCount != samples || nowMs - callMs > kTemporalFootGapMs) {\n"
TRACK_START = "            samplesMs = nowMs;\n"
TRACK_CALL = "        callMs = nowMs;\n"
TRACK_CLAMP = "        f.sampleAgeMs = age > 0x7FFFFFFFull ? 0x7FFFFFFFu : static_cast<unsigned>(age);\n"
TRACK_WHY = "        why = temporalFootVerdict(f);\n"
TRACK_SAME = "        if (now == on) return false;\n"
TRACK_ON = "        on = now;\n"
TRACK_CHANGES = "        ++changes;\n"
SEAT_FLAGS = "constexpr uint32_t kStatusFlagsSeated = (1u << 24) | (1u << 25) | (1u << 26);\n"
SEAT_FLAGS2 = "constexpr uint32_t kStatusFlags2Seated = (1u << 1) | (1u << 2);\n"
SEAT = "    return (flags & kStatusFlagsSeated) != 0 || (flags2 & kStatusFlags2Seated) != 0;\n"


def drop(old):
    return [(old, "")]


MUTANTS = [
    # ---- R1: the verdict ----------------------------------------------------------------------------------------------------------
    M("journal-off-still-on-foot", "mode", "R1f", drop(WATCHING), "a journal that is not read says on foot"),
    M("no-loadgame-still-on-foot", "mode", "R1f", drop(GAMEPLAY), "the last session's Status.json is believed before this one's LoadGame"),
    M("unknown-flags2-is-on-foot", "mode", "R1d", [(UNKNOWN, "    if (!f.vehicleKnown) return TemporalFootWhy::Unknown;\n")], "a menu's Status.json (no Flags2) is believed"),
    M("unknown-flags-is-on-foot", "mode", "R1d", [(UNKNOWN, "    if (!f.known) return TemporalFootWhy::Unknown;\n")], "a Status.json without Flags is believed"),
    M("stale-is-on-foot", "mode", "R1e", drop(STALE), "a stopped reader's last word is believed forever"),
    M("stale-limit-inclusive", "mode", "R1e", [(STALE, "    if (f.sampleAgeMs >= kTemporalFootStaleMs) return TemporalFootWhy::Stale;\n")], "the limit itself is stale"),
    M("seated-is-on-foot", "mode", "R1b", drop(SEATED), "a cockpit is on foot whenever Flags2 says so"),
    M("seated-only-when-not-on-foot", "mode", "R1g", [(SEATED, "    if (f.seated && !f.onFoot) return TemporalFootWhy::Seated;\n")], "a sample that says both is on foot"),
    M("not-on-foot-is-on-foot", "mode", "R1h", drop(NOT_ON_FOOT), "every commander not seated is on foot"),
    M("yes-inverted", "mode", "R1a", [(YES, "inline bool temporalOnFoot(const TemporalFoot& f) { return temporalFootVerdict(f) != TemporalFootWhy::OnFoot; }\n")],
      "the yes is the verdict's no"),
    M("stale-limit-zero", "mode", ("R1c", "R1j", "R1l"), [(STALE_MS, "inline constexpr unsigned kTemporalFootStaleMs = 0;\n")], "any age is stale: the mode never stays on"),
    M("stale-limit-huge", "mode", ("R1j", "R1l"), [(STALE_MS, "inline constexpr unsigned kTemporalFootStaleMs = 5000000;\n")], "nothing is stale for an hour and a half"),
    # ---- R1: the verdict over time (TemporalFootTracker) ---------------------------------------------------------------------------
    M("clock-ignores-the-first-question", "mode", "R1p", [(TRACK_RESTART, "        if (readCount != samples || nowMs - callMs > kTemporalFootGapMs) {\n")],
      "the clock starts at tick 0, not at the first question"),
    M("clock-ignores-the-count-moving", "mode", "R1m", [(TRACK_RESTART, "        if (!started || nowMs - callMs > kTemporalFootGapMs) {\n")],
      "a reader that came back is stale for as long as the first silence said"),
    M("clock-ignores-our-stall", "mode", "R1n", [(TRACK_RESTART, "        if (!started || readCount != samples) {\n")], "a hitch of the pass's own reads as a stopped reader"),
    M("gap-inclusive", "mode", "R1o", [(TRACK_RESTART, "        if (!started || readCount != samples || nowMs - callMs >= kTemporalFootGapMs) {\n")], "a gap of exactly a second is a stall"),
    M("gap-a-tenth-of-a-second", "mode", "R1o", [(GAP_MS, "inline constexpr unsigned kTemporalFootGapMs = 100;\n")], "any pause restarts the clock: nothing is ever stale"),
    M("gap-ten-seconds", "mode", "R1n", [(GAP_MS, "inline constexpr unsigned kTemporalFootGapMs = 10000;\n")], "a stall of eight seconds is a stopped reader"),
    M("clock-never-sees-the-call", "mode", "R1m", drop(TRACK_CALL), "the gap is measured from tick 0: every question restarts the clock"),
    M("clock-never-starts", "mode", "R1m", drop(TRACK_START), "the clock's start stays at tick 0: stale from the first question"),
    M("age-wraps-at-49-days", "mode", "R1s", [(TRACK_CLAMP, "        f.sampleAgeMs = static_cast<unsigned>(age);\n")], "after 2^32 ms of looking the age reads fresh again"),
    M("tracker-keeps-the-old-reason", "mode", "R1m", drop(TRACK_WHY), "the verdict is never recomputed"),
    M("tracker-same-answer-is-a-change", "mode", "R1m", [(TRACK_SAME, "        if (now == on) return true;\n")], "every question reports a change"),
    M("tracker-mode-never-moves", "mode", "R1m", drop(TRACK_ON), "the mode is never taken"),
    M("tracker-changes-not-counted", "mode", "R1m", drop(TRACK_CHANGES), "the change count stays zero"),
    # ---- R3: the split ------------------------------------------------------------------------------------------------------------
    M("on-foot-split-zero", "mode", "R3d", [(ON_FOOT_SPLIT, "inline constexpr float kTemporalOnFootSplitMetres = 0.0f;\n")],
      "the on-foot split is zero: the shader reads it as the world path OFF and puts everything on the head path"),
    M("on-foot-split-ten", "mode", "R3d", [(ON_FOOT_SPLIT, "inline constexpr float kTemporalOnFootSplitMetres = 10.0f;\n")], "the on-foot split is the cockpit's: the bug stays"),
    M("on-foot-split-a-metre", "mode", "R3d", [(ON_FOOT_SPLIT, "inline constexpr float kTemporalOnFootSplitMetres = 1.0f;\n")],
      "a metre: the commander's own near things stay on the head path"),
    M("split-ignores-the-players-zero", "mode", "R3e", [(SPLIT, "    return onFoot ? kTemporalOnFootSplitMetres : configured;\n")],
      "on foot turns the world path back on for a player who turned it off"),
    M("split-ignores-on-foot", "mode", "R3b", [(SPLIT, "    return configured > 0.0f ? kTemporalOnFootSplitMetres : configured;\n")], "the millimetre in a cockpit"),
    M("split-never-changes", "mode", "R3c", [(SPLIT, "    return configured;\n")], "the on-foot split is the configured one: no fix"),
    # ---- R2: the seat ------------------------------------------------------------------------------------------------------------
    M("seat-drops-the-main-ship", "journal", "R2a", [(SEAT_FLAGS, "constexpr uint32_t kStatusFlagsSeated = (1u << 25) | (1u << 26);\n")], "Flags bit 24 seats nobody"),
    M("seat-drops-the-fighter", "journal", "R2a", [(SEAT_FLAGS, "constexpr uint32_t kStatusFlagsSeated = (1u << 24) | (1u << 26);\n")], "Flags bit 25 seats nobody"),
    M("seat-drops-the-srv", "journal", "R2a", [(SEAT_FLAGS, "constexpr uint32_t kStatusFlagsSeated = (1u << 24) | (1u << 25);\n")], "Flags bit 26 seats nobody"),
    M("seat-adds-the-hud-mode", "journal", "R2a", [(SEAT_FLAGS, "constexpr uint32_t kStatusFlagsSeated = (1u << 24) | (1u << 25) | (1u << 26) | (1u << 27);\n")],
      "the HUD's analysis mode (Flags bit 27) seats the commander"),
    M("seat-drops-the-taxi", "journal", "R2a", [(SEAT_FLAGS2, "constexpr uint32_t kStatusFlags2Seated = (1u << 2);\n")], "Flags2 bit 1 seats nobody"),
    M("seat-drops-multicrew", "journal", "R2a", [(SEAT_FLAGS2, "constexpr uint32_t kStatusFlags2Seated = (1u << 1);\n")], "Flags2 bit 2 seats nobody"),
    M("seat-adds-on-foot-in-station", "journal", "R2a", [(SEAT_FLAGS2, "constexpr uint32_t kStatusFlags2Seated = (1u << 1) | (1u << 2) | (1u << 3);\n")],
      "on foot in a station (Flags2 bit 3) seats the commander"),
    M("seat-ignores-flags2", "journal", "R2c", [(SEAT, "    return (flags & kStatusFlagsSeated) != 0;\n")], "a taxi or someone else's ship seats nobody"),
    M("seat-ignores-flags", "journal", "R2b", [(SEAT, "    return (flags2 & kStatusFlags2Seated) != 0;\n")], "a ship or an SRV seats nobody"),
    M("seat-needs-both-words", "journal", "R2b", [(SEAT, "    return (flags & kStatusFlagsSeated) != 0 && (flags2 & kStatusFlags2Seated) != 0;\n")], "only a ship and a taxi at once seats"),
]


# ---- applying an edit ----------------------------------------------------------------------------------------------------------
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


def parse_fail(output):
    """The label of the first 'FAIL R<n><letter>. ' line of the rig's output (what the rig printed before it died), or None."""
    for line in output.splitlines():
        m = re.match(r"FAIL (R\d+[a-z]?)\. (.*)", line)
        if m:
            return m.group(1), m.group(2)
    return None


def rig_label_exists(rig_text, label):
    return ('"%s"' % label) in rig_text


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


# ---- the toolchain: the house one (panel_curve_test finds cl.exe the way build.bat does) ----------------------------------------
def toolchain():
    spec = importlib.util.spec_from_file_location("panel_mutants_toolchain", ROOT / "tools" / "panel_curve_test" / "mutants.py")
    module = importlib.util.module_from_spec(spec)
    previous = sys.dont_write_bytecode
    sys.dont_write_bytecode = True
    try:
        spec.loader.exec_module(module)
    finally:
        sys.dont_write_bytecode = previous
    return module.Toolchain()


def build_rig(tc, header_dir, outdir):
    """Compile the rig against the headers in header_dir first (the repo's behind them, for the shader's source); (exit code, output, exe)."""
    exe = outdir / "rig.exe"
    cmd = [tc.cl] + CL_FLAGS + ["/I" + str(header_dir), "/I" + str(ROOT / "src" / "common"), "/I" + str(ROOT / "src" / "d3d11"),
                                "/Fo" + str(outdir) + os.sep, "/Fe" + str(exe), str(RIG)] + LINK
    done = subprocess.run(cmd, capture_output=True, text=True, env=tc.env, errors="replace", cwd=str(outdir))
    return done.returncode, (done.stdout + done.stderr), exe


def run_rig(exe, tc):
    """(outcome, detail) of one run of the rig from the repo root: 'pass', 'fail' (detail = label and reason), 'crash', 'timeout'."""
    try:
        done = subprocess.run([str(exe), "--self-test", str(ROOT)], capture_output=True, text=True, env=tc.env, errors="replace", timeout=RUN_TIMEOUT, cwd=str(ROOT))
    except subprocess.TimeoutExpired:
        return "timeout", "no result within %d s" % RUN_TIMEOUT
    if done.returncode == 0:
        return "pass", ""
    failed = parse_fail(done.stdout + "\n" + done.stderr)
    if failed is None:
        return "crash", "exit 0x%08X" % (done.returncode & 0xFFFFFFFF)
    return "fail", "%s. %s" % failed


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
    lines = ["%d mutation(s); each header copy is compiled with the rig in a temp directory outside the repo and run from the repo root:" % len(mutants)]
    for m in mutants:
        lines.append("  %-34s %-8s caught by %-10s %s" % (m.name, HEADERS[m.header].name, "/".join(m.caught), m.why))
    lines.append("cl.exe flags: " + " ".join(CL_FLAGS))
    return "\n".join(lines)


def run_all(only=None, jobs=None, keep=False, dry_run=False, out=sys.stdout):
    mutants = select(only)
    if dry_run:
        print(plan_text(mutants), file=out)
        print("dry-run: nothing compiled, started or written", file=out)
        return 0
    prod = {k: read_source(p) for k, p in HEADERS.items()}
    tc = toolchain()
    work = Path(tempfile.mkdtemp(prefix="on_foot_split_mutants_"))
    try:
        def build_and_run(name, texts):
            d = work / name
            d.mkdir()
            for key, text in texts.items():
                (d / HEADERS[key].name).write_text(text, encoding="utf-8", newline="\n")
            code, text, exe = build_rig(tc, d, d)
            if code != 0:
                return "nocompile", text.strip().splitlines()[-1] if text.strip() else ""
            return run_rig(exe, tc)

        control, detail = build_and_run("control", prod)
        print("control (the unmutated headers, every rule): %s %s" % (control, detail), file=out)
        if control != "pass":
            print("the rig does not pass on the unmutated headers when built this way; nothing below means anything", file=out)
            return 1

        def one(m):
            try:
                texts = dict(prod)
                texts[m.header] = apply_edits(prod[m.header], m.edits, m.name)
            except ValueError as error:
                return m, "badedit", str(error)
            outcome, detail = build_and_run(m.name, texts)
            return m, outcome, detail

        results = []
        with concurrent.futures.ThreadPoolExecutor(max_workers=jobs or min(8, os.cpu_count() or 2)) as pool:
            for m, outcome, detail in pool.map(one, mutants):
                if outcome == "fail" and any(detail.startswith(p + ".") for p in m.caught):
                    verdict = "caught"
                elif outcome == "fail":
                    verdict = "OTHER"
                else:
                    verdict = {"pass": "SURVIVED", "crash": "CRASH", "timeout": "TIMEOUT", "nocompile": "NOCOMPILE", "badedit": "BADEDIT"}[outcome]
                results.append((m, verdict, detail))
                print("%-9s %-34s %s" % (verdict, m.name, detail[:140]), file=out)
        bad = [r for r in results if r[1] != "caught"]
        print("%d mutation(s): %d caught by their own rule, %d not" % (len(results), len(results) - len(bad), len(bad)), file=out)
        for m, verdict, detail in bad:
            print("  NOT CAUGHT: %s (%s): wanted %s, got %s %s" % (m.name, m.why, "/".join(m.caught), verdict, detail[:120]), file=out)
        return 0 if not bad else 1
    finally:
        if keep:
            print("kept: %s" % work, file=out)
        else:
            shutil.rmtree(work, ignore_errors=True)


# ---- the self-test ----------------------------------------------------------------------------------------------------------------------
def self_test():
    failures = []

    def check(ok, what):
        if not ok:
            failures.append(what)

    # the tool's own pieces
    check(parse_fail("a\nFAIL R1c. x y\nFAIL R2a. z\n") == ("R1c", "x y") and parse_fail("PASS: 3\n") is None, "parse_fail reads the first FAIL line")
    check(apply_edits("a b c", [("b", "B")]) == "a B c", "apply_edits applies one edit")
    for bad, why in ((("a b b", [("b", "B")]), "a repeated anchor"), (("a b", [("x", "y")]), "a missing anchor"), (("a b", [("b", "b")]), "a no-op edit")):
        try:
            apply_edits(*bad)
            check(False, "apply_edits refuses " + why)
        except ValueError:
            pass
    joined = label_block(":rig_a\nx ^\n y\nexit /b 0\n:rig_b\nz\n", ":rig_a")
    check(joined is not None and [" ".join(l.split()) for l in joined] == ["x y", "exit /b 0"], "label_block joins continuations and stops at the next label")

    # every mutation against the headers as they are now, and against the rig
    prod = {k: read_source(p) for k, p in HEADERS.items()}
    rig = read_source(RIG)
    names = [m.name for m in MUTANTS]
    check(len(names) == len(set(names)), "mutation names are unique")
    check(len(MUTANTS) >= 38, "the mutation list did not shrink (%d)" % len(MUTANTS))
    for m in MUTANTS:
        try:
            mutated = apply_edits(prod[m.header], m.edits, m.name)
            check(mutated != prod[m.header], "%s changes the header" % m.name)
        except ValueError as error:
            failures.append(str(error))
        for label in m.caught:
            check(rig_label_exists(rig, label), "%s: the rig has no check labelled %s" % (m.name, label))
    rules = {re.match(r"R\d+", label).group(0) for m in MUTANTS for label in m.caught}
    check(rules >= {"R1", "R2", "R3"}, "the mutations cover the rig's three pure rules: %s" % sorted(rules))

    # build.bat compiles the rig the way this tool does, and runs the rig, the fixture's self-test and this tool's self-test
    bat = BUILD_BAT.read_bytes().decode("utf-8", errors="replace")
    block = label_block(bat, RIG_LABEL)
    check(block is not None, "build.bat has %s" % RIG_LABEL)
    if block:
        text = "\n".join(block)
        cl = next((l for l in block if l.strip().lower().startswith("cl.exe")), "")
        for flag in CL_FLAGS + LINK[1:]:
            check(flag in cl, "build.bat's rig compile has %s" % flag)
        check("tools\\on_foot_split_test\\on_foot_split_test.cpp" in cl, "build.bat compiles the rig")
        check('/I"src\\common"' in cl and '/I"src\\d3d11"' in cl, "build.bat's rig compile finds the headers through /I src\\common and src\\d3d11 (the mutated copies are found the same way)")
        check("on_foot_split_test.exe\" --dry-run" in text and "on_foot_split_test.exe\" --self-test \"%ROOT%\"" in text,
              "build.bat runs the rig's --dry-run and --self-test with the repo root")
        check("on_foot_split_fixture.py\" --self-test" in text, "build.bat runs the fixture tool's --self-test")
        check("mutants.py\" --self-test" in text, "build.bat runs this tool's --self-test")
    check((HERE / "fixture_onfoot.bin").is_file(), "the fixture is in the repo")

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
    print("PASS: on_foot_split_test mutants.py self-test (%d mutations over %s, every anchor found once, build.bat wired)" % (len(MUTANTS), sorted(rules)))
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--self-test", action="store_true")
    ap.add_argument("--run", action="store_true")
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--only", default="")
    ap.add_argument("--jobs", type=int, default=0)
    ap.add_argument("--keep", action="store_true")
    a = ap.parse_args(argv)
    if a.self_test:
        return self_test()
    if a.list:
        print(plan_text(select(a.only)))
        return 0
    if a.run:
        try:
            return run_all(a.only, a.jobs or None, a.keep, a.dry_run)
        except ValueError as error:
            print(str(error), file=sys.stderr)
            return 2
    ap.print_help()
    return 2


if __name__ == "__main__":
    sys.exit(main())
