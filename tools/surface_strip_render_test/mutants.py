#!/usr/bin/env python3
"""The mutation proof for tools\\surface_strip_render_test: the rig fails when a rule that decides which way the curved intro picture faces is flipped.

The rig (surface_strip_render_test.cpp) draws the PRODUCTION surface strip through the intro composite's own vertex shader on a WARP device and
asks whether left stays left and up stays up; every check it makes carries a label "R<case>.<what>" (its header says which case is which). A rig
that passes proves little until it is seen to FAIL on a build that breaks the rule it pins. This tool does that: for each mutation below it copies
the production source the rule lives in (src\\d3d11\\panel_curve.cpp, intro_panel.cpp, intro_curve.cpp, or the pure header intro_curve_math.h) into a
temp directory OUTSIDE the repo, applies one textual edit (or a few that belong together), recompiles what includes it, links the rig with the
unmutated rest, runs it, and requires it to fail on a check of the case that belongs to the rule (a FAIL label that starts with one of the
mutation's case ids and a dot). Nothing is written inside the repo; the temp directory is removed at the end.

  python tools\\surface_strip_render_test\\mutants.py --self-test   text only: every anchor is found exactly once in the source as it is now, every
                                                                 case named is in the rig, every case of the rig has a mutation, and build.bat
                                                                 compiles the rig the way this tool does (add --build-bat PATH for another copy)
  python tools\\surface_strip_render_test\\mutants.py --run [--only a,b] [--jobs N] [--keep] [--verbose]
  python tools\\surface_strip_render_test\\mutants.py --list
  python tools\\surface_strip_render_test\\mutants.py --run --dry-run   the plan; writes nothing, starts nothing

--run needs the MSVC toolchain (cl.exe on PATH, or Visual Studio found with vswhere as build.bat does); build\\gen is added to the include path
when there is one. The rig is quick, so --run takes a few minutes. --self-test runs in build.bat's rig and is what keeps an edit of the production
code from silently orphaning a mutation: if an anchor stops matching, the build fails and this file says which.
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
SRC = ROOT / "src" / "d3d11"
# The production sources a rule can live in: "panel" the strip, "movie" intro_panel.cpp, "splash" intro_curve.cpp, "math" the pure header both read
# the placement's direction with (it is included by the two modules and by the rig, so a mutated copy rebuilds all three).
FILES = {"panel": SRC / "panel_curve.cpp", "movie": SRC / "intro_panel.cpp", "splash": SRC / "intro_curve.cpp", "math": SRC / "intro_curve_math.h"}
CPP_KEYS = ("panel", "movie", "splash")
RIG = HERE / "surface_strip_render_test.cpp"
BUILD_BAT = ROOT / "build.bat"
RIG_LABEL = ":rig_surface_strip_render_test"
COMMON = [ROOT / "src" / "common" / n for n in ("config.cpp", "log.cpp", "guard.cpp", "proxy.cpp")]
LIBS = ["user32.lib", "version.lib", "d3dcompiler.lib"]

# How build.bat compiles the rig; --self-test checks that the label still says the same.
CL_FLAGS = ["/nologo", "/O2", "/MT", "/std:c++17", "/EHsc", "/W4", "/DWIN32_LEAN_AND_MEAN", "/DNOMINMAX", "/D_CRT_SECURE_NO_WARNINGS"]
RUN_TIMEOUT = 180.0


class Mutant:
    def __init__(self, name, caught, edits, why, file):
        self.name = name
        self.caught = tuple([caught] if isinstance(caught, str) else caught)   # case ids whose checks must report it
        self.edits = list(edits)                                                # (old, new) pairs, applied in order, to `file`
        self.why = why
        self.file = file                                                        # a key of FILES


def M(name, caught, file, edits, why):
    return Mutant(name, caught, edits, why, file)


# ---- the anchors: text of the production sources, verbatim (the self-test finds each exactly once in the file it edits) ------------------------
# panel_curve.cpp: the strip
STRIP_U = "        const float u = reverseU ? (1.0f - x) * 0.5f : (x + 1.0f) * 0.5f;\n"
STRIP_BOTTOM = "        vb[i] = Vertex{bx, -1.0f, bz, u, 1.0f};\n"
STRIP_TOP = "        vb[(n + 1) + i] = Vertex{bx, 1.0f, bz, u, 0.0f};\n"
WIND_1 = "        ib[w++] = bl; ib[w++] = tr; ib[w++] = br;\n"
BEND_X = "    *xOut = sinf(theta) / k;\n"
S_FILL = "    fillStrip(n, detail::g_panelCurveCurvature, -toward, gain, reverseU, vb, ib);\n"
ONFOOT_FILL = "    fillStrip(n, detail::g_panelCurveCurvature, g_sign, activeGain(), false, vb, ib);   // the screen's u runs with x: its +x runs to the viewer's right\n"
S_RS_CULL = "            if (game.CullMode != D3D11_CULL_NONE) {\n"
# intro_panel.cpp: the movie's flag
M_REVERSE_SET = "                g_stripReverseU = reverseU;\n"
M_DIR_REVERSE = "                        reverseU = dir == IntroXDir::kLeft;\n"
# intro_curve.cpp: the splash's flag
S_PAIR_REVERSE = "    e.reverseU = w.xDir == IntroXDir::kLeft;\n"
S_ARMED_REVERSE = "    g_armedReverseU = e->reverseU;\n"
S_ACCESSOR = "bool introCurveReverseU() {\n    return g_armedReverseU;\n}\n"
# intro_curve_math.h: the direction rule both modules read the placement with
X_RULE = "    return minor < 0.0 ? IntroXDir::kLeft : IntroXDir::kRight;\n"

MUTANTS = [
    # ---- the strip's own texture coordinate and geometry: the picture must be upright through every placement ---------------------------------------
    M("strip-u-ignores-the-flag", ("R2", "R3"), "panel", [(STRIP_U, "        const float u = (x + 1.0f) * 0.5f;\n")],
      "u always runs with x: a placement whose +x runs left is mirrored (the bug the first flight showed)"),
    M("strip-u-flag-inverted", ("R1", "R2", "R3"), "panel", [(STRIP_U, "        const float u = reverseU ? (x + 1.0f) * 0.5f : (1.0f - x) * 0.5f;\n")],
      "the generator runs u against x when told to run it with x: the on-foot strip is mirrored too"),
    M("surface-flag-inverted-at-the-build", ("R2", "R3"), "panel", [(S_FILL, "    fillStrip(n, detail::g_panelCurveCurvature, -toward, gain, !reverseU, vb, ib);\n")],
      "the surface builds the strip the other way round from the one asked for"),
    M("surface-flag-never-reaches-the-build", ("R2", "R3"), "panel", [(S_FILL, "    fillStrip(n, detail::g_panelCurveCurvature, -toward, gain, false, vb, ib);\n")],
      "the surface strip always runs u with x"),
    M("onfoot-u-reversed", "R1", "panel", [(ONFOOT_FILL, ONFOOT_FILL.replace("activeGain(), false,", "activeGain(), true,"))], "the screen's own strip runs u against x"),
    M("strip-v-flipped", ("R1", "R2", "R3"), "panel", [(STRIP_BOTTOM, STRIP_BOTTOM.replace("u, 1.0f}", "u, 0.0f}")), (STRIP_TOP, STRIP_TOP.replace("u, 0.0f}", "u, 1.0f}"))],
      "v runs the other way: the picture is upside down"),
    M("bend-x-negated", ("R1", "R2", "R3"), "panel", [(BEND_X, "    *xOut = -sinf(theta) / k;\n")], "the bent x runs the other way: the picture is mirrored"),
    M("cull-off-never-applied", ("R2", "R3"), "panel", [(S_RS_CULL, "            if (false) {\n")],
      "the surface strip is drawn with the game's own cull, and a placement whose +x runs left faces it away: a blank picture"),
    M("winding-first-flipped", "R1", "panel", [(WIND_1, "        ib[w++] = bl; ib[w++] = br; ib[w++] = tr;\n")],
      "the first triangle of each quad is wound the other way: the on-foot strip, drawn with the game's cull, loses half its triangles"),
    # ---- the movie's flag ----------------------------------------------------------------------------------------------------------------------
    M("movie-flag-constant-false", "R2", "movie", [(M_REVERSE_SET, "                g_stripReverseU = false;\n")], "the movie's u never runs against x: the movie comes out mirrored"),
    M("movie-flag-constant-true", "R4", "movie", [(M_REVERSE_SET, "                g_stripReverseU = true;\n")],
      "the movie's u always runs against x, a placement whose +x runs right included"),
    M("movie-flag-inverted", ("R2", "R4"), "movie", [(M_DIR_REVERSE, "                        reverseU = dir == IntroXDir::kRight;\n")],
      "the movie's u runs with x when +x runs left, and against it when it runs right"),
    # ---- the splash's flag ---------------------------------------------------------------------------------------------------------------------
    M("splash-pair-flag-constant-false", "R3", "splash", [(S_PAIR_REVERSE, "    e.reverseU = false;\n")], "the splash's u never runs against x: the splash comes out mirrored"),
    M("splash-pair-flag-constant-true", "R4", "splash", [(S_PAIR_REVERSE, "    e.reverseU = true;\n")], "the splash's u always runs against x, a placement whose +x runs right included"),
    M("splash-pair-flag-inverted", ("R3", "R4"), "splash", [(S_PAIR_REVERSE, "    e.reverseU = w.xDir == IntroXDir::kRight;\n")], "the splash's u runs with x when +x runs left, and against it when it runs right"),
    M("splash-armed-flag-ignored", "R3", "splash", [(S_ARMED_REVERSE, "    g_armedReverseU = false;\n")], "the pair's flag never reaches the armed draw"),
    M("splash-accessor-constant-false", "R3", "splash", [(S_ACCESSOR, "bool introCurveReverseU() {\n    return false;\n}\n")], "the accessor the wiring reads always says u runs with x"),
    # ---- the direction rule both modules read the placement with -------------------------------------------------------------------------------------
    M("xdir-rule-sign-flipped", ("R2", "R3", "R4"), "math", [(X_RULE, "    return minor < 0.0 ? IntroXDir::kRight : IntroXDir::kLeft;\n")],
      "the rule says a placement whose +x runs left runs right, and the reverse: both modules mirror"),
    M("xdir-rule-always-left", "R4", "math", [(X_RULE, "    return IntroXDir::kLeft;\n")], "the rule never says a placement runs right"),
    M("xdir-rule-always-right", ("R2", "R3"), "math", [(X_RULE, "    return IntroXDir::kRight;\n")], "the rule never says a placement runs left"),
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
    """Every check label the rig printed on a 'FAIL: R<case>.<what>' line (the text up to ' -- ', ' [' or the end of the line); the rig's closing
    'FAIL: surface strip render: n of m checks failed' line is not a label."""
    labels = []
    for line in output.splitlines():
        if line.startswith("FAIL: "):
            label = re.split(r" -- | \[", line[len("FAIL: "):], 1)[0].strip()
            if re.match(r"R\d+\.", label):
                labels.append(label)
    return labels


def rig_cases(rig_text):
    """The cases the rig labels its checks with: R<digit> from a quoted "R<digit>.<what>" label, R0 (the device) left out."""
    return {c for c in re.findall(r'"(R\d)\.', rig_text) if c != "R0"}


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


def find_gen():
    """The generated-headers directory: this checkout's build\\gen, else the main checkout's (a worktree is .claude\\worktrees\\<name>)."""
    for candidate in (ROOT / "build" / "gen", ROOT.parents[2] / "build" / "gen" if len(ROOT.parents) > 2 else None):
        if candidate and candidate.is_dir():
            return candidate
    return None


# ---- the toolchain -----------------------------------------------------------------------------------------------------------
class Toolchain:
    """cl.exe and link.exe by absolute path (Windows looks a bare name up on THIS process's PATH, not on the env passed), with the
    environment they need: this one if cl is on PATH, else the one vcvars64.bat makes, found with vswhere as build.bat does."""

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
        self.link = str(Path(found).with_name("link.exe"))


def compile_obj(tc, source, outdir, includes, obj_name=None):
    """Compile one source to outdir\\<obj_name, else the source's stem>.obj; (exit code, output, obj path)."""
    obj = outdir / ((obj_name or Path(source).stem) + ".obj")
    cmd = [tc.cl] + CL_FLAGS + ["/c"] + ["/I" + str(i) for i in includes] + ["/Fo" + str(obj), str(source)]
    done = subprocess.run(cmd, capture_output=True, text=True, env=tc.env, errors="replace", cwd=str(outdir))
    return done.returncode, (done.stdout + done.stderr), obj


def link_exe(tc, objs, exe, outdir):
    cmd = [tc.link, "/nologo", "/INCREMENTAL:NO", "/OUT:" + str(exe)] + [str(o) for o in objs] + LIBS
    done = subprocess.run(cmd, capture_output=True, text=True, env=tc.env, errors="replace", cwd=str(outdir))
    return done.returncode, (done.stdout + done.stderr)


def run_rig(exe, tc):
    """(outcome, labels, tail) of one run of the rig: 'pass', 'fail', 'crash', 'timeout'."""
    try:
        done = subprocess.run([str(exe), "--self-test"], capture_output=True, text=True, env=tc.env, errors="replace", timeout=RUN_TIMEOUT)
    except subprocess.TimeoutExpired:
        return "timeout", [], "no result within %d s" % RUN_TIMEOUT
    out = done.stdout + "\n" + done.stderr
    if done.returncode == 0:
        return "pass", [], ""
    labels = fail_labels(out)
    if not labels:
        return "crash", [], "exit 0x%08X" % (done.returncode & 0xFFFFFFFF)
    return "fail", labels, ""


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
    lines = ["%d mutation(s); each is compiled with the rig in a temp directory outside the repo and must fail on a case named:" % len(mutants)]
    for m in mutants:
        lines.append("  %-38s %-6s caught by %-12s %s" % (m.name, m.file, "/".join(m.caught), m.why))
    lines.append("cl.exe flags: " + " ".join(CL_FLAGS))
    return "\n".join(lines)


def run_all(only=None, jobs=None, keep=False, dry_run=False, verbose=False, out=sys.stdout):
    mutants = select(only)
    if dry_run:
        print(plan_text(mutants), file=out)
        print("dry-run: nothing compiled, started or written", file=out)
        return 0
    gen = find_gen()
    tc = Toolchain()
    sources = {k: read_source(p) for k, p in FILES.items()}
    work = Path(tempfile.mkdtemp(prefix="ssm_"))
    workers = jobs or min(4, os.cpu_count() or 2)
    try:
        base_inc = ([gen] if gen else []) + [SRC]
        # The control: every source as it is, built the way every mutation is; the control's objects are what a mutation links its unmutated rest from.
        control = work / "k"
        control.mkdir()
        ctrl_obj = {}
        jobs_list = [("rig", RIG), ("panel", FILES["panel"]), ("movie", FILES["movie"]), ("splash", FILES["splash"])] + [("common%d" % i, c) for i, c in enumerate(COMMON)]
        with concurrent.futures.ThreadPoolExecutor(max_workers=max(workers, 2)) as pool:
            futures = {k: pool.submit(compile_obj, tc, src, control, base_inc, k) for k, src in jobs_list}
            built = {k: f.result() for k, f in futures.items()}
        for k, (code, text, obj) in built.items():
            if code != 0:
                print("control: nocompile %s\n%s" % (k, text.strip()[-1500:]), file=out)
                return 1
            ctrl_obj[k] = obj
        common_objs = [ctrl_obj["common%d" % i] for i in range(len(COMMON))]
        exe0 = control / "rig.exe"
        code, text = link_exe(tc, [ctrl_obj["rig"], ctrl_obj["panel"], ctrl_obj["movie"], ctrl_obj["splash"]] + common_objs, exe0, control)
        if code != 0:
            print("control: nolink\n" + text.strip()[-1500:], file=out)
            return 1
        outcome, labels, tail = run_rig(exe0, tc)
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
            inc = [d] + base_inc
            objs = dict(ctrl_obj)
            if m.file == "math":
                # the header is included by the two modules and by the rig: all three are rebuilt against the mutated copy, which sits beside the
                # modules' own copies (a quoted include finds it first) and first on the rig's include path
                (d / FILES["math"].name).write_text(mutated, encoding="utf-8", newline="\n")
                for key in ("movie", "splash"):
                    (d / FILES[key].name).write_text(sources[key], encoding="utf-8", newline="\n")
                    code, text, objs[key] = compile_obj(tc, d / FILES[key].name, d, inc, key)
                    if code != 0:
                        return m, "nocompile", (text.strip().splitlines() or [""])[-1]
                code, text, objs["rig"] = compile_obj(tc, RIG, d, inc, "rig")
                if code != 0:
                    return m, "nocompile", (text.strip().splitlines() or [""])[-1]
            else:
                (d / FILES[m.file].name).write_text(mutated, encoding="utf-8", newline="\n")
                code, text, objs[m.file] = compile_obj(tc, d / FILES[m.file].name, d, inc, m.file)
                if code != 0:
                    return m, "nocompile", (text.strip().splitlines() or [""])[-1]
            exe = d / "rig.exe"
            code, text = link_exe(tc, [objs["rig"], objs["panel"], objs["movie"], objs["splash"]] + common_objs, exe, d)
            if code != 0:
                return m, "nolink", (text.strip().splitlines() or [""])[-1]
            outcome, labels, tail = run_rig(exe, tc)
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
                print("%-9s %-38s %-12s %s" % (verdict, m.name, "/".join(m.caught)[:12], detail if verbose else detail[:150]), file=out)
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
    check(fail_labels("a\nFAIL: R2.upright -- x [y]\nFAIL: R4.movie-control-mirrored\nFAIL: surface strip render: 3 of 199 checks failed, 2 distinct\n")
          == ["R2.upright", "R4.movie-control-mirrored"], "fail_labels reads the label of every check's FAIL: line, and not the closing summary")
    check(fail_labels("PASS: 199 surface strip render checks\n") == [], "fail_labels reads nothing from a pass")
    check(apply_edits("a b c", [("b", "B")]) == "a B c", "apply_edits applies one edit")
    for bad, why in ((("a b b", [("b", "B")]), "a repeated anchor"), (("a b", [("x", "y")]), "a missing anchor"), (("a b", [("b", "b")]), "a no-op edit")):
        try:
            apply_edits(*bad)
            check(False, "apply_edits refuses " + why)
        except ValueError:
            pass
    joined = label_block(":rig_a\nx ^\n y\nexit /b 0\n:rig_b\nz\n", ":rig_a")
    check(joined is not None and [" ".join(l.split()) for l in joined] == ["x y", "exit /b 0"], "label_block joins continuations and stops at the next label")
    check(rig_cases('check(a, "R0.gpu"); check(b, "R1.upright"); check(c, "R4.movie-control")') == {"R1", "R4"}, "rig_cases reads the cases of the labels and leaves R0 out")

    # every mutation against the sources as they are now, and against the rig
    sources = {k: read_source(p) for k, p in FILES.items()}
    rig = read_source(RIG)
    cases = rig_cases(rig)
    names = [m.name for m in MUTANTS]
    check(len(names) == len(set(names)), "mutation names are unique")
    check(len(MUTANTS) >= 10, "the mutation list did not shrink below 10 (%d)" % len(MUTANTS))
    for m in MUTANTS:
        check(m.file in FILES, "%s edits %s, which this tool does not know" % (m.name, m.file))
        if m.file not in FILES:
            continue
        try:
            mutated = apply_edits(sources[m.file], m.edits, m.name)
            check(mutated != sources[m.file], "%s changes its source" % m.name)
        except ValueError as error:
            failures.append(str(error))
        for c in m.caught:
            check(c in cases, "%s: the rig has no case %s" % (m.name, c))
    covered = {c for m in MUTANTS for c in m.caught}
    check(covered == cases, "every case of the rig has a mutation, and only cases of the rig: %s vs %s" % (sorted(covered), sorted(cases)))
    # the sources the rig links all exist where this tool reads them
    for k, p in FILES.items():
        check(p.is_file(), "%s (%s) exists" % (k, p))
    check('#include "intro_curve_math.h"' in sources["movie"] and '#include "intro_curve_math.h"' in sources["splash"],
          "both modules include intro_curve_math.h (a mutated copy of the header is written beside each)")

    # build.bat compiles the rig the way this tool does, and runs this tool's self-test
    bat = Path(build_bat).read_bytes().decode("utf-8", errors="replace")
    block = label_block(bat, RIG_LABEL)
    check(block is not None, "build.bat has %s" % RIG_LABEL)
    if block:
        text = "\n".join(block)
        cl = next((l for l in block if l.strip().lower().startswith("cl.exe")), "")
        for flag in CL_FLAGS:
            check(flag in cl, "build.bat's rig compile has %s" % flag)
        for src in ("tools\\surface_strip_render_test\\surface_strip_render_test.cpp", "src\\d3d11\\panel_curve.cpp", "src\\d3d11\\intro_panel.cpp",
                    "src\\d3d11\\intro_curve.cpp", "src\\common\\config.cpp", "src\\common\\log.cpp", "src\\common\\guard.cpp", "src\\common\\proxy.cpp"):
            check(src in cl, "build.bat's rig compile has %s (the sources this tool links)" % src)
        check('/I"src\\d3d11"' in cl, "build.bat's rig compile finds the headers through /I src\\d3d11")
        for lib in LIBS:
            check(lib in cl, "build.bat's rig link has %s" % lib)
        check("d3d11.lib" not in cl, "build.bat's rig does not link d3d11.lib (it takes System32's device through src\\common\\system_d3d11.h)")
        check('surface_strip_render_test.exe" --dry-run' in text and 'surface_strip_render_test.exe" --self-test' in text, "build.bat runs the rig's --dry-run and --self-test")
        check('mutants.py" --self-test' in text, "build.bat runs this tool's --self-test")

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
    print("PASS: surface_strip_render_test mutants.py self-test (%d mutations over %d cases, every anchor found once, build.bat wired)" % (len(MUTANTS), len(covered)))
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
        return run_all(only=args.only, jobs=args.jobs, keep=args.keep, dry_run=args.dry_run, verbose=args.verbose)
    parser.print_help()
    return 2


if __name__ == "__main__":
    sys.exit(main())
