#!/usr/bin/env python3
"""The mutation machinery the F2 rigs share (skin_join_test, skin_join_gpu_test, skin_clone_test): tools\\skin_ledger_test\\mutants.py, parametrised.

A rig that passes proves little until it is seen to FAIL on a source that breaks the rule it pins. A rig's mutants.py describes its sources and its
mutations (one textual edit, or a few that belong together, each naming the case ids whose checks must report it) and calls main(config, mutants).
For each mutation the machinery copies the sources it edits into a temp directory OUTSIDE the repo, applies the edit, rebuilds the rig against the
copy (a mutated header or unit) or runs the unmutated rig against an edited copy of a glue source it reads as text (a pin), and requires the rig
to fail on a check of the named case: a FAIL label starting with "<case>.". Nothing is written inside the repo; the temp directory is removed.

  --self-test   text only: every anchor is found exactly once in the file it edits as it is now, every case named is in the rig, and build.bat
                compiles the rig the way this machinery does and runs this self-test
  --run [--only a,b] [--jobs N] [--keep] [--verbose]
  --list
  --run --dry-run   the plan; writes nothing, starts nothing

--run needs the MSVC toolchain (cl.exe on PATH, or Visual Studio found with vswhere as build.bat does).
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

DEFAULT_CL_FLAGS = ["/nologo", "/O2", "/MT", "/std:c++17", "/EHsc", "/W4", "/DWIN32_LEAN_AND_MEAN", "/DNOMINMAX", "/D_CRT_SECURE_NO_WARNINGS"]


class Mutant:
    def __init__(self, name, caught, file, edits, why):
        self.name = name
        self.caught = tuple([caught] if isinstance(caught, str) else caught)
        self.file = file
        self.edits = list(edits)
        self.why = why


def M(name, caught, file, edits, why):
    return Mutant(name, caught, file, edits, why)


class Config:
    """Everything that differs between rigs."""

    def __init__(self, here, files, header_keys, pin_keys, rig, rig_label, rig_source_in_bat, rig_exe_in_bat, case_prefix,
                 include_dirs=("src/d3d11",), units=(), cl_flags=None, defines=(), link_args=(), min_mutants=1, run_timeout=180.0,
                 rig_args=lambda root: ["--self-test", str(root)], bat_run_checks=None, tree_extra=()):
        self.here = Path(here).resolve().parent
        self.root = self.here.parents[1]
        self.files = files                       # key -> absolute Path
        self.header_keys = tuple(header_keys)    # compiled into the rig: a mutation of one rebuilds it
        self.pin_keys = tuple(pin_keys)          # read by the rig as text: a mutation of one runs through the unmutated rig
        self.rig = rig
        self.rig_label = rig_label
        self.rig_source_in_bat = rig_source_in_bat
        self.rig_exe_in_bat = rig_exe_in_bat
        self.case_prefix = case_prefix
        self.include_dirs = tuple(include_dirs)
        self.units = tuple(units)                # (tree-relative path, key) compiled from the mutated tree beside the rig
        self.cl_flags = list(cl_flags or DEFAULT_CL_FLAGS) + ["/D" + d for d in defines]
        self.link_args = list(link_args)
        self.min_mutants = min_mutants
        self.run_timeout = run_timeout
        self.rig_args = rig_args
        self.bat_run_checks = bat_run_checks     # extra strings the rig's build.bat block must contain
        self.tree_extra = tuple(tree_extra)      # (relative path, source) copied into the tree unmutated (headers the rig includes)
        self.build_bat = self.root / "build.bat"
        self.tree = [(rel, p) for rel, p in self.tree_extra]


# ---- text helpers --------------------------------------------------------------------------------------------------------
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


def fail_labels(output, prefix):
    """Every check label on a 'FAIL: <case>.<what>' line (the text up to ' -- ', ' [' or the end of the line)."""
    labels = []
    for line in output.splitlines():
        if line.startswith("FAIL: "):
            label = re.split(r" -- | \[", line[len("FAIL: "):], 1)[0].strip()
            if re.match(prefix + r"\d+\.", label):
                labels.append(label)
    return labels


def rig_cases(rig_text, prefix):
    return set(re.findall(r'"(' + prefix + r'\d+)[.:]', rig_text))


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


class Toolchain:
    """cl.exe with the environment it needs: this one if cl is on PATH, else the one vcvars64.bat makes."""

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


def build_rig(cfg, tc, tree, exe):
    cmd = [tc.cl] + cfg.cl_flags + ["/I" + str(tree / d) for d in cfg.include_dirs] + ["/I" + str(tree)] + [
        "/Fo" + str(exe.parent) + "\\", "/Fe" + str(exe), str(cfg.rig)] + [str(tree / rel) for rel, _ in cfg.units] + ["/link", "/INCREMENTAL:NO"] + cfg.link_args
    done = subprocess.run(cmd, capture_output=True, text=True, env=tc.env, errors="replace", cwd=str(exe.parent))
    return done.returncode, done.stdout + done.stderr


def run_rig(cfg, exe, root, tc):
    try:
        done = subprocess.run([str(exe)] + cfg.rig_args(root), capture_output=True, text=True, env=tc.env, errors="replace", timeout=cfg.run_timeout)
    except subprocess.TimeoutExpired:
        return "timeout", [], "no result within %d s" % cfg.run_timeout
    out = done.stdout + "\n" + done.stderr
    if done.returncode == 0:
        return "pass", [], ""
    labels = fail_labels(out, cfg.case_prefix)
    if not labels:
        return "crash", [], "exit 0x%08X" % (done.returncode & 0xFFFFFFFF)
    return "fail", labels, ""


def make_tree(cfg, dest, mutated=None):
    """The headers and units the rig includes, laid out under `dest`; `mutated` maps a files key to replacement text."""
    mutated = mutated or {}
    for key in cfg.header_keys:
        target = dest / cfg.files[key].relative_to(cfg.root)
        target.parent.mkdir(parents=True, exist_ok=True)
        if key in mutated:
            target.write_text(mutated[key], encoding="utf-8", newline="\n")
        else:
            shutil.copyfile(cfg.files[key], target)
    for rel, src in cfg.tree:
        target = dest / rel
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(src, target)


def make_root(cfg, dest, mutated):
    for key in cfg.pin_keys:
        target = dest / cfg.files[key].relative_to(cfg.root)
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(mutated.get(key, read_source(cfg.files[key])), encoding="utf-8", newline="\n")


def select(mutants, only):
    if not only:
        return mutants
    wanted = [n for n in only.split(",") if n]
    unknown = [n for n in wanted if n not in {m.name for m in mutants}]
    if unknown:
        raise ValueError("no such mutation: " + ", ".join(unknown))
    return [m for m in mutants if m.name in wanted]


def plan_text(cfg, mutants):
    lines = ["%d mutation(s); each runs the rig against an edited copy of one source in a temp directory outside the repo and must fail on a check of the cases named:" % len(mutants)]
    for m in mutants:
        lines.append("  %-40s %-8s caught by %-7s %s" % (m.name, m.file, "/".join(m.caught), m.why))
    lines.append("cl.exe flags: " + " ".join(cfg.cl_flags))
    return "\n".join(lines)


def run_all(cfg, mutants_all, only=None, jobs=None, keep=False, dry_run=False, verbose=False, out=sys.stdout):
    mutants = select(mutants_all, only)
    if dry_run:
        print(plan_text(cfg, mutants), file=out)
        print("dry-run: nothing compiled, started or written", file=out)
        return 0
    tc = Toolchain()
    sources = {k: read_source(p) for k, p in cfg.files.items()}
    work = Path(tempfile.mkdtemp(prefix="rm_"))
    workers = jobs or min(8, os.cpu_count() or 2)
    try:
        control = work / "k"
        make_tree(cfg, control)
        exe = control / "rig.exe"
        code, text = build_rig(cfg, tc, control, exe)
        if code != 0:
            print("control: nocompile\n" + text.strip()[-1500:], file=out)
            return 1
        outcome, labels, tail = run_rig(cfg, exe, cfg.root, tc)
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
            if m.file in cfg.header_keys:
                tree = d / "t"
                make_tree(cfg, tree, {m.file: mutated})
                mexe = d / "rig.exe"
                code, text = build_rig(cfg, tc, tree, mexe)
                if code != 0:
                    return m, "nocompile", (text.strip().splitlines() or [""])[-1]
                outcome, labels, tail = run_rig(cfg, mexe, cfg.root, tc)
            else:
                root = d / "r"
                make_root(cfg, root, {m.file: mutated})
                outcome, labels, tail = run_rig(cfg, exe, root, tc)
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


def self_test(cfg, mutants_all, build_bat=None):
    failures = []

    def check(ok, what):
        if not ok:
            failures.append(what)

    check(fail_labels("a\nFAIL: S4.flight -- x [y]\nFAIL: S2.sub\nFAIL: rig: 3 of 9\n", "S") == ["S4.flight", "S2.sub"],
          "fail_labels reads the label of every check's FAIL: line, and not the closing summary")
    check(fail_labels("PASS: 12 checks\n", "S") == [], "fail_labels reads nothing from a pass")
    check(apply_edits("a b c", [("b", "B")]) == "a B c", "apply_edits applies one edit")
    for bad, why in ((("a b b", [("b", "B")]), "a repeated anchor"), (("a b", [("x", "y")]), "a missing anchor"), (("a b", [("b", "b")]), "a no-op edit")):
        try:
            apply_edits(*bad)
            check(False, "apply_edits refuses " + why)
        except ValueError:
            pass
    joined = label_block(":rig_a\nx ^\n y\nexit /b 0\n:rig_b\nz\n", ":rig_a")
    check(joined is not None and [" ".join(l.split()) for l in joined] == ["x y", "exit /b 0"], "label_block joins continuations and stops at the next label")
    check(rig_cases('check(x, "S1.a: b"); check(y, "S10.c")', "S") == {"S1", "S10"}, "rig_cases reads the case ids off the labels")

    sources = {k: read_source(p) for k, p in cfg.files.items()}
    rig = read_source(cfg.rig)
    cases = rig_cases(rig, cfg.case_prefix)
    names = [m.name for m in mutants_all]
    check(len(names) == len(set(names)), "mutation names are unique")
    check(len(mutants_all) >= cfg.min_mutants, "the mutation list did not shrink below %d (%d)" % (cfg.min_mutants, len(mutants_all)))
    for m in mutants_all:
        check(m.file in cfg.files, "%s edits %s, which this tool does not know" % (m.name, m.file))
        if m.file in cfg.files:
            try:
                mutated = apply_edits(sources[m.file], m.edits, m.name)
                check(mutated != sources[m.file], "%s changes the source" % m.name)
            except ValueError as error:
                failures.append(str(error))
        for c in m.caught:
            check(c in cases, "%s: the rig has no case %s" % (m.name, c))
    covered = {c for m in mutants_all for c in m.caught}
    check(covered == cases, "every case of the rig has a mutation, and only cases of the rig: %s vs %s" % (sorted(covered), sorted(cases)))
    for key, p in cfg.files.items():
        check(Path(p).is_file(), "%s exists" % p)

    bat = Path(build_bat or cfg.build_bat).read_bytes().decode("utf-8", errors="replace")
    block = label_block(bat, cfg.rig_label)
    check(block is not None, "build.bat has %s" % cfg.rig_label)
    if block:
        text = "\n".join(block)
        cl = next((l for l in block if l.strip().lower().startswith("cl.exe")), "")
        for flag in cfg.cl_flags:
            check(flag in cl, "build.bat's rig compile has %s" % flag)
        for d in cfg.include_dirs:
            check('/I"%s"' % d.replace("/", "\\") in cl, "build.bat's rig finds the headers through /I %s (the mutated tree is found the same way)" % d)
        check(cfg.rig_source_in_bat in cl, "build.bat's rig compile has the rig's source")
        for rel, _ in cfg.units:
            check(rel.replace("/", "\\") in cl, "build.bat's rig compile has the unit %s" % rel)
        check(cfg.rig_exe_in_bat + '" --dry-run' in text, "build.bat runs the rig's --dry-run")
        check(cfg.rig_exe_in_bat + '" --self-test' in text, "build.bat runs the rig's --self-test")
        check('mutants.py" --self-test' in text, "build.bat runs this tool's self-test")
        for extra in (cfg.bat_run_checks or []):
            check(extra in text, "build.bat's rig block has %s" % extra)

    calls = []
    real_mkdtemp, real_run = tempfile.mkdtemp, subprocess.run
    tempfile.mkdtemp = lambda *a, **k: calls.append("mkdtemp") or real_mkdtemp(*a, **k)
    subprocess.run = lambda *a, **k: calls.append("subprocess") or real_run(*a, **k)
    try:
        sink = io.StringIO()
        code = run_all(cfg, mutants_all, dry_run=True, out=sink)
    finally:
        tempfile.mkdtemp, subprocess.run = real_mkdtemp, real_run
    check(code == 0 and not calls and "dry-run" in sink.getvalue(), "--dry-run starts no process and makes no directory (saw %s)" % calls)
    try:
        select(mutants_all, "no-such-mutation")
        check(False, "--only names an unknown mutation")
    except ValueError:
        pass

    if failures:
        for f in failures:
            print("FAIL: " + f, file=sys.stderr)
        return 1
    print("PASS: %s mutants.py self-test (%d mutations over %d cases, every anchor found once, build.bat wired)" % (cfg.rig.parent.name, len(mutants_all), len(covered)))
    return 0


def main(cfg, mutants, argv=None):
    parser = argparse.ArgumentParser(description=(__doc__ or "").split("\n")[0])
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--run", action="store_true")
    parser.add_argument("--list", action="store_true")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--only", default="")
    parser.add_argument("--jobs", type=int, default=0)
    parser.add_argument("--keep", action="store_true", help="leave the temp directory (printed) for a look at a mutant")
    parser.add_argument("--verbose", action="store_true")
    parser.add_argument("--build-bat", default=str(cfg.build_bat))
    args = parser.parse_args(argv)
    if args.self_test:
        return self_test(cfg, mutants, args.build_bat)
    if args.list:
        print(plan_text(cfg, mutants))
        return 0
    if args.run:
        try:
            return run_all(cfg, mutants, only=args.only, jobs=args.jobs or None, keep=args.keep, dry_run=args.dry_run, verbose=args.verbose)
        except (ValueError, RuntimeError) as error:
            print("error: %s" % error, file=sys.stderr)
            return 2
    parser.print_help()
    return 2
