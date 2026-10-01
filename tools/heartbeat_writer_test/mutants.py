#!/usr/bin/env python3
"""The mutation proof for tools\\heartbeat_writer_test: the rig fails when a rule of the heartbeat's writer thread is flipped.

The rig (heartbeat_writer_test.cpp) runs the REAL src\\common\\heartbeat_writer.h and the REAL src\\common\\proxy.cpp (its breadcrumb file, its crash
filter) and reads proxy.cpp and src\\d3d11\\d3d11_proxy.cpp as text for the glue. Every check carries a label "H<case>.<what>". A rig that passes
proves little until it is seen to FAIL on a source that breaks the rule it pins: that the render thread does not write, that a held-up writer
never writes an old frame after a new one, that a hung render thread stops the heartbeat, that nothing lands behind the crash filter's lines.
For each mutation below this tool copies the sources into a temp directory OUTSIDE the repo, applies one textual edit (or a few that belong
together), compiles the rig against the copies (the header and proxy.cpp are compiled in, so the copies are what runs) and runs it with that
copy as the repository root, requiring it to fail on a check of the case that belongs to the rule. Nothing is written inside the repo.

  python tools\\heartbeat_writer_test\\mutants.py --self-test       text only: every anchor is found exactly once in the file it edits as it is
                                                                  now, every case named is in the rig, and build.bat builds the rig the way this
                                                                  tool does (add --build-bat PATH to check another copy)
  python tools\\heartbeat_writer_test\\mutants.py --run [--only a,b] [--jobs N] [--keep]
  python tools\\heartbeat_writer_test\\mutants.py --list
  python tools\\heartbeat_writer_test\\mutants.py --run --dry-run   the plan; writes nothing, starts nothing

--run needs the MSVC toolchain. The rig runs real threads and starts two child processes (about twenty seconds a run) and each mutant has
its own directory, so its own breadcrumb file: --jobs defaults to 2.
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
    "hdr": ROOT / "src" / "common" / "heartbeat_writer.h",
    "proxy": ROOT / "src" / "common" / "proxy.cpp",
    "dll": ROOT / "src" / "d3d11" / "d3d11_proxy.cpp",
}
# What the rig compiles besides its own source, and the headers those include, laid out as they are in the repository.
COMMON_SOURCES = ["config.cpp", "log.cpp", "guard.cpp"]
COMMON_HEADERS = ["config.h", "ini_name.h", "log.h", "guard.h", "crash_context.h", "proxy.h", "heartbeat_writer.h"]
RIG = HERE / "heartbeat_writer_test.cpp"
BUILD_BAT = ROOT / "build.bat"
RIG_LABEL = ":rig_heartbeat_writer_test"
CL_FLAGS = ["/nologo", "/O2", "/MT", "/std:c++17", "/EHsc", "/W4", "/DWIN32_LEAN_AND_MEAN", "/DNOMINMAX", "/D_CRT_SECURE_NO_WARNINGS"]
LIBS = ["user32.lib", "version.lib"]
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
POST_STORE = "        posts_.fetch_add(1, std::memory_order_release);\n        if (wake_) SetEvent(wake_);\n"
POST_FIELDS = "        frame_.store(frameNo, std::memory_order_relaxed);\n        uptime_.store(uptimeSeconds, std::memory_order_relaxed);\n"
POST_FIELDS_FIRST_WINS = ("        if (posts_.load(std::memory_order_acquire) == written_.load(std::memory_order_acquire)) {\n"
                          "            frame_.store(frameNo, std::memory_order_relaxed);\n"
                          "            uptime_.store(uptimeSeconds, std::memory_order_relaxed);\n"
                          "        }\n")
POST_CLOSED = "        if (closed_.load(std::memory_order_acquire) || !started_.load(std::memory_order_acquire)) return;\n"
VERSION_ODD = "        version_.store(v + 1, std::memory_order_relaxed);   // odd: a write is in progress\n"
SNAP_ODD = "            if (before & 1u) {\n                YieldProcessor();\n                continue;\n            }\n"
SNAP_AGAIN = "            if (version_.load(std::memory_order_relaxed) == before) return before != 0;\n"
RUN_NEW = "            if (posted == lastPost) continue;                        // nothing new: never a heartbeat of its own\n"
RUN_CLOSED = "            if (closed_.load(std::memory_order_acquire)) continue;   // dropped: nothing is written after a close\n"
RUN_INNER = "            if (!closed_.load(std::memory_order_seq_cst) && sink_) {\n"
RUN_WRITING = "            writing_.store(true, std::memory_order_seq_cst);\n"
RUN_WAIT = "            WaitForSingleObject(wake_, INFINITE);\n"
DRAIN_LOOP = "        while (writing_.load(std::memory_order_seq_cst)) {\n            if (GetTickCount64() >= until) return false;\n            Sleep(1);\n        }\n"
DRAIN_CLOSE = "        close();\n        const ULONGLONG until = GetTickCount64() + maxWaitMs;\n"
CLOSE_STORE = "        closed_.store(true, std::memory_order_seq_cst);\n"
EVENT = "        wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);\n"
HB_POST = "    g_heartbeat.post(frameNo, now / 1000);\n"
HB_START = "    g_heartbeat.start(&heartbeatSink);\n"
HB_CLOSE_FN = "void breadcrumbHeartbeatClose() {\n    g_heartbeat.close();\n}\n"
SINK_CRUMB = "    breadcrumb(line);\n}\n\nLPTOP_LEVEL_EXCEPTION_FILTER g_prevFilter = nullptr;"
FILTER_DRAIN = "    g_heartbeat.closeAndDrain(20);\n"
FILTER_AFTER = "    // The detailed helper has a separate stack frame, preserving the early\n"
DLL_EXIT = "                edvr::breadcrumbHeartbeatClose();\n                edvr::breadcrumb(\"gfx: process exit\");"
DLL_UNLOAD = "                edvr::breadcrumbHeartbeatClose();\n                edvr::breadcrumb(\"gfx: FreeLibrary unload\");"

MUTANTS = [
    # ---- H1: the render thread does not write ------------------------------------------------------------------------------------
    M("post-writes-inline", "H1", "hdr", [(POST_STORE, POST_STORE + "        if (sink_) sink_(frame_.load(), uptime_.load());\n")], "post() calls the sink on the posting thread"),
    M("heartbeat-writes-the-file", ("H10", "H1"), "proxy", [(HB_POST, "    breadcrumb(\"gfx: alive\");\n" + HB_POST)], "breadcrumbHeartbeat writes the breadcrumb file itself again"),
    M("heartbeat-never-posts", ("H10", "H7"), "proxy", [(HB_POST, "")], "breadcrumbHeartbeat hands nothing to the writer"),
    M("heartbeat-never-starts-the-writer", ("H10", "H7"), "proxy", [(HB_START, "")], "the writer thread is never started"),
    M("sink-writes-nothing", ("H10", "H7"), "proxy", [(SINK_CRUMB, "}\n\nLPTOP_LEVEL_EXCEPTION_FILTER g_prevFilter = nullptr;")], "the writer's sink does not write the line"),
    # ---- H2, H3: order and coalescing --------------------------------------------------------------------------------------------
    M("version-never-odd", "H5", "hdr", [(VERSION_ODD, "")], "the record's version does not mark a write in progress: a reader can take half of two posts"),
    M("snapshot-takes-a-torn-read", "H5", "hdr", [(SNAP_ODD, ""), (SNAP_AGAIN, "            return before != 0;\n")], "the reader does not check the version around its reads"),
    # ---- H2, H3: order and coalescing --------------------------------------------------------------------------------------------
    M("first-post-of-a-batch-wins", "H2", "hdr", [(POST_FIELDS, POST_FIELDS_FIRST_WINS)],
      "a record that is pending is not overwritten by a newer post: a held-up writer writes the OLD frame when it comes back"),
    M("writer-drops-every-second-post", "H3", "hdr", [(RUN_NEW, RUN_NEW + "            if ((posted & 1u) == 0) { lastPost = posted; continue; }\n")], "every second post is lost"),
    # ---- H4: no clock of its own -------------------------------------------------------------------------------------------------
    M("writer-writes-every-old-post", "H4", "hdr", [(RUN_NEW, "")], "a wake with nothing new posted writes the last record again"),
    M("event-manual-reset-never-sleeps", "H4", "hdr", [(EVENT, "        wake_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);\n")], "a manual-reset event: once signalled the writer spins on it, burning a core"),
    M("writer-times-out-and-writes", "H4", "hdr", [(RUN_WAIT, "            WaitForSingleObject(wake_, 100);\n"), (RUN_NEW, "")],
      "the writer wakes by itself and writes what it last saw: the heartbeat goes on through a hang"),
    # ---- H6: close and drain -----------------------------------------------------------------------------------------------------
    M("close-does-not-drop-pending", "H6", "hdr", [(RUN_CLOSED, ""), (RUN_INNER, "            if (sink_) {\n")], "a close does not reach the writer: a pending post is written after it"),
    M("write-ignores-a-late-close", "H6", "hdr", [(RUN_INNER, "            if (sink_) {\n")], "a close that lands as the write begins does not stop it"),
    M("post-after-close-accepted", "H6", "hdr", [(POST_CLOSED, "        if (!started_.load(std::memory_order_acquire)) return;\n")], "a post after the close is accepted and written"),
    M("drain-does-not-wait", "H6", "hdr", [(DRAIN_LOOP, "")], "closeAndDrain returns without waiting for the write under way"),
    M("drain-waits-for-ever", "H6", "hdr", [(DRAIN_LOOP, "        while (writing_.load(std::memory_order_seq_cst)) {\n            Sleep(1);\n        }\n")], "closeAndDrain has no bound"),
    M("drain-does-not-close", "H6", "hdr", [(DRAIN_CLOSE, "        const ULONGLONG until = GetTickCount64() + maxWaitMs;\n")], "closeAndDrain waits but does not close"),
    M("close-never-closes", "H6", "hdr", [(CLOSE_STORE, "")], "close() does not close"),
    # ---- H8, H10: nothing behind the crash filter's lines ------------------------------------------------------------------------
    M("filter-does-not-close-the-heartbeat", "H10", "proxy", [(FILTER_DRAIN, "")], "the crash filter leaves the heartbeat running"),
    M("filter-closes-after-its-first-line", "H10", "proxy", [(FILTER_DRAIN, ""), (FILTER_AFTER, "    g_heartbeat.closeAndDrain(20);\n" + FILTER_AFTER)],
      "the crash filter closes the heartbeat only after it has written its first crumb"),
    M("dll-exit-does-not-close", "H10", "dll", [(DLL_EXIT, "                edvr::breadcrumb(\"gfx: process exit\");")], "DllMain's process-exit path leaves the heartbeat open"),
    M("dll-unload-does-not-close", "H10", "dll", [(DLL_UNLOAD, "                edvr::breadcrumb(\"gfx: FreeLibrary unload\");")], "DllMain's FreeLibrary path leaves the heartbeat open"),
    M("close-function-does-nothing", ("H7", "H10"), "proxy", [(HB_CLOSE_FN, "void breadcrumbHeartbeatClose() {\n}\n")], "breadcrumbHeartbeatClose does not close"),
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
    """Every check label the rig printed on a 'FAIL: H<case>.<what>' line (the text up to ' -- ', ' [' or the end of the line); the rig's
    closing 'FAIL: heartbeat writer: n of m checks failed' line is not a label."""
    labels = []
    for line in output.splitlines():
        if line.startswith("FAIL: "):
            label = re.split(r" -- | \[", line[len("FAIL: "):], 1)[0].strip()
            if re.match(r"H\d+\.", label):
                labels.append(label)
    return labels


def rig_cases(rig_text):
    """The case ids the rig's checks are labelled with."""
    return set(re.findall(r'"(H\d+)[.:]', rig_text))


def label_block(bat_text, label):
    """The lines of one build.bat rig, continuation lines (a trailing ^) joined, from its :rig_ label to the next :rig_ label; None when the
    label is absent."""
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


def make_tree(dest, mutated):
    """The sources the rig compiles and the headers they include, under `dest` laid out as in the repository, `mutated` replacing the text of
    some of the three files this tool edits."""
    # All of src\common, whole: config.h alone includes headers the rig never names (ini_name.h), and a list of them
    # here would be one more thing to drift. The mutated files are written over their copies.
    shutil.copytree(ROOT / "src" / "common", dest / "src" / "common")
    for key, path in FILES.items():
        if key in mutated and path.parent == ROOT / "src" / "common":
            (dest / "src" / "common" / path.name).write_text(mutated[key], encoding="utf-8", newline="\n")
    dll = dest / FILES["dll"].relative_to(ROOT)
    dll.parent.mkdir(parents=True, exist_ok=True)
    dll.write_text(mutated.get("dll", read_source(FILES["dll"])), encoding="utf-8", newline="\n")


def build_rig(tc, tree, outdir):
    """Compile the rig and the common sources against the tree; (exit code, output)."""
    common = tree / "src" / "common"
    cmd = ([tc.cl] + CL_FLAGS + ["/I" + str(common), "/Fo" + str(outdir) + "\\", "/Fe" + str(outdir / "heartbeat_writer_test.exe"), str(RIG)] +
           [str(common / n) for n in COMMON_SOURCES + ["proxy.cpp"]] + ["/link", "/INCREMENTAL:NO"] + LIBS)
    done = subprocess.run(cmd, capture_output=True, text=True, env=tc.env, errors="replace", cwd=str(outdir))
    return done.returncode, done.stdout + done.stderr


def run_rig(exe, root, tc):
    """(outcome, labels, tail) of one run of the rig against the repository root `root`: 'pass', 'fail', 'crash', 'timeout'."""
    try:
        done = subprocess.run([str(exe), "--self-test", str(root)], capture_output=True, text=True, env=tc.env, errors="replace", timeout=RUN_TIMEOUT,
                              cwd=str(exe.parent))
    except subprocess.TimeoutExpired:
        return "timeout", [], "no result within %d s" % RUN_TIMEOUT
    out = done.stdout + "\n" + done.stderr
    if done.returncode == 0:
        return "pass", [], ""
    labels = fail_labels(out)
    if not labels:
        return "crash", [], "exit 0x%08X" % (done.returncode & 0xFFFFFFFF)
    return "fail", labels, ""


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
        lines.append("  %-38s %-6s caught by %-7s %s" % (m.name, m.file, "/".join(m.caught), m.why))
    lines.append("cl.exe flags: " + " ".join(CL_FLAGS))
    return "\n".join(lines)


def run_all(only=None, jobs=2, keep=False, dry_run=False, verbose=False, out=sys.stdout):
    mutants = select(only)
    if dry_run:
        print(plan_text(mutants), file=out)
        print("dry-run: nothing compiled, started or written", file=out)
        return 0
    tc = Toolchain()
    sources = {k: read_source(p) for k, p in FILES.items()}
    work = Path(tempfile.mkdtemp(prefix="hbm_"))
    try:
        control = work / "k"
        make_tree(control, {})
        cdir = work / "kx"
        cdir.mkdir()
        code, text = build_rig(tc, control, cdir)
        if code != 0:
            print("control: nocompile\n" + text.strip()[-1500:], file=out)
            return 1
        outcome, labels, tail = run_rig(cdir / "heartbeat_writer_test.exe", ROOT, tc)
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
            tree = d / "t"
            make_tree(tree, {m.file: mutated})
            mdir = d / "x"
            mdir.mkdir()
            code, text = build_rig(tc, tree, mdir)
            if code != 0:
                return m, "nocompile", (text.strip().splitlines() or [""])[-1]
            outcome, labels, tail = run_rig(mdir / "heartbeat_writer_test.exe", tree, tc)
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
                print("%-9s %-38s %-7s %s" % (verdict, m.name, "/".join(m.caught), detail if verbose else detail[:150]), file=out)
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

    check(fail_labels("a\nFAIL: H6.drain -- x [y]\nFAIL: H2.order\nFAIL: heartbeat writer: 3 of 9\n") == ["H6.drain", "H2.order"],
          "fail_labels reads the label of every check's FAIL: line, and not the closing summary")
    check(fail_labels("PASS: 12 heartbeat writer checks\n") == [], "fail_labels reads nothing from a pass")
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
    check(rig_cases('check(x, "H1.a: b"); check(y, "H10.c")') == {"H1", "H10"}, "rig_cases reads the case ids off the labels")

    sources = {k: read_source(p) for k, p in FILES.items()}
    rig = read_source(RIG)
    cases = rig_cases(rig)
    names = [m.name for m in MUTANTS]
    check(len(names) == len(set(names)), "mutation names are unique")
    check(len(MUTANTS) >= 20, "the mutation list did not shrink below 20 (%d)" % len(MUTANTS))
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
    check(covered >= cases - {"H8", "H9"}, "every case of the rig but the two process-death ones (H8, H9, which only the filter's closing pins reach) has a mutation: %s vs %s" % (sorted(covered), sorted(cases)))
    for name in COMMON_SOURCES + COMMON_HEADERS:
        check((ROOT / "src" / "common" / name).is_file(), "src\\common\\%s exists (the tree the rig is built against)" % name)

    bat = Path(build_bat).read_bytes().decode("utf-8", errors="replace")
    block = label_block(bat, RIG_LABEL)
    check(block is not None, "build.bat has %s" % RIG_LABEL)
    if block:
        text = "\n".join(block)
        cl = next((l for l in block if l.strip().lower().startswith("cl.exe")), "")
        for flag in CL_FLAGS:
            check(flag in cl, "build.bat's rig compile has %s" % flag)
        check('/I"src\\common"' in cl, "build.bat's rig finds the headers through /I src\\common (the mutated copies are found the same way)")
        for src in ["tools\\heartbeat_writer_test\\heartbeat_writer_test.cpp"] + ["src\\common\\" + n for n in COMMON_SOURCES + ["proxy.cpp"]]:
            check(src in cl, "build.bat's rig compile has %s (the sources this tool links)" % src)
        for lib in LIBS:
            check(lib in cl, "build.bat's rig link has %s" % lib)
        check("/Fe\"%OBJ%\\heartbeat\\heartbeat_writer_test.exe\"" in cl, "build.bat puts the rig's exe in its own obj directory (its breadcrumb file is written beside it)")
        check('heartbeat_writer_test.exe" --dry-run' in text and 'heartbeat_writer_test.exe" --self-test "%ROOT%"' in text,
              "build.bat runs the rig's --dry-run and --self-test with the repository root (the H10 pins read the sources from it)")
        check('mutants.py" --self-test' in text, "build.bat runs this tool's self-test")

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
    print("PASS: heartbeat_writer_test mutants.py self-test (%d mutations over %d cases, every anchor found once, build.bat wired)" % (len(MUTANTS), len(covered)))
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--run", action="store_true")
    parser.add_argument("--list", action="store_true")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--only", default="")
    parser.add_argument("--jobs", type=int, default=2)
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
