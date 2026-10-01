#!/usr/bin/env python3
"""The mutation proof for the jitter phase count (experimental.temporal_aa_jitter_follows_upscale, 2026-10-01).

The rule (src\\common\\temporal_math.h temporalJitterPhaseCount: ceil(8 x (output / render)^2) phases, never fewer than eight, capped),
the flat route's decision (flat_camera_phase.h flatCameraPhaseCount: upstream only, key on) and the phase machine's count
(flat_live_phase.h), the VR eye pass's wiring (native_temporal.cpp: the larger eye's count from the last treat's input and output, a
log line at every change, the key outside every history) and the world route's 5 s token are pinned by four rigs:

  math   tools\\temporal_test\\temporal_test.cpp                header-only: the rule's table, the fixed eight's bit-for-bit sameness
  flat   tools\\flat_temporal_test\\flat_temporal_test.cpp      header-only: the phase machine, the route decision, the Legacy lighting bound
  world  tools\\vr_world_route_test\\vr_world_route_test.cpp    header-only plus source pins: the window line's phases= token
  eye    tools\\native_temporal_test\\native_temporal_test.cpp  native_temporal.cpp itself, flown on WARP (--phase-count-self-test)

A rig that passes proves little until it is seen to FAIL on a source that breaks what it pins. For each mutation below this tool applies
one textual edit (or two, where a format and its argument go together) to ONE production file, builds the rig against the edited copy in
a temp directory OUTSIDE the repo, runs it, and requires it to fail on a check whose label contains the text the mutation names:

  closure  the rig and the include closure of its quoted includes are mirrored under the temp directory with the edited header, and
           the rig is built and run there (a rig's runtime source pins still read the real repo, which they are not about);
  replace  the edited copy of a production .cpp replaces that file in the rig's compile line (everything else is the real tree, found
           through /I, as build.bat compiles it).

Nothing is written inside the repo; the temp directory is removed at the end.

  python tools\\temporal_test\\jitter_phase_mutants.py --self-test   text only: every anchor is found exactly once in its file as it is
                                                                     now, every label named is in its rig, and build.bat compiles the
                                                                     rigs the way this tool does and runs this self-test
  python tools\\temporal_test\\jitter_phase_mutants.py --run [--rig math|flat|world|eye] [--only a,b] [--jobs N] [--keep]
  python tools\\temporal_test\\jitter_phase_mutants.py --list
  python tools\\temporal_test\\jitter_phase_mutants.py --run --dry-run   the plan; writes nothing, starts nothing

--run needs the MSVC toolchain (cl.exe on PATH, or Visual Studio found with vswhere as build.bat does) and takes a few minutes.
--self-test runs in build.bat's :rig_temporal_test and is what keeps an edit of a source from silently orphaning a mutation: if an
anchor stops matching, the build fails and this file says which. It is a temporary instrument with the key it pins: when
experimental.temporal_aa_jitter_follows_upscale is dropped after its flight, delete this file and its build.bat line with it.
"""
import argparse
import concurrent.futures
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
BUILD_BAT = ROOT / "build.bat"
SELF = "tools\\temporal_test\\jitter_phase_mutants.py"

CL_FLAGS = ["/nologo", "/O2", "/MT", "/std:c++17", "/EHsc", "/W4", "/DWIN32_LEAN_AND_MEAN", "/DNOMINMAX", "/D_CRT_SECURE_NO_WARNINGS"]
RUN_TIMEOUT = 300.0

# One entry a rig: where it is, its build.bat label, how it is built (closure or replace), and what its compile line holds (the self-test
# checks that build.bat still compiles it that way). `tests` names the files whose text holds the rig's check labels.
RIGS = {
    "math": {
        "rig": ROOT / "tools" / "temporal_test" / "temporal_test.cpp",
        "label": ":rig_temporal_test",
        "kind": "closure",
        "extra_sources": [],
        "libs": [],
        "flags": [],
        "run": [],
    },
    "flat": {
        "rig": ROOT / "tools" / "flat_temporal_test" / "flat_temporal_test.cpp",
        "label": ":rig_flat_temporal_test",
        "kind": "closure",
        "extra_sources": ["third_party\\dxbc_hash\\DxilHash.cpp"],
        "libs": ["kernel32.lib"],
        "flags": [],
        "run": ["--self-test"],
    },
    "world": {
        "rig": ROOT / "tools" / "vr_world_route_test" / "vr_world_route_test.cpp",
        "label": ":rig_vr_world_route_test",
        "kind": "closure",
        "extra_sources": [],
        "libs": ["kernel32.lib"],
        "flags": [],
        "run": ["--self-test", "{root}"],
    },
    "eye": {
        "rig": ROOT / "tools" / "native_temporal_test" / "native_temporal_test.cpp",
        "label": ":rig_native_temporal_test",
        "kind": "replace",
        "replace": "src\\d3d11\\native_temporal.cpp",
        # the production sources of build.bat's compile line, the first being replaced by the edited copy
        "extra_sources": ["src\\d3d11\\native_temporal.cpp", "src\\common\\config.cpp", "src\\common\\frame_flag.cpp", "src\\common\\log.cpp"],
        "libs": ["kernel32.lib", "user32.lib", "dxgi.lib"],
        "flags": ["/DUNICODE", "/D_UNICODE"],
        "includes": ["third_party\\openxr\\include"],
        "run": ["--phase-count-self-test"],
    },
}


class Mutant:
    def __init__(self, name, rig, target, expect, edits, why):
        self.name = name
        self.rig = rig                      # a key of RIGS
        self.target = ROOT / target         # the production file the edits apply to
        self.expect = expect                # text a failing check's label must contain
        self.edits = list(edits)            # (old, new) pairs, applied in order
        self.why = why


def M(name, rig, target, expect, edits, why):
    return Mutant(name, rig, target, expect, edits, why)


MATH = "src/common/temporal_math.h"
LIVE = "src/d3d11/flat_live_phase.h"
CAMERA = "src/d3d11/flat_camera_phase.h"
ROUTE = "src/d3d11/vr_world_route_math.h"
EYE = "src/d3d11/native_temporal.cpp"

MUTANTS = [
    # ---- math: the rule and the fixed eight (temporal_math.h) ----------------------------------------------------------------------------
    M("ceiling-lost", "math", MATH, "a hair above 1x (8.008) is 9",
      [("(8u * out + render - 1u) / render;", "(8u * out) / render;")], "the count is rounded down, not up"),
    M("cap-lost", "math", MATH, "held at the cap",
      [("    return phases > kTemporalJitterCountMax ? kTemporalJitterCountMax\n                                            : static_cast<uint32_t>(phases);",
        "    return static_cast<uint32_t>(phases);")], "a ratio past what any upscaler serves is not held at the cap"),
    M("floor-of-eight-lost", "math", MATH, "supersampling) never drops below eight",
      [("    if (out <= render) return kTemporalJitterCount;\n", "")], "a render above the output gets fewer than eight phases"),
    M("cap-too-low", "math", MATH, "3x (ultra performance) is 72",
      [("constexpr uint32_t kTemporalJitterCountMax = 128;", "constexpr uint32_t kTemporalJitterCountMax = 64;")], "the cap sits under NVIDIA's own 3x figure"),
    M("fixed-eight-changed", "math", MATH, "temporalJitter is exactly the fixed eight",
      [("    temporalJitterPhase(n, kTemporalJitterCount, jx, jy);\n}", "    temporalJitterPhase(n, 2 * kTemporalJitterCount, jx, jy);\n}")],
      "the fixed-eight sequence every path ran before the key is not the eight"),
    M("zero-count-not-eight", "math", MATH, "a count of zero reads as eight",
      [("(n % (count ? count : kTemporalJitterCount)) + 1;", "(n % (count ? count : 2 * kTemporalJitterCount)) + 1;")], "a count of zero is not read as the fixed eight"),
    # ---- flat: the phase machine and the route decision ---------------------------------------------------------------------------------
    M("machine-ignores-count", "flat", LIVE, "each frame's offset is the sequence number's remainder in 32",
      [("temporalJitterPhase(phaseSequence++, phaseCount, &currentX, &currentY);", "temporalJitterPhase(phaseSequence++, kTemporalJitterCount, &currentX, &currentY);")],
      "the phase machine draws from the fixed eight whatever it was asked for"),
    M("machine-default-not-eight", "flat", LIVE, "the default is the fixed eight and the machine says so",
      [("uint32_t phases = kTemporalJitterCount) {", "uint32_t phases = 2 * kTemporalJitterCount) {")], "a four-argument beginFrame no longer runs the fixed eight"),
    M("machine-count-not-recorded", "flat", LIVE, "the machine records the count it was asked for",
      [("        phaseCount = phases ? phases : kTemporalJitterCount;", "        phaseCount = kTemporalJitterCount;")],
      "the 5 s line would say eight whatever the sequence ran"),
    M("route-gate-dropped", "flat", CAMERA, "every route but upstream keeps the eight",
      [("if (!followsUpscale || route != FlatCameraRoute::Upstream) return kTemporalJitterCount;", "if (!followsUpscale) return kTemporalJitterCount;")],
      "the Legacy and Off routes run a longer sequence than the lighting patch can take"),
    M("key-ignored-by-route", "flat", CAMERA, "the key off is the fixed eight on the upstream route",
      [("if (!followsUpscale || route != FlatCameraRoute::Upstream) return kTemporalJitterCount;", "if (route != FlatCameraRoute::Upstream) return kTemporalJitterCount;")],
      "the key off still gives the upstream route a longer sequence"),
    # ---- world: the route's window token --------------------------------------------------------------------------------------------------
    M("window-token-dropped", "world", ROUTE, "carries the phase count the route ran",
      [('"phases=%u jitter=%s phase=%.4f,%.4f', '"jitter=%s phase=%.4f,%.4f'), ("w.hdr.lastVerdict, w.phases, w.jitter,", "w.hdr.lastVerdict, w.jitter,")],
      "the 5 s line does not say how many phases the route ran"),
    M("window-default-zero", "world", ROUTE, "carries the phase count the route ran",
      [("    uint32_t phases = kTemporalJitterCount;", "    uint32_t phases = 0;")], "a window nothing has set says zero phases"),
    # ---- eye: native_temporal.cpp, flown on WARP ----------------------------------------------------------------------------------------------
    M("eye-key-ignored", "eye", EYE, "key off (the default), DLSS 320x240",
      [("  if(!s.currentSettings.jitterScaled)return phases;\n", "")], "the key off still follows the upscale ratio"),
    M("eye-min-not-max", "eye", EYE, "key on, the same 1.5x upscale",
      [("phases=(std::max)(phases,edvr::temporalJitterPhaseCount(", "phases=(std::min)(phases,edvr::temporalJitterPhaseCount(")],
      "the eyes' counts combine to the smaller, which is never above eight"),
    M("eye-count-not-used", "eye", EYE, "key on, the same 1.5x upscale",
      [("edvr::temporalJitterPhase(s->frameCounter,phases,&jx,&jy);", "edvr::temporalJitterPhase(s->frameCounter,edvr::kTemporalJitterCount,&jx,&jy);")],
      "the count is worked out and the frame still draws from the fixed eight"),
    M("eye-output-not-recorded", "eye", EYE, "key on, the same 1.5x upscale",
      [("  s->outWidth[eye]=outW?outW:w;s->outHeight[eye]=outH?outH:h;\n", "  s->outWidth[eye]=0;s->outHeight[eye]=0;\n")],
      "the output the pass was asked for never reaches the ratio"),
    M("eye-count-not-logged", "eye", EYE, "logs its count at the start and again at each change",
      [("  if(phased)notePhases(*s,phases);\n", "  (void)phased;\n")], "the count is never logged"),
    M("eye-key-flip-unlogged", "eye", EYE, "logs its count at the start and again at each change",
      [("if(phases==s.phasesLogged&&scaled==s.phasesLoggedScaled)return;", "if(phases==s.phasesLogged)return;")],
      "a flip of the key that leaves the count where it was is not logged"),
    M("eye-key-in-history", "eye", EYE, "flipping the key live resets no history",
      [("a.jitter==b.jitter&&", "a.jitter==b.jitter&&a.jitterScaled==b.jitterScaled&&")], "flipping the key live resets every eye's history"),
]


# ---- applying an edit ------------------------------------------------------------------------------------------------------------------
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
    return Path(path).read_bytes().decode("utf-8").replace("\r\n", "\n")


def failing_labels(output):
    """The labels of the rigs' failing checks: '  FAIL  <label>' (the pure rigs) or 'FAIL: <label>' (the native and flat rigs)."""
    found = []
    for line in output.splitlines():
        if line.startswith("  FAIL  "):
            found.append(line[len("  FAIL  "):].strip())
        elif line.startswith("FAIL: "):
            found.append(line[len("FAIL: "):].strip())
    return found


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


def include_closure(first_files):
    """Every file under the repo that `first_files` include with quotes, transitively, plus the files themselves."""
    seen, todo = set(), [Path(p) for p in first_files]
    while todo:
        path = todo.pop().resolve()
        if path in seen or not path.is_file():
            continue
        try:
            path.relative_to(ROOT)
        except ValueError:
            continue
        seen.add(path)
        for inc in re.findall(r'^\s*#\s*include\s+"([^"]+)"', path.read_text(encoding="utf-8", errors="replace"), re.M):
            todo.append(path.parent / inc)
    return sorted(seen)


def rig_text(key):
    """The text that holds a rig's check labels: its source and every file under tools\\ it includes."""
    spec = RIGS[key]
    tools = ROOT / "tools"
    parts = []
    for path in include_closure([spec["rig"]]):
        try:
            path.relative_to(tools)
        except ValueError:
            continue
        parts.append(read_source(path))
    return "\n".join(parts)


# ---- the toolchain -----------------------------------------------------------------------------------------------------------------------
class Toolchain:
    """cl.exe by absolute path, with the environment it needs: this one if cl is on PATH, else the one vcvars64.bat makes, found with
    vswhere as build.bat does."""

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


def build_and_run(tc, rig_key, target, tree, edited_text):
    """('nocompile'|'pass'|'fail'|'crash'|'timeout', detail): the rig built in `tree` with `edited_text` in place of `target` (the unedited
    text for the control) and run. A 'fail' detail is the failing labels, joined with ' | '."""
    spec = RIGS[rig_key]
    tree.mkdir(parents=True, exist_ok=True)
    exe = tree / "rig.exe"
    extra = []
    if spec["kind"] == "closure":
        # The rig and its include closure, mirrored under the temp directory, so its relative includes find the edited header.
        rig_dest = tree / spec["rig"].relative_to(ROOT)
        for src in include_closure([spec["rig"]]):
            dest = tree / src.relative_to(ROOT)
            dest.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(src, dest)
        (tree / target.relative_to(ROOT)).write_text(edited_text, encoding="utf-8", newline="\n")
        sources = [str(rig_dest)] + [str(ROOT / s) for s in spec["extra_sources"]]
    else:
        edited = tree / target.name
        edited.write_text(edited_text, encoding="utf-8", newline="\n")
        sources = [str(spec["rig"])] + [str(edited) if s == spec["replace"] else str(ROOT / s) for s in spec["extra_sources"]]
        extra = ["/I" + str(SRC)]
    extra += ["/I" + str(ROOT / i) for i in spec.get("includes", [])]
    cmd = [tc.cl] + CL_FLAGS + spec["flags"] + extra + ["/Fo" + str(tree) + os.sep, "/Fe" + str(exe)] + sources + \
          ["/link", "/INCREMENTAL:NO"] + spec["libs"]
    done = subprocess.run(cmd, capture_output=True, text=True, env=tc.env, errors="replace", cwd=str(ROOT))
    if done.returncode != 0:
        errors = [l.strip() for l in (done.stdout + done.stderr).splitlines() if "error" in l]
        return "nocompile", " | ".join(errors[:8])[:1500]
    run_cmd = [str(exe)] + [a.replace("{root}", str(ROOT)) for a in spec["run"]]
    try:
        run = subprocess.run(run_cmd, capture_output=True, text=True, env=tc.env, errors="replace", timeout=RUN_TIMEOUT, cwd=str(ROOT))
    except subprocess.TimeoutExpired:
        return "timeout", "no result within %d s" % RUN_TIMEOUT
    out = run.stdout + "\n" + run.stderr
    if run.returncode == 0:
        return "pass", ""
    labels = failing_labels(out)
    if not labels:
        return "crash", "exit 0x%08X" % (run.returncode & 0xFFFFFFFF)
    return "fail", " | ".join(labels)


# ---- the run -------------------------------------------------------------------------------------------------------------------------------
def select(only, rig):
    chosen = MUTANTS
    if rig:
        chosen = [m for m in chosen if m.rig == rig]
    if only:
        wanted = [n for n in only.split(",") if n]
        unknown = [n for n in wanted if n not in {m.name for m in MUTANTS}]
        if unknown:
            raise ValueError("no such mutation: " + ", ".join(unknown))
        chosen = [m for m in chosen if m.name in wanted]
    return chosen


def plan_text(mutants):
    lines = ["%d mutation(s); each is built in a temp directory outside the repo against one edited production file and run:" % len(mutants)]
    for m in mutants:
        lines.append("  %-28s %-5s %s -- fails on a check labelled %r" % (m.name, m.rig, m.why, m.expect))
    lines.append("cl.exe flags: " + " ".join(CL_FLAGS))
    return "\n".join(lines)


def run_all(only=None, rig=None, jobs=None, keep=False, dry_run=False, out=sys.stdout):
    mutants = select(only, rig)
    if dry_run:
        print(plan_text(mutants), file=out)
        print("dry-run: nothing compiled, started or written", file=out)
        return 0
    tc = Toolchain()
    work = Path(tempfile.mkdtemp(prefix="jitter_phase_mutants_"))
    try:
        used = sorted({m.rig for m in mutants})
        # The control for each rig: its first target, unedited, built this way. A rig that does not pass on the real source built this
        # way would make every verdict below meaningless.
        firsts = {k: next(m.target for m in mutants if m.rig == k) for k in used}
        with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, len(used))) as pool:
            controls = list(pool.map(lambda k: (k, build_and_run(tc, k, firsts[k], work / ("control_" + k), read_source(firsts[k]))), used))
        for key, (outcome, detail) in controls:
            print("control, rig %s (the unedited source): %s %s" % (key, outcome, detail[:150]), file=out)
        if any(outcome != "pass" for _, (outcome, _) in controls):
            print("a rig does not pass on the unedited source when built this way; nothing below means anything", file=out)
            return 1

        def one(m):
            try:
                text = apply_edits(read_source(m.target), m.edits, m.name)
            except ValueError as error:
                return m, "badedit", str(error)
            outcome, detail = build_and_run(tc, m.rig, m.target, work / m.name, text)
            return m, outcome, detail

        results = []
        with concurrent.futures.ThreadPoolExecutor(max_workers=jobs or min(4, os.cpu_count() or 2)) as pool:
            for m, outcome, detail in pool.map(one, mutants):
                if outcome == "fail" and any(m.expect in label for label in detail.split(" | ")):
                    verdict = "caught"
                elif outcome == "fail":
                    verdict = "OTHER"
                else:
                    verdict = {"pass": "SURVIVED", "crash": "CRASH", "timeout": "TIMEOUT", "nocompile": "NOCOMPILE", "badedit": "BADEDIT"}[outcome]
                results.append((m, verdict, detail))
                print("%-9s %-28s %-5s %s" % (verdict, m.name, m.rig, detail[:140]), file=out)
        bad = [r for r in results if r[1] != "caught"]
        print("%d mutation(s): %d caught by a check that names them, %d not" % (len(results), len(results) - len(bad), len(bad)), file=out)
        for m, verdict, detail in bad:
            print("  NOT CAUGHT: %s (%s): wanted a failing check labelled %r, got %s %s" % (m.name, m.why, m.expect, verdict, detail[:160]), file=out)
        return 0 if not bad else 1
    finally:
        if keep:
            print("kept: %s" % work, file=out)
        else:
            shutil.rmtree(work, ignore_errors=True)


# ---- the self-test -----------------------------------------------------------------------------------------------------------------------------
def self_test():
    failures = []

    def check(ok, what):
        if not ok:
            failures.append(what)

    # the tool's own pieces
    check(failing_labels("  ok    a\n  FAIL  b: c\n  FAIL  d\nFAIL: e\nx FAIL: not a line start\n3 checks FAILED\n") == ["b: c", "d", "e"],
          "failing_labels reads the rigs' FAIL lines, both spellings, and only at the start of a line")
    check(apply_edits("a b c", [("b", "B")]) == "a B c", "apply_edits applies one edit")
    for bad, why in ((("a b b", [("b", "B")]), "a repeated anchor"), (("a b", [("x", "y")]), "a missing anchor"), (("a b", [("b", "b")]), "a no-op edit")):
        try:
            apply_edits(*bad)
            check(False, "apply_edits refuses " + why)
        except ValueError:
            pass
    joined = label_block(":rig_a\nx ^\n y\nexit /b 0\n:rig_b\nz\n", ":rig_a")
    check(joined is not None and [" ".join(l.split()) for l in joined] == ["x y", "exit /b 0"], "label_block joins continuations and stops at the next label")

    # every mutation against the file it edits as it is now, and against its rig
    sources = {}
    names = [m.name for m in MUTANTS]
    check(len(names) == len(set(names)), "mutation names are unique")
    check(len(MUTANTS) >= 19, "the mutation list did not shrink (%d)" % len(MUTANTS))
    labels = {key: rig_text(key) for key in RIGS}
    for m in MUTANTS:
        check(m.rig in RIGS, "%s names a rig this tool knows" % m.name)
        if m.rig not in RIGS:
            continue
        if m.target not in sources:
            sources[m.target] = read_source(m.target)
        try:
            mutated = apply_edits(sources[m.target], m.edits, m.name)
            check(mutated != sources[m.target], "%s changes the source" % m.name)
        except ValueError as error:
            failures.append(str(error))
        check(m.expect in labels[m.rig], "%s: rig %s has no check labelled %r" % (m.name, m.rig, m.expect))
    for key in RIGS:
        check(any(m.rig == key for m in MUTANTS), "rig %s has a mutation" % key)

    # build.bat compiles each rig the way this tool does, and runs this tool's self-test
    bat = BUILD_BAT.read_bytes().decode("utf-8", errors="replace")
    for key, spec in RIGS.items():
        block = label_block(bat, spec["label"])
        check(block is not None, "build.bat has %s" % spec["label"])
        if not block:
            continue
        cl = next((l for l in block if l.strip().lower().startswith("cl.exe")), "")
        for flag in CL_FLAGS + spec["flags"]:
            check(flag in cl, "build.bat's %s compile line lacks %s" % (spec["label"], flag))
        rig_name = "tools\\%s\\%s" % (spec["rig"].parent.name, spec["rig"].name)
        check('"%s"' % rig_name in cl, "build.bat's %s compile line lacks %s" % (spec["label"], rig_name))
        for src in spec["extra_sources"]:
            check('"%s"' % src in cl, "build.bat's %s compile line lacks %s" % (spec["label"], src))
        for lib in spec["libs"]:
            check(lib in cl, "build.bat's %s compile line lacks %s" % (spec["label"], lib))
        for inc in spec.get("includes", []):
            check('"%s"' % inc in cl, "build.bat's %s compile line lacks /I%s" % (spec["label"], inc))
        for arg in spec["run"]:
            if arg.startswith("--"):
                check(any(arg in l for l in block), "build.bat's %s does not run its rig with %s" % (spec["label"], arg))
    block = label_block(bat, RIGS["math"]["label"])
    check(block is not None and any(SELF in l and "--self-test" in l for l in block),
          "build.bat's %s runs this tool's --self-test" % RIGS["math"]["label"])

    if failures:
        for f in failures:
            print("FAIL: " + f)
        return 1
    print("PASS: jitter phase mutants.py self-test (%d mutations over %d rigs, every anchor found once, build.bat wired)" % (len(MUTANTS), len(RIGS)))
    return 0


def main(argv):
    ap = argparse.ArgumentParser(description="mutation proof for the jitter phase count's rigs")
    ap.add_argument("--self-test", action="store_true")
    ap.add_argument("--run", action="store_true")
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--rig", choices=sorted(RIGS))
    ap.add_argument("--only", default="")
    ap.add_argument("--jobs", type=int, default=0)
    ap.add_argument("--keep", action="store_true")
    ap.add_argument("--dry-run", action="store_true")
    a = ap.parse_args(argv)
    if a.self_test:
        return self_test()
    if a.list:
        print(plan_text(select(a.only, a.rig)))
        return 0
    if a.run:
        return run_all(a.only, a.rig, a.jobs or None, a.keep, a.dry_run)
    ap.print_help()
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
