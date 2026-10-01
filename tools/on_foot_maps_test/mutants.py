#!/usr/bin/env python3
"""The mutation proof for tools\\on_foot_maps_test: the rig fails when a rule of the pure header is flipped.

The rig (on_foot_maps_test.cpp) pins nine rules of src\\d3d11\\ui_maps_math.h, R1..R9 (its header says which). A rig that
passes proves little until it is seen to FAIL on a header that breaks the rule it pins. This tool does that: for each mutation
below it copies the production header into a temp directory OUTSIDE the repo, applies one textual edit (or a few that belong
together), compiles the rig against that copy, runs it on the rule's cases, and requires the rig to fail on the check that
belongs to the rule (the first line it prints after FAIL: starts with the label prefix the mutation names). Nothing is written
inside the repo; the temp directory is removed at the end. The source pins (P1..P8) read the runtime's sources and are not
mutated here (P4a and P8 carry controls of their own: the same predicate over copies with one edit each must fail).

  python tools\\on_foot_maps_test\\mutants.py --self-test        text only: every anchor is found exactly once in the header as it
                                                                is now, every label named is in the rig, and build.bat compiles
                                                                the rig the way this tool does
  python tools\\on_foot_maps_test\\mutants.py --run [--only a,b] [--jobs N] [--keep]
  python tools\\on_foot_maps_test\\mutants.py --list
  python tools\\on_foot_maps_test\\mutants.py --run --dry-run    the plan; writes nothing, starts nothing

--run needs the MSVC toolchain (cl.exe on PATH, or Visual Studio found with vswhere as build.bat does) and takes about a minute.
--self-test runs in build.bat's rig and is what keeps an edit of the header from silently orphaning a mutation: if an anchor
stops matching, the build fails and this file says which.
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
PROD = ROOT / "src" / "d3d11" / "ui_maps_math.h"
RIG = HERE / "on_foot_maps_test.cpp"
BUILD_BAT = ROOT / "build.bat"
RIG_LABEL = ":rig_on_foot_maps_test"

# How build.bat compiles the rig; --self-test checks that the label still says the same.
CL_FLAGS = ["/nologo", "/O2", "/MT", "/std:c++17", "/EHsc", "/W4", "/DWIN32_LEAN_AND_MEAN", "/DNOMINMAX", "/D_CRT_SECURE_NO_WARNINGS"]
RUN_TIMEOUT = 90.0


class Mutant:
    def __init__(self, name, caught, edits, why):
        self.name = name
        self.caught = tuple([caught] if isinstance(caught, str) else caught)   # label prefixes that count as caught by its rule
        self.edits = list(edits)                                                # (old, new) pairs, applied in order
        self.why = why
        self.rule = re.match(r"R\d+", self.caught[0]).group(0)                  # the rig's case to run


def M(name, caught, edits, why):
    return Mutant(name, caught, edits, why)


# ---- the anchors: text of the production header, verbatim (the self-test finds each exactly once) -------------------------
LOWER = "        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');\n"
KEY_END = "    return *text == 0 ? UiMapsKey::On : UiMapsKey::Off;\n"
KEY_NULL = "    if (!text) return UiMapsKey::Off;\n"
KEY_MISMATCH = "        if (c != *want) return UiMapsKey::Off;\n"
KEY_NAME = '{ return key == UiMapsKey::On ? "on" : "off"; }'
HOLD_CONST = "constexpr uint32_t kUiMapsHoldFrames = 2;\n"
RELEASE_CONST = "constexpr uint32_t kUiMapsReleaseFrames = 3;\n"
PANEL_UNNAMED = "        if (!named) {\n            g.run = 0;\n            return UiMapsEdge::None;\n        }\n"
HOLD_EDGE = "            g.world = true;\n            g.run = 0;\n            return UiMapsEdge::Hold;\n"
WORLD_NAMED = "    if (named) {\n        g.run = 0;\n        return UiMapsEdge::None;\n    }\n"
RELEASE_EDGE = "        g.world = false;\n        g.run = 0;\n        return UiMapsEdge::Release;\n"
COMBINE = "    return decide ? g.world : (byJournal || byDepth);\n"
CARRY = "    g.world = heldNow;\n    g.run = 0;\n"
DOOR_ZERO = "    if (seq == 0 || takenSeq != seq) return UiMapsDoor::No;\n"
DOOR_COUNT = "    return eyeDraws <= takenDraws ? UiMapsDoor::Yes : UiMapsDoor::NotEmpty;\n"
ON_COUNTS = "        static_cast<unsigned long long>(frame), kUiMapsHoldFrames, kUiMapsReleaseFrames,\n"
TAKE_WORDS = '"on foot maps sharp: the layer TAKES the 2D screen at frame=%llu: no world camera named its source for %u frames in a row "'
HAND_ARGS = ("        static_cast<unsigned long long>(eyesLayerOnly), static_cast<unsigned long long>(eyesNotEmpty), why ? why : \"?\");\n")
WINDOW_GATE = 'world ? "world" : "panel"'
WINDOW_RELEASES = '"releases=%u screen-takes=%u recognised=%u door-layer-only=%u door-not-empty=%u not-live-frames=%u screen-draws=%u",'
WINDOW_ARGS = "w.releases, w.screenTakes, w.recognised, w.doorLayerOnly, w.doorNotEmpty, w.notLive, w.screenDraws);\n"
WINDOW_RESET = "    void reset() { *this = UiMapsWindow{}; }\n"
OFF_PANEL = '        wasPanel ? "; the layer held a panel at that moment and the gate now decides it again" : "");\n'


def drop(old):
    return [(old, "")]


MUTANTS = [
    # ---- R1: the key ----------------------------------------------------------------------------------------------------------
    M("key-case-sensitive", "R1b", drop(LOWER), "the value is compared as typed, so ON is not on"),
    M("key-prefix-only", "R1d", [(KEY_END, "    return UiMapsKey::On;\n")], "\"onn\", \"on \" and \"on\\n\" read as on"),
    M("key-absent-is-on", "R1e", [(KEY_NULL, "    if (!text) return UiMapsKey::On;\n")], "no value switches the gate on"),
    M("key-mismatch-is-on", "R1c", [(KEY_MISMATCH, "        if (c != *want) return UiMapsKey::On;\n")], "a value that is not \"on\" reads as on"),
    M("key-names-swapped", "R1f", [(KEY_NAME, '{ return key == UiMapsKey::On ? "off" : "on"; }')], "the key's names are swapped"),
    # ---- R2: the step ----------------------------------------------------------------------------------------------------------
    M("hold-one-frame", "R2b", [(HOLD_CONST, "constexpr uint32_t kUiMapsHoldFrames = 1;\n")], "one named frame holds the panel as the world"),
    M("hold-three-frames", "R2c", [(HOLD_CONST, "constexpr uint32_t kUiMapsHoldFrames = 3;\n")], "the world is held on the third named frame"),
    M("release-one-frame", "R2i", [(RELEASE_CONST, "constexpr uint32_t kUiMapsReleaseFrames = 1;\n")], "one unnamed frame releases the panel"),
    M("release-two-frames", "R2j", [(RELEASE_CONST, "constexpr uint32_t kUiMapsReleaseFrames = 2;\n")], "two unnamed frames release the panel"),
    M("release-four-frames", "R2k", [(RELEASE_CONST, "constexpr uint32_t kUiMapsReleaseFrames = 4;\n")], "the panel is released on the fourth unnamed frame"),
    M("unnamed-keeps-the-hold-run", "R2e", [(PANEL_UNNAMED, "        if (!named) {\n            return UiMapsEdge::None;\n        }\n")],
      "an unnamed frame does not restart the run toward holding"),
    M("named-keeps-the-release-run", "R2m", [(WORLD_NAMED, "    if (named) {\n        return UiMapsEdge::None;\n    }\n")],
      "a named frame does not restart the run toward releasing"),
    M("hold-edge-not-reported", "R2c", [(HOLD_EDGE, "            g.world = true;\n            g.run = 0;\n            return UiMapsEdge::None;\n")], "the hold is not an edge"),
    M("release-edge-not-reported", "R2k", [(RELEASE_EDGE, "        g.world = false;\n        g.run = 0;\n        return UiMapsEdge::None;\n")],
      "the release is not an edge"),
    M("release-reported-as-hold", "R2k", [(RELEASE_EDGE, "        g.world = false;\n        g.run = 0;\n        return UiMapsEdge::Hold;\n")],
      "the release edge is reported as a hold"),
    M("release-does-not-release", "R2k", [(RELEASE_EDGE, "        g.run = 0;\n        return UiMapsEdge::Release;\n")], "the release edge leaves the world held"),
    M("hold-keeps-the-run", "R2c", [(HOLD_EDGE, "            g.world = true;\n            return UiMapsEdge::Hold;\n")], "a hold leaves its run counting"),
    # ---- R3: the constants -----------------------------------------------------------------------------------------------------
    M("hold-constant-one", "R3a", [(HOLD_CONST, "constexpr uint32_t kUiMapsHoldFrames = 1;\n")], "the hold count is one frame"),
    M("hold-constant-three", "R3a", [(HOLD_CONST, "constexpr uint32_t kUiMapsHoldFrames = 3;\n")], "the hold count is three frames"),
    M("release-constant-two", "R3b", [(RELEASE_CONST, "constexpr uint32_t kUiMapsReleaseFrames = 2;\n")], "the release count is two frames"),
    M("release-constant-four", "R3b", [(RELEASE_CONST, "constexpr uint32_t kUiMapsReleaseFrames = 4;\n")], "the release count is four frames"),
    # (R3c, letting go is slower than taking hold, follows from R3a and R3b: no edit of the header breaks it alone.)
    # ---- R4: flight 1's runs ---------------------------------------------------------------------------------------------------
    M("replay-hold-three", "R4c", [(HOLD_CONST, "constexpr uint32_t kUiMapsHoldFrames = 3;\n")], "the replay's world is held a frame late"),
    M("replay-release-two", "R4d", [(RELEASE_CONST, "constexpr uint32_t kUiMapsReleaseFrames = 2;\n")], "the replay's first map is released a frame early"),
    M("replay-release-four", "R4d", [(RELEASE_CONST, "constexpr uint32_t kUiMapsReleaseFrames = 4;\n")], "the replay's first map is released a frame late"),
    # ---- R5: the combine -------------------------------------------------------------------------------------------------------
    M("combine-ignores-the-key", "R5b", [(COMBINE, "    return g.world;\n")], "with the key off the gate is the step's alone"),
    M("combine-journal-only", "R5b", [(COMBINE, "    return decide ? g.world : byJournal;\n")], "with the key off the depth is not asked"),
    M("combine-depth-only", "R5b", [(COMBINE, "    return decide ? g.world : byDepth;\n")], "with the key off the journal is not asked"),
    M("combine-and", "R5b", [(COMBINE, "    return decide ? g.world : (byJournal && byDepth);\n")], "with the key off both signals are needed"),
    M("combine-inverted", "R5b", [(COMBINE, "    return decide ? (byJournal || byDepth) : g.world;\n")], "the key's two answers are swapped"),
    M("combine-naming-or-journal", "R5c", [(COMBINE, "    return decide ? (g.world || byJournal) : (byJournal || byDepth);\n")], "decided by naming, the journal still holds the world"),
    M("combine-naming-or-depth", "R5c", [(COMBINE, "    return decide ? (g.world || byDepth) : (byJournal || byDepth);\n")], "decided by naming, the depth still holds the world"),
    # ---- R6: the carry ---------------------------------------------------------------------------------------------------------
    M("carry-ignores-today", "R6a", [(CARRY, "    g.world = false;\n    g.run = 0;\n")], "the switch in always starts released"),
    M("carry-always-world", "R6b", [(CARRY, "    g.world = true;\n    g.run = 0;\n")], "the switch in always starts held"),
    M("carry-keeps-the-run", "R6a", [(CARRY, "    g.world = heldNow;\n")], "the switch in keeps the old run"),
    # ---- R7: the door ----------------------------------------------------------------------------------------------------------
    M("door-sequence-zero-matches", "R7a", [(DOOR_ZERO, "    if (takenSeq != seq) return UiMapsDoor::No;\n")], "a zero mark matches the first frame"),
    M("door-ignores-the-sequence", "R7b", [(DOOR_ZERO, "    if (seq == 0) return UiMapsDoor::No;\n")], "a take marked in another sequence still counts"),
    M("door-later-sequences-match", "R7b", [(DOOR_ZERO, "    if (seq == 0 || takenSeq > seq) return UiMapsDoor::No;\n")], "an earlier sequence's take still counts"),
    M("door-strict-count", "R7c", [(DOOR_COUNT, "    return eyeDraws < takenDraws ? UiMapsDoor::Yes : UiMapsDoor::NotEmpty;\n")], "every draw taken is not enough"),
    M("door-lenient-count", "R7d", [(DOOR_COUNT, "    return eyeDraws <= takenDraws + 1 ? UiMapsDoor::Yes : UiMapsDoor::NotEmpty;\n")], "one draw the layer did not take is allowed"),
    M("door-reversed-count", "R7d", [(DOOR_COUNT, "    return eyeDraws >= takenDraws ? UiMapsDoor::Yes : UiMapsDoor::NotEmpty;\n")], "the count comparison is reversed"),
    M("door-always-yes", "R7d", [(DOOR_COUNT, "    return UiMapsDoor::Yes;\n")], "an eye with something else in it is still the layer's"),
    M("door-never-yes", "R7c", [(DOOR_COUNT, "    return UiMapsDoor::NotEmpty;\n")], "no eye is ever the layer's"),
    # ---- R8: the lines ---------------------------------------------------------------------------------------------------------
    M("on-line-counts-swapped", "R8a", [(ON_COUNTS, "        static_cast<unsigned long long>(frame), kUiMapsReleaseFrames, kUiMapsHoldFrames,\n")], "the ON line says the counts the other way round"),
    M("take-line-says-hands-back", "R8e", [(TAKE_WORDS, TAKE_WORDS.replace("the layer TAKES the 2D screen", "the layer HANDS BACK the 2D screen"))], "the take line says it is a hand-back"),
    M("hand-back-counts-swapped", "R8g", [(HAND_ARGS, "        static_cast<unsigned long long>(eyesNotEmpty), static_cast<unsigned long long>(eyesLayerOnly), why ? why : \"?\");\n")],
      "the hand-back line swaps the door's two counts"),
    M("off-line-always-says-panel", "R8d", [(OFF_PANEL, '        "; the layer held a panel at that moment and the gate now decides it again");\n')], "the OFF line always says the layer held a panel"),
    # ---- R9: the 5 s window ----------------------------------------------------------------------------------------------------
    M("window-gate-inverted", "R9a", [(WINDOW_GATE, 'world ? "panel" : "world"')], "the window says panel for the world"),
    M("window-counter-missing", "R9a", [(WINDOW_RELEASES, '"screen-takes=%u recognised=%u door-layer-only=%u door-not-empty=%u not-live-frames=%u screen-draws=%u",'),
                                        (WINDOW_ARGS, "w.screenTakes, w.recognised, w.doorLayerOnly, w.doorNotEmpty, w.notLive, w.screenDraws);\n")],
      "the window leaves the releases out"),
    M("window-reset-incomplete", "R9c", [(WINDOW_RESET, "    void reset() { frames = 0; }\n")], "a window's reset zeroes one counter"),
    # The composites the decision saw (design doc 8.10): the reader sets screen-draws against screen-takes, so it must be there, last, and its own.
    M("window-draws-missing", "R9a", [(WINDOW_RELEASES, '"releases=%u screen-takes=%u recognised=%u door-layer-only=%u door-not-empty=%u not-live-frames=%u",'),
                                      (WINDOW_ARGS, "w.releases, w.screenTakes, w.recognised, w.doorLayerOnly, w.doorNotEmpty, w.notLive);\n")],
      "the window leaves screen-draws out, so the reader cannot tell a cockpit from composites drawn and not taken"),
    M("window-draws-before-not-live", "R9a", [(WINDOW_RELEASES, '"releases=%u screen-takes=%u recognised=%u door-layer-only=%u door-not-empty=%u screen-draws=%u not-live-frames=%u",'),
                                              (WINDOW_ARGS, "w.releases, w.screenTakes, w.recognised, w.doorLayerOnly, w.doorNotEmpty, w.screenDraws, w.notLive);\n")],
      "screen-draws moves in front of not-live-frames, which the reader's pattern ends with"),
    M("window-draws-prints-takes", "R9b", [(WINDOW_ARGS, "w.releases, w.screenTakes, w.recognised, w.doorLayerOnly, w.doorNotEmpty, w.notLive, w.screenTakes);\n")],
      "screen-draws prints the takes, so a window never shows a composite that was not taken"),
    M("window-reset-keeps-draws", "R9c", [(WINDOW_RESET, "    void reset() { const uint32_t keep = screenDraws; *this = UiMapsWindow{}; screenDraws = keep; }\n")],
      "a window's reset keeps the composites it counted, so every window after the first overstates them"),
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


def parse_fail(output):
    """The text after the last 'FAIL: ' line of the rig's output, or None when it printed none."""
    found = None
    for line in output.splitlines():
        if line.startswith("FAIL: "):
            found = line[len("FAIL: "):]
    return found


def rig_cases(rig_text):
    return set(re.findall(r'\{"(R\d+)", case\w+\}', rig_text))


def rig_label_exists(rig_text, prefix):
    return ('"%s:' % prefix) in rig_text


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
    """cl.exe by absolute path (Windows looks a bare name up on THIS process's PATH, not on the env passed), with the environment
    it needs: this one if cl is on PATH, else the one vcvars64.bat makes, found with vswhere as build.bat does."""

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


def build_rig(tc, header_dir, outdir):
    """Compile the rig against the header in header_dir (and only there); (exit code, output)."""
    exe = outdir / "rig.exe"
    cmd = [tc.cl] + CL_FLAGS + ["/I" + str(header_dir), "/Fo" + str(outdir) + os.sep, "/Fe" + str(exe), str(RIG)]
    done = subprocess.run(cmd, capture_output=True, text=True, env=tc.env, errors="replace", cwd=str(outdir))
    return done.returncode, (done.stdout + done.stderr), exe


def run_rig(exe, rule, tc):
    """(outcome, detail) of one run of the rig: 'pass', 'fail' (detail = the label), 'crash', 'timeout'."""
    cmd = [str(exe), "--self-test"] + (["--only", rule] if rule else [])
    try:
        done = subprocess.run(cmd, capture_output=True, text=True, env=tc.env, errors="replace", timeout=RUN_TIMEOUT)
    except subprocess.TimeoutExpired:
        return "timeout", "no result within %d s" % RUN_TIMEOUT
    if done.returncode == 0:
        return "pass", ""
    label = parse_fail(done.stdout + "\n" + done.stderr)
    if label is None:
        return "crash", "exit 0x%08X" % (done.returncode & 0xFFFFFFFF)
    return "fail", label


# ---- the run ---------------------------------------------------------------------------------------------------------------------
def select(only):
    if not only:
        return MUTANTS
    wanted = [n for n in only.split(",") if n]
    unknown = [n for n in wanted if n not in {m.name for m in MUTANTS}]
    if unknown:
        raise ValueError("no such mutation: " + ", ".join(unknown))
    return [m for m in MUTANTS if m.name in wanted]


def plan_text(mutants):
    lines = ["%d mutation(s); each header copy is compiled with the rig in a temp directory outside the repo and run on its rule's cases:" % len(mutants)]
    for m in mutants:
        lines.append("  %-30s rule %-4s caught by %-14s %s" % (m.name, m.rule, "/".join(m.caught), m.why))
    lines.append("cl.exe flags: " + " ".join(CL_FLAGS))
    return "\n".join(lines)


def run_all(only=None, jobs=None, keep=False, dry_run=False, out=sys.stdout):
    mutants = select(only)
    if dry_run:
        print(plan_text(mutants), file=out)
        print("dry-run: nothing compiled, started or written", file=out)
        return 0
    prod = read_source(PROD)
    tc = Toolchain()
    work = Path(tempfile.mkdtemp(prefix="on_foot_maps_mutants_"))
    try:
        def build_and_run(name, header_text, rule):
            d = work / name
            d.mkdir()
            (d / "ui_maps_math.h").write_text(header_text, encoding="utf-8", newline="\n")
            code, text, exe = build_rig(tc, d, d)
            if code != 0:
                return "nocompile", text.strip().splitlines()[-1] if text.strip() else ""
            return run_rig(exe, rule, tc)

        control, detail = build_and_run("control", prod, None)
        print("control (the unmutated header, every case): %s %s" % (control, detail), file=out)
        if control != "pass":
            print("the rig does not pass on the unmutated header when built this way; nothing below means anything", file=out)
            return 1

        def one(m):
            try:
                text = apply_edits(prod, m.edits, m.name)
            except ValueError as error:
                return m, "badedit", str(error)
            outcome, detail = build_and_run(m.name, text, m.rule)
            return m, outcome, detail

        results = []
        with concurrent.futures.ThreadPoolExecutor(max_workers=jobs or min(8, os.cpu_count() or 2)) as pool:
            for m, outcome, detail in pool.map(one, mutants):
                if outcome == "fail" and any(detail.startswith(p + ":") for p in m.caught):
                    verdict = "caught"
                elif outcome == "fail":
                    verdict = "OTHER"
                else:
                    verdict = {"pass": "SURVIVED", "crash": "CRASH", "timeout": "TIMEOUT", "nocompile": "NOCOMPILE", "badedit": "BADEDIT"}[outcome]
                results.append((m, verdict, detail))
                print("%-9s %-30s %-4s %s" % (verdict, m.name, m.rule, detail[:150]), file=out)
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


# ---- the self-test ------------------------------------------------------------------------------------------------------------------
def self_test():
    failures = []

    def check(ok, what):
        if not ok:
            failures.append(what)

    # the tool's own pieces
    check(parse_fail("a\nFAIL: R1c: x\nb\n") == "R1c: x" and parse_fail("PASS: 3\n") is None, "parse_fail reads the last FAIL: line")
    check(apply_edits("a b c", [("b", "B")]) == "a B c", "apply_edits applies one edit")
    for bad, why in ((("a b b", [("b", "B")]), "a repeated anchor"), (("a b", [("x", "y")]), "a missing anchor"), (("a b", [("b", "b")]), "a no-op edit")):
        try:
            apply_edits(*bad)
            check(False, "apply_edits refuses " + why)
        except ValueError:
            pass
    joined = label_block(":rig_a\nx ^\n y\nexit /b 0\n:rig_b\nz\n", ":rig_a")
    check(joined is not None and [" ".join(l.split()) for l in joined] == ["x y", "exit /b 0"], "label_block joins continuations and stops at the next label")
    check(rig_cases('{"R1", caseR1}, {"R10", caseR10}') == {"R1", "R10"}, "rig_cases reads the case table")

    # every mutation against the header as it is now, and against the rig
    prod = read_source(PROD)
    rig = read_source(RIG)
    cases = rig_cases(rig)
    names = [m.name for m in MUTANTS]
    check(len(names) == len(set(names)), "mutation names are unique")
    check(len(MUTANTS) >= 40, "the mutation list did not shrink (%d)" % len(MUTANTS))
    for m in MUTANTS:
        try:
            mutated = apply_edits(prod, m.edits, m.name)
            check(mutated != prod, "%s changes the header" % m.name)
        except ValueError as error:
            failures.append(str(error))
        for p in m.caught:
            check(rig_label_exists(rig, p), "%s: the rig has no check labelled %s" % (m.name, p))
        check(m.rule in cases, "%s: the rig has no case %s" % (m.name, m.rule))
    rules = {m.rule for m in MUTANTS}
    check(rules == cases, "every rule of the rig has a mutation, and only rules of the rig: %s vs %s" % (sorted(rules), sorted(cases)))

    # build.bat compiles the rig the way this tool does, and runs this tool's self-test
    bat = BUILD_BAT.read_bytes().decode("utf-8", errors="replace")
    block = label_block(bat, RIG_LABEL)
    check(block is not None, "build.bat has %s" % RIG_LABEL)
    if block:
        text = "\n".join(block)
        cl = next((l for l in block if l.strip().lower().startswith("cl.exe")), "")
        for flag in CL_FLAGS:
            check(flag in cl, "build.bat's rig compile has %s" % flag)
        check("tools\\on_foot_maps_test\\on_foot_maps_test.cpp" in cl, "build.bat compiles the rig")
        check('/I"src\\d3d11"' in cl, "build.bat's rig compile finds the header through /I src\\d3d11 (the mutated copy is found the same way)")
        check("on_foot_maps_test.exe\" --dry-run" in text and "on_foot_maps_test.exe\" --self-test \"%ROOT%\"" in text, "build.bat runs the rig's --dry-run and --self-test with the repo root")
        check("mutants.py\" --self-test" in text, "build.bat runs this tool's --self-test")

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
    print("PASS: on_foot_maps_test mutants.py self-test (%d mutations over %d rules, every anchor found once, build.bat wired)" % (len(MUTANTS), len(rules)))
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
    args = parser.parse_args(argv)
    if args.self_test:
        return self_test()
    if args.list:
        print(plan_text(MUTANTS))
        return 0
    if args.run:
        return run_all(only=args.only, jobs=args.jobs or None, keep=args.keep, dry_run=args.dry_run)
    parser.print_help()
    return 2


if __name__ == "__main__":
    sys.exit(main())
