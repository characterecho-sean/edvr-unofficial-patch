#!/usr/bin/env python3
"""The mutation proof for tools\\vram_watch_test: the rig fails when a rule of the graphics memory watch is flipped.

The rig (vram_watch_test.cpp) holds the watch's rules (src\\common\\vram_watch.h: the policy, the lines, the machine the Present hook drives), the
DXGI read (src\\common\\vram_query.h) and, by source text, the glue that calls them (device_hook.cpp, vram_tick.cpp, build.bat). Every check carries a
label "V<case>.<what>". A rig that passes proves little until it is seen to FAIL on a source that breaks the rule it pins. This tool does that: for
each mutation below it copies the source it edits into a temp directory OUTSIDE the repo, applies one textual edit (or a few that belong together),
and runs the rig against that copy, requiring it to fail on a check of the case that belongs to the rule (a FAIL label starting with one of the
mutation's case ids). A mutated HEADER is compiled into a fresh rig; a mutated GLUE source (the V11 pins) is run through the unmutated rig, which is
handed a temp repository root holding the edited copy. Nothing is written inside the repo; the temp directory is removed at the end.

  python tools\\vram_watch_test\\mutants.py --self-test       text only: every anchor is found exactly once in the file it edits as it is now,
                                                            every case named is in the rig, and build.bat compiles the rig the way this tool
                                                            does (add --build-bat PATH to check another copy)
  python tools\\vram_watch_test\\mutants.py --run [--only a,b] [--jobs N] [--keep]
  python tools\\vram_watch_test\\mutants.py --list
  python tools\\vram_watch_test\\mutants.py --run --dry-run   the plan; writes nothing, starts nothing

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
    "watch": ROOT / "src" / "common" / "vram_watch.h",
    "query": ROOT / "src" / "common" / "vram_query.h",
    "hook": ROOT / "src" / "d3d11" / "device_hook.cpp",
    "glue": ROOT / "src" / "d3d11" / "vram_tick.cpp",
    "bat": ROOT / "build.bat",
}
HEADER_KEYS = ("watch", "query")          # compiled into the rig: a mutation of one rebuilds the rig
PIN_KEYS = ("hook", "glue", "bat")        # read by the rig as text: a mutation of one is run through the unmutated rig
# Everything the rig's headers include, laid out as the headers expect to find each other (all of it in src\common).
TREE = [
    ("src/common/vram_watch.h", ROOT / "src" / "common" / "vram_watch.h"),
    ("src/common/vram_query.h", ROOT / "src" / "common" / "vram_query.h"),
    ("src/common/system_d3d11.h", ROOT / "src" / "common" / "system_d3d11.h"),
]
INCLUDE_DIRS = ("src/common",)            # the rig's /I directories under the header tree (build.bat's compile line has the same)
RIG = HERE / "vram_watch_test.cpp"
BUILD_BAT = ROOT / "build.bat"
RIG_LABEL = ":rig_vram_watch_test"
RIG_SOURCE_IN_BAT = "tools\\vram_watch_test\\vram_watch_test.cpp"
RIG_EXE_IN_BAT = "vram_watch_test.exe"
CASE_PREFIX = "V"
CL_FLAGS = ["/nologo", "/O2", "/MT", "/std:c++17", "/EHsc", "/W4", "/DWIN32_LEAN_AND_MEAN", "/DNOMINMAX", "/D_CRT_SECURE_NO_WARNINGS"]
LINK_ARGS = []
MIN_MUTANTS = 40
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
A_SAMPLE = "constexpr uint64_t kVramSampleEveryMs = 1000;\n"
A_LINE = "constexpr uint64_t kVramLineEveryMs = 30000;\n"
A_PRESS_EVERY = "constexpr uint64_t kVramPressureEveryMs = 5000;\n"
A_PERCENT = "constexpr uint64_t kVramPressurePercent = 90;\n"
A_CAP = "constexpr uint32_t kVramCadenceLineCap = 3000;\n"
A_PRESSURE_PRED = "    return s.valid && s.budget > 0 && s.usage * 100ull >= s.budget * kVramPressurePercent;\n"
A_OVER_PRED = "inline bool vramOverBudget(const VramSegment& s) { return s.valid && s.budget > 0 && s.usage > s.budget; }\n"
A_PCT_ZERO = "    if (!s.valid || s.budget == 0) return -1;\n"
A_PCT_ROUND = "    return static_cast<int>(tenths + 0.5);\n"
A_VALID = "        if (!f.local.valid) return VramReason::None;\n"
A_ARM = "            lastMs_ = nowMs;\n            return VramReason::None;\n        }\n        VramReason r = VramReason::None;\n"
A_CROSS = "        if (over != over_) {\n"
A_CROSS_COUNT = "            ++crossings_;\n"
A_ENTER = "        } else if (pressure && !pressure_) {\n"
A_BACKWARDS = ("            if (nowMs < lastMs_) lastMs_ = nowMs;   // a clock that went backwards restarts the interval\n"
               "            else if (nowMs - lastMs_ >= every) r = pressure ? VramReason::Pressure : VramReason::Periodic;\n")
A_EVERY = "            const uint64_t every = pressure ? kVramPressureEveryMs : kVramLineEveryMs;\n"
A_KIND = "r = pressure ? VramReason::Pressure : VramReason::Periodic;\n"
A_CAPCHECK = "            if (cadenceLines_ >= kVramCadenceLineCap) {\n"
A_CAPONCE = "                if (capNoted_) return VramReason::None;\n"
A_CAPCOUNT = "            ++cadenceLines_;\n"
A_CAPCOND = "        if (r == VramReason::Periodic || r == VramReason::Pressure) {\n"
A_LASTMS = "        lastMs_ = nowMs;\n        return r;\n    }\n"
A_MB = "    std::snprintf(out, size, \"%llu\", static_cast<unsigned long long>(bytes / (1024ull * 1024ull)));\n"
A_DASH = "    if (!s.valid) {\n        std::snprintf(out, size, \"-\");\n"
A_FIG = "\"%slocal_used_mb=%s%c%slocal_budget_mb=%s%c%slocal_pct=%s%c%snonlocal_used_mb=%s%c%snonlocal_budget_mb=%s%c%snonlocal_pct=%s\""
A_FIGSIG = "inline size_t formatVramFigures(char* buf, size_t cap, const VramFigures& f, char sep = ' ', const char* prefix = \"\") {\n"
A_LINEFMT = "\"vram: reason=%s %s\""
A_ARMED = "\"vram: reason=armed %s; adapter %s; this process's"
A_UNAVAIL = "\"vram: unavailable -- %s. No vram: line will be written this session.\""
A_CAPLINE = "\"vram: reason=cap_reached %s; %u periodic and pressure lines have been written this session, which is the \""
A_NOBUDGET = "\" The OS reports no local budget, so no percentage and no crossing can be judged.\""
A_CUT = "    return static_cast<size_t>(n) < cap ? static_cast<size_t>(n) : cap - 1;\n"
W_DUE = "    bool due(int64_t qpc) const { return !unavailable_ && qpc >= nextQpc_; }\n"
W_NEXT = "        nextQpc_ = qpc + qpcPerSecond * static_cast<int64_t>(kVramSampleEveryMs) / 1000;\n"
W_UNAV1 = "                write(static_cast<const char*>(line));\n                unavailable_ = true;\n                return;\n            }\n            std::snprintf(adapter_"
W_UNAV2 = "                write(static_cast<const char*>(line));\n                unavailable_ = true;\n                return;\n            }\n            policy_.observe(nowMs, figures);"
W_ARMPOL = "            policy_.observe(nowMs, figures);   // arms the policy; the line below is the first of its cadence\n"
W_NOTED = "                failureNoted_ = true;\n"
W_NOLINE = "        const VramReason reason = policy_.observe(nowMs, figures);\n        if (reason == VramReason::None) return;\n"
W_NAME = "            std::snprintf(adapter_, sizeof(adapter_), \"%s\", opening.adapter);\n"
Q_NODE = "    const HRESULT result = adapter->QueryVideoMemoryInfo(0, group, &info);\n"
Q_NL = "    f.nonLocal = vramReadSegment(adapter, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, hr);\n"
Q_FIELDS = "    s.usage = info.CurrentUsage;\n    s.budget = info.Budget;\n"
Q_NODEV = "        *reason = \"the device has no IDXGIDevice\";\n"
Q_NOADAPTER = "        *reason = \"the device reports no DXGI adapter\";\n"
Q_NOA3 = "\"IDXGIAdapter3 is not offered, and QueryVideoMemoryInfo needs it"
Q_REL1 = "    dxgiDevice->Release();\n"
Q_REL2 = "    adapter->Release();\n"
Q_CUT = "                    while (cut > 0 && (static_cast<unsigned char>(utf8[cut]) & 0xC0) == 0x80) --cut;   // not inside a character\n"
Q_TERM = "                name[cut] = 0;\n"
Q_CLEAR = "    if (name && nameSize) name[0] = 0;\n"
H_BEAT = "    stallWatchBeat(presentT1, g_state->frameCounter);\n"
H_TICK = "    vramWatchTick(presentT1, g_state->device);\n"
G_DUE = "    if (!g_watcher.due(qpc)) return;\n"
G_BUDGET = "    guardedBudget(g_budget, [&] {\n"
B_LIST = "\"src\\d3d11\\stall_watch.cpp\" \"src\\d3d11\\vram_tick.cpp\" ^\n"

MUTANTS = [
    # ---- V1: constants and keys ------------------------------------------------------------------------------------------------
    M("sample-every-two-seconds", ("V1", "V9"), "watch", [(A_SAMPLE, "constexpr uint64_t kVramSampleEveryMs = 2000;\n")], "the OS is asked every two seconds"),
    M("line-every-minute", ("V1", "V3"), "watch", [(A_LINE, "constexpr uint64_t kVramLineEveryMs = 60000;\n")], "a line every minute, not every 30 s"),
    M("pressure-every-ten-seconds", ("V1", "V4"), "watch", [(A_PRESS_EVERY, "constexpr uint64_t kVramPressureEveryMs = 10000;\n")], "pressure lines every 10 s"),
    M("pressure-from-95", ("V1", "V2", "V4"), "watch", [(A_PERCENT, "constexpr uint64_t kVramPressurePercent = 95;\n")], "pressure starts at 95%, not 90%"),
    M("cap-four-thousand", ("V1", "V6"), "watch", [(A_CAP, "constexpr uint32_t kVramCadenceLineCap = 4000;\n")], "the cadence cap is 4000"),
    # ---- V2: the predicates ----------------------------------------------------------------------------------------------------
    M("pressure-needs-more-than-90", "V2", "watch", [(A_PRESSURE_PRED, "    return s.valid && s.budget > 0 && s.usage * 100ull > s.budget * kVramPressurePercent;\n")],
      "exactly 90% is not pressure"),
    M("equal-is-over", "V2", "watch", [(A_OVER_PRED, "inline bool vramOverBudget(const VramSegment& s) { return s.valid && s.budget > 0 && s.usage >= s.budget; }\n")],
      "usage equal to the budget is over it"),
    M("percent-without-budget", "V2", "watch", [(A_PCT_ZERO, "    if (!s.valid) return -1;\n")], "a budget of 0 divides: the percentage is nonsense, not unknown"),
    M("percent-truncates", "V2", "watch", [(A_PCT_ROUND, "    return static_cast<int>(tenths);\n")], "two thirds prints 66.6"),
    # ---- V3: the cadence ---------------------------------------------------------------------------------------------------------
    M("arm-writes-a-line", "V3", "watch", [(A_ARM, "            lastMs_ = nowMs;\n            return VramReason::Periodic;\n        }\n        VramReason r = VramReason::None;\n")],
      "the first sample returns a line, so the armed line is written twice"),
    M("interval-from-boot", ("V3", "V9"), "watch", [(A_LASTMS, "        return r;\n    }\n")], "a line does not restart the interval: after one every sample is another"),
    # ---- V4: pressure ------------------------------------------------------------------------------------------------------------
    M("pressure-waits-for-the-cadence", "V4", "watch", [(A_ENTER, "        } else if (false) {\n")], "reaching 90% writes nothing until the next cadence line"),
    M("pressure-cadence-is-thirty-seconds", "V4", "watch", [(A_EVERY, "            const uint64_t every = kVramLineEveryMs;\n")], "while at pressure the lines still come every 30 s"),
    M("pressure-lines-are-periodic", "V4", "watch", [(A_KIND, "r = VramReason::Periodic;\n")], "a line at pressure says periodic"),
    # ---- V5: crossings -----------------------------------------------------------------------------------------------------------
    M("back-under-is-silent", "V5", "watch", [(A_CROSS, "        if (over && !over_) {\n")], "going over the budget is written, coming back under is not"),
    M("crossings-not-counted", "V5", "watch", [(A_CROSS_COUNT, "")], "the count of crossings stays at zero"),
    # ---- V6: the cap -------------------------------------------------------------------------------------------------------------
    M("cap-off-by-one", "V6", "watch", [(A_CAPCHECK, "            if (cadenceLines_ > kVramCadenceLineCap) {\n")], "3001 lines are written before the cap"),
    M("cap-notice-repeats", "V6", "watch", [(A_CAPONCE, "")], "the cap notice is written for every line past the cap"),
    M("cap-never-counts", "V6", "watch", [(A_CAPCOUNT, "")], "the cadence lines are never counted, so the cap never comes"),
    M("crossings-held-to-the-cap", "V6", "watch", [(A_CAPCOND, "        if (r != VramReason::None) {\n")], "a crossing after the cap is not written"),
    # ---- V7: robustness ----------------------------------------------------------------------------------------------------------
    M("invalid-sample-arms", "V7", "watch", [(A_VALID, "")], "a sample with no figures arms the policy and is judged as zeros"),
    M("clock-back-fires", "V7", "watch", [(A_BACKWARDS, "            if (nowMs - lastMs_ >= every) r = pressure ? VramReason::Pressure : VramReason::Periodic;\n")],
      "a clock that went backwards makes the elapsed time enormous: a line at once"),
    # ---- V8: the text ------------------------------------------------------------------------------------------------------------
    M("megabytes-are-millions", "V8", "watch", [(A_MB, "    std::snprintf(out, size, \"%llu\", static_cast<unsigned long long>(bytes / 1000000ull));\n")], "figures in millions of bytes, not MiB"),
    M("no-answer-prints-zero", "V8", "watch", [(A_DASH, "    if (!s.valid) {\n        std::snprintf(out, size, \"0\");\n")], "a segment the OS did not answer for prints 0"),
    M("figures-reordered", "V8", "watch", [(A_FIG, "\"%slocal_budget_mb=%s%c%slocal_used_mb=%s%c%slocal_pct=%s%c%snonlocal_used_mb=%s%c%snonlocal_budget_mb=%s%c%snonlocal_pct=%s\"")],
      "the local budget and usage are swapped in the line"),
    M("figures-default-prefix", "V8", "watch", [(A_FIGSIG, "inline size_t formatVramFigures(char* buf, size_t cap, const VramFigures& f, char sep = ' ', const char* prefix = \"g_\") {\n")],
      "the graphics log's figures carry a key prefix by default"),
    M("figures-default-comma", "V8", "watch", [(A_FIGSIG, "inline size_t formatVramFigures(char* buf, size_t cap, const VramFigures& f, char sep = ',', const char* prefix = \"\") {\n")],
      "the graphics log's figures are comma-separated by default"),
    M("line-head-changed", "V8", "watch", [(A_LINEFMT, "\"vram: reason=%s; %s\"")], "the line has a semicolon after its reason"),
    M("armed-renamed", "V8", "watch", [(A_ARMED, "\"vram: reason=start %s; adapter %s; this process's")], "the armed line has another reason word"),
    M("unavailable-without-the-promise", "V8", "watch", [(A_UNAVAIL, "\"vram: unavailable -- %s.\"")], "the unavailable line does not say no vram: line will follow"),
    M("cap-line-renamed", "V8", "watch", [(A_CAPLINE, "\"vram: reason=cap %s; %u periodic and pressure lines have been written this session, which is the \"")],
      "the cap notice has another reason word"),
    M("no-budget-unsaid", "V8", "watch", [(A_NOBUDGET, "\"\"")], "a budget the OS did not report is not said in the armed line"),
    M("overlong-line-not-cut", "V8", "watch", [(A_CUT, "    return static_cast<size_t>(n);\n")], "a line longer than its buffer reports the length it wanted"),
    # ---- V9: the machine ---------------------------------------------------------------------------------------------------------
    M("unavailable-asked-again", "V9", "watch", [(W_DUE, "    bool due(int64_t qpc) const { return qpc >= nextQpc_; }\n")], "after the unavailable line the hook still asks every second"),
    M("sample-every-frame", "V9", "watch", [(W_NEXT, "        nextQpc_ = qpc;\n")], "the OS is asked at every Present"),
    M("unavailable-repeats", "V9", "watch", [(W_UNAV1, "                write(static_cast<const char*>(line));\n                return;\n            }\n            std::snprintf(adapter_")],
      "an adapter that cannot be opened is tried, and said, at every sample"),
    M("first-read-failure-repeats", "V9", "watch", [(W_UNAV2, "                write(static_cast<const char*>(line));\n                return;\n            }\n            policy_.observe(nowMs, figures);")],
      "a first reading that fails does not stop the watch"),
    M("armed-sample-unobserved", "V9", "watch", [(W_ARMPOL, "")], "the first reading does not arm the policy: the first periodic line is a sample late"),
    M("failure-said-every-time", "V9", "watch", [(W_NOTED, "")], "a failed read is written at every sample"),
    M("policy-never-written", "V9", "watch", [(W_NOLINE, "        const VramReason reason = policy_.observe(nowMs, figures);\n        if (reason == VramReason::None || reason != VramReason::None) return;\n")],
      "the policy's lines are never written"),
    M("armed-without-the-adapter", "V9", "watch", [(W_NAME, "            adapter_[0] = 0;\n")], "the armed line does not name the adapter"),
    # ---- V10: the DXGI read ------------------------------------------------------------------------------------------------------
    M("read-node-one", "V10", "query", [(Q_NODE, "    const HRESULT result = adapter->QueryVideoMemoryInfo(1, group, &info);\n")], "the second GPU's node is read"),
    M("non-local-reads-local", "V10", "query", [(Q_NL, "    f.nonLocal = vramReadSegment(adapter, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, hr);\n")], "the non-local figures are the local ones"),
    M("usage-and-budget-swapped", "V10", "query", [(Q_FIELDS, "    s.usage = info.Budget;\n    s.budget = info.CurrentUsage;\n")], "usage and budget are read the wrong way round"),
    M("no-device-text", "V10", "query", [(Q_NODEV, "        *reason = \"device lacks an interface\";\n")], "a device that is not an IDXGIDevice is not said"),
    M("no-adapter-text", "V10", "query", [(Q_NOADAPTER, "        *reason = \"device problem\";\n")], "a device with no adapter is not said"),
    M("no-adapter3-text", "V10", "query", [(Q_NOA3, "\"adapter3 missing, and QueryVideoMemoryInfo needs it")], "the missing IDXGIAdapter3 is not named"),
    M("device-reference-leaked", "V10", "query", [(Q_REL1, "")], "the IDXGIDevice reference taken on the way is not given back"),
    M("adapter-reference-leaked", "V10", "query", [(Q_REL2, "")], "the IDXGIAdapter reference taken on the way is not given back"),
    M("name-cut-in-a-character", "V10", "query", [(Q_CUT, "")], "a name that does not fit is cut in the middle of a character"),
    M("name-not-terminated", "V10", "query", [(Q_TERM, "")], "a name that does not fit is not terminated"),
    M("name-not-cleared", "V10", "query", [(Q_CLEAR, "")], "a failed open leaves the caller's name buffer as it was"),
    # ---- V11: the glue -----------------------------------------------------------------------------------------------------------
    M("glue-hook-no-tick", "V11", "hook", [(H_TICK, "")], "hookedPresent never ticks the watch"),
    M("glue-hook-before-the-beat", "V11", "hook", [(H_TICK, ""), (H_BEAT, H_TICK + H_BEAT)], "the watch is ticked before the stall sampler's beat"),
    M("glue-hook-vr-only", "V11", "hook", [(H_TICK, "    if (!runtimeFlatProfile()) vramWatchTick(presentT1, g_state->device);\n")], "the flat profile does not get the watch"),
    M("glue-hook-twice", "V11", "hook", [(H_TICK, H_TICK + H_TICK)], "the watch is ticked twice a frame"),
    M("glue-no-cheap-question", "V11", "glue", [(G_DUE, "")], "the glue enters the fault guard on every frame"),
    M("glue-no-fault-budget", "V11", "glue", [(G_BUDGET, "    [&] {\n")], "the sample runs outside the fault budget"),
    M("glue-not-in-the-build", "V11", "bat", [(B_LIST, "\"src\\d3d11\\stall_watch.cpp\" ^\n")], "build.bat does not compile the glue"),
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
    closing 'FAIL: vram watch: n of m checks failed' line is not a label."""
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
    work = Path(tempfile.mkdtemp(prefix="vwm_"))
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
    check(fail_labels("a\nFAIL: V4.flight -- x [y]\nFAIL: V2.sub\nFAIL: vram watch: 3 of 9\n") == ["V4.flight", "V2.sub"],
          "fail_labels reads the label of every check's FAIL: line, and not the closing summary")
    check(fail_labels("PASS: 12 vram watch checks\n") == [], "fail_labels reads nothing from a pass")
    check(apply_edits("a b c", [("b", "B")]) == "a B c", "apply_edits applies one edit")
    for bad, why in ((("a b b", [("b", "B")]), "a repeated anchor"), (("a b", [("x", "y")]), "a missing anchor"), (("a b", [("b", "b")]), "a no-op edit")):
        try:
            apply_edits(*bad)
            check(False, "apply_edits refuses " + why)
        except ValueError:
            pass
    joined = label_block(":rig_a\nx ^\n y\nexit /b 0\n:rig_b\nz\n", ":rig_a")
    check(joined is not None and [" ".join(l.split()) for l in joined] == ["x y", "exit /b 0"], "label_block joins continuations and stops at the next label")
    check(rig_cases('check(x, "V1.a: b"); check(y, "V10.c")') == {"V1", "V10"}, "rig_cases reads the case ids off the labels")

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
              "build.bat runs the rig's --dry-run and --self-test with the repository root (the V11 pins read the glue's sources from it)")
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
    print("PASS: vram_watch_test mutants.py self-test (%d mutations over %d cases, every anchor found once, build.bat wired)" % (len(MUTANTS), len(covered)))
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
