#!/usr/bin/env python3
"""The mutation proof for tools\\terrain_checkerboard_test: the rig fails when a rule of the VR terrain checkerboard hint, or a wiring pin, is broken.

The rig (terrain_checkerboard_test.cpp) pins the words and the pure picks (T1, T2), the reader on fixture folders (T3), the monitor's rule for a
half-written file (T4), the worker thread on a real log (T5) and the wiring in menu.cpp, the worker and the headers as source pins (T6). A rig that
passes proves little until it is seen to FAIL on a source that breaks the rule it pins. This tool does that, for three kinds of mutation:

  code    one textual edit of the words header or the reader header, in a temp copy of the sources OUTSIDE the repo; the rig is built against the
          edited copies (-DTCB_NOTICE_HEADER, -DTCB_READER_HEADER, -DTCB_MUTANT, which leaves the worker out) and run on the rule's case;
  worker  one textual edit of the worker's source (terrain_checkerboard.cpp), the rig built with it and the real Config, Log and fault guard, and run
          on T5 (the thread, the real log and tools\\edvr_log.py);
  pin     one textual edit of a COPY of a source file the rig's pins read (menu.cpp, the worker, the headers, the flat runtime, the proxy, the device
          hook), and the rig (built once, from the real sources) run on T6 with --root.

Each must make the rig fail on a check whose label starts with the id the mutation names. Nothing is written inside the repo; the temp directory is
removed at the end.

  python tools\\terrain_checkerboard_test\\mutants.py --self-test        text only: every anchor is found exactly once in its source as it is now, every
                                                                       label named is in the rig, and build.bat compiles the rig the way this tool does
  python tools\\terrain_checkerboard_test\\mutants.py --run [--only a,b] [--jobs N] [--keep]
  python tools\\terrain_checkerboard_test\\mutants.py --list
  python tools\\terrain_checkerboard_test\\mutants.py --run --dry-run      the plan; writes nothing, starts nothing

--run needs the MSVC toolchain (cl.exe on PATH, or Visual Studio found with vswhere as build.bat does) and python on PATH (T5 runs the log reader);
it takes about a minute. --self-test runs in build.bat's rig and is what keeps an edit of a source from silently orphaning a mutation: if an anchor
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
NOTICE = "src/common/terrain_checkerboard_notice.h"
READER = "src/d3d11/terrain_checkerboard_reader.h"
WORKER = "src/d3d11/terrain_checkerboard.cpp"
WORKER_H = "src/d3d11/terrain_checkerboard.h"
MENU = "src/d3d11/menu.cpp"
FLAT_RUNTIME = "src/d3d11/flat_runtime.cpp"
FLAT_SETTINGS = "src/d3d11/flat_elite_settings.h"
PROXY = "src/d3d11/d3d11_proxy.cpp"
HOOK = "src/d3d11/device_hook.cpp"
RIG = HERE / "terrain_checkerboard_test.cpp"
BUILD_BAT = ROOT / "build.bat"
RIG_LABEL = ":rig_terrain_checkerboard_test"

# How build.bat compiles the rig; --self-test checks that the label still says the same.
CL_FLAGS = ["/nologo", "/O2", "/MT", "/std:c++17", "/EHsc", "/W4", "/DWIN32_LEAN_AND_MEAN", "/DNOMINMAX", "/D_CRT_SECURE_NO_WARNINGS"]
LINK_LIBS = ["kernel32.lib", "user32.lib", "version.lib"]
REAL_SOURCES = ["src/common/config.cpp", "src/common/log.cpp", "src/common/guard.cpp", "src/common/proxy.cpp"]
RUN_TIMEOUT = 180.0

# The files a temp tree holds for each kind (a mutation edits one of them; the rest are copies). Relative paths inside it are the repo's, so the
# sources' relative includes resolve inside the temp tree.
HEADER_FILES = [NOTICE, READER, FLAT_SETTINGS, "src/d3d11/flat_mono_frame.h", "src/d3d11/flat_temporal_model.h", "src/common/elite_graphics_folder.h"]
WORKER_FILES = HEADER_FILES + [WORKER, WORKER_H, "src/common/config.h", "src/common/ini_name.h", "src/common/runtime_profile.h", "src/common/guard.h", "src/common/log.h"]
PIN_FILES = [NOTICE, READER, WORKER, WORKER_H, FLAT_SETTINGS, MENU, FLAT_RUNTIME, PROXY, HOOK]


class Mutant:
    def __init__(self, name, caught, edits, why, target):
        self.name = name
        self.caught = tuple([caught] if isinstance(caught, str) else caught)   # label prefixes that count as caught by its rule
        self.edits = list(edits)                                                # (old, new) pairs, applied in order
        self.why = why
        self.target = target                                                    # the file edited
        self.rule = re.match(r"T\d+", self.caught[0]).group(0)                  # the rig's case to run
        if name.startswith("pin-") or target not in (NOTICE, READER, WORKER):
            self.kind = "pin"
        else:
            self.kind = "worker" if target == WORKER else "code"


def M(name, caught, edits, why, target):
    return Mutant(name, caught, edits, why, target)


N, R, W = NOTICE, READER, WORKER
MUTANTS = [
    # ---- T1: the words and the constants ------------------------------------------------------------------------------------------
    M("toast-too-long", "T1b", [('"Distant terrain shimmers: turn off terrain checkerboard"', '"Distant terrain shimmers a lot: turn off terrain checkerboard"')], "the toast is past 55 characters", N),
    M("toast-names-nothing", "T1b", [('"Distant terrain shimmers: turn off terrain checkerboard"', '"Distant terrain shimmers: turn off checkerboard"')], "the toast no longer says terrain checkerboard", N),
    M("hint-too-long", "T1c", [('"Turn off terrain checkerboard rendering in Elite\'s graphics options."', '"Turn off terrain checkerboard rendering in Elite\'s graphics options, then restart the game."')], "the hint is past 78 characters", N),
    M("sentence-no-instruction", "T1d", [(" Turn it off in Elite's graphics options.\");", "\");")], "the sentence says what it does and not what to set", N),
    M("sentence-parenthesis", "T1d", [("makes distant terrain shimmer with DLSS.", "makes distant terrain shimmer (with DLSS).")], "a parenthesis in the sentence, which ends the log reader's detail early", N),
    M("on-line-prefix", "T1e", [('"vr terrain checkerboard: ON (preset', '"vr terrain checkerboard: On (preset')], "the ON line's prefix is not the one the log reader looks for", N),
    M("off-line-detail", "T1e", [('"vr terrain checkerboard: OFF (preset %s, %s: TerrainCheckerboardRenderingEnabled=%s): no notice."', '"vr terrain checkerboard: OFF (preset %s, %s: TerrainCheckerboardRenderingEnabled=%s)."')], "the OFF line does not say there is no notice", N),
    M("unknown-says-notice", "T1e", [('"vr terrain checkerboard: unknown (%s): no notice."', '"vr terrain checkerboard: unknown (%s): notice raised."')], "an unknown read says a notice was raised", N),
    M("start-line-words", "T1f", [('"vr terrain checkerboard: reading Elite\'s graphics settings on its own thread', '"vr terrain checkerboard: reading Elite\'s graphics settings on this thread')], "the start line no longer says its own thread", N),
    M("limit-line-count", "T1f", [('no longer logged.", kLogMax);', 'no longer logged.", kLogMax + 1);')], "the bound's line names another count", N),
    M("queued-note-always-toast", "T1f", [('"vr terrain checkerboard: menu.toasts is off, so no toast; the Status page shows the advice as its hint while the menu is open."',
                                           '"vr terrain checkerboard: the headset notice is queued as a toast; the Status page shows the advice as its hint while the menu is open."')],
      "with menu.toasts off the note still says a toast was queued", N),
    M("poll-1s", "T1a", [("constexpr uint32_t kPollMs = 3000;", "constexpr uint32_t kPollMs = 1000;")], "the worker reads every second", N),
    M("one-unknown-read", "T1a", [("constexpr int kUnknownReads = 2;", "constexpr int kUnknownReads = 1;")], "one bad read is enough to leave a known state", N),
    M("log-max-32", "T1a", [("constexpr int kLogMax = 16;", "constexpr int kLogMax = 32;")], "the log's bound moved", N),
    # ---- T2: the pure picks -----------------------------------------------------------------------------------------------------
    M("pack-version-lost", "T2a", [("return (version & 0xFFFFFFu) << 8 | static_cast<uint32_t>(state);", "return static_cast<uint32_t>(state);")], "the published word carries no version", N),
    M("next-version-zero", "T2a", [("    return v ? v : 1u;", "    return v;")], "the version wraps to 0, which a latch that starts at 0 would take for 'never'", N),
    M("version-not-counted", "T2a", [("const uint32_t v = (unpackVersion(word) + 1u) & 0xFFFFFFu;", "const uint32_t v = unpackVersion(word) & 0xFFFFFFu;")], "the version does not go up", N),
    M("toast-every-frame", "T2b", [("if (state != State::On || version == *toasted) return false;", "if (state != State::On) return false;")], "the toast is said every frame the option is on", N),
    M("toast-on-unknown", "T2b", [("if (state != State::On || version == *toasted) return false;", "if (state == State::Off || version == *toasted) return false;")], "an Unknown read toasts", N),
    M("toast-latch-not-set", "T2b", [("    *toasted = version;\n    return true;", "    return true;")], "the latch is not set by the function (the caller would have to)", N),
    M("hint-turns-3", "T2c", [("(nowMs / kHintAlternateMs) % 2u == 0u", "(nowMs / kHintAlternateMs) % 3u == 0u")], "the two hints do not take equal turns", N),
    M("hint-both-supersampling", "T2c", [("        return (nowMs / kHintAlternateMs) % 2u == 0u ? Hint::Supersampling : Hint::Checkerboard;", "        return Hint::Supersampling;")], "with both, the checkerboard hint never shows", N),
    M("hint-both-checkerboard", "T2c", [("        return (nowMs / kHintAlternateMs) % 2u == 0u ? Hint::Supersampling : Hint::Checkerboard;", "        return Hint::Checkerboard;")], "with both, the supersampling hint never shows", N),
    M("hint-checkerboard-alone-hidden", "T2c", [("    if (checkerboardApplies) return Hint::Checkerboard;", "    if (checkerboardApplies) return Hint::None;")], "the checkerboard hint is lost when it is the only one", N),
    M("hint-shows-when-neither", "T2c", [("    return Hint::None;\n}", "    return Hint::Checkerboard;\n}")], "the hint shows when nothing applies", N),
    M("log-unbounded", "T2d", [("if (*logged < kLogMax) { ++*logged; return LogDue::Line; }", "{ ++*logged; return LogDue::Line; }")], "no bound on the log", N),
    M("log-no-limit-line", "T2d", [("    if (*logged == kLogMax) { ++*logged; return LogDue::Limit; }\n", "")], "the bound is reached without saying so", N),
    # ---- T3: the reader on fixture folders ----------------------------------------------------------------------------------------------
    M("reads-settings-tag", ("T3a", "T3b", "T3i"), [("flatXmlText(stripXmlComments(xml), kTag, &text)", "flatXmlText(stripXmlComments(settings), kTag, &text)")], "the toggle is read from Settings.xml's own tag", R),
    M("lowest-version", "T3c", [("const int pick = flatPickCustomFxcfg(versioned);", "const int pick = versioned.empty() ? -1 : 0;")], "the first Custom file is read, not the highest version", R),
    M("lookalikes-allowed", "T3d", [("            if (flatCustomFxcfgVersion(f.name, &major, &minor)) versioned.push_back(f);", "            versioned.push_back(f);")], "a lookalike file is handed to the pick", R),
    M("no-custom-is-off", ("T3d", "T3e"), [('            setReason(&r, "preset Custom but the folder has no Custom.<major>.<minor>.fxcfg");', "            r.state = State::Off;")], "no Custom file reads as OFF", R),
    M("no-settings-is-off", "T3f", [('        setReason(&r, "Settings.xml is missing or unreadable, so the active preset is not known");', "        r.state = State::Off;")], "no Settings.xml reads as OFF", R),
    M("no-folder-is-off", "T3f", [(r'        setReason(&r, "no Elite graphics settings folder was found at Frontier Developments\\Elite Dangerous\\Options\\Graphics under LocalAppData");', "        r.state = State::Off;")],
      "no settings folder reads as OFF", R),
    M("no-preset-name-is-on", "T3f", [('        setReason(&r, "Settings.xml names no PresetName, so the active preset is not known");', "        r.state = State::On;")], "no PresetName reads as ON", R),
    M("tag-absent-is-off", "T3g", [('        setReason(&r, "%s has no %s", r.source, kTag);', "        r.state = State::Off;")], "an absent tag reads as OFF", R),
    M("tag-absent-is-on", "T3g", [('        setReason(&r, "%s has no %s", r.source, kTag);', "        r.state = State::On;")], "an absent tag reads as the game's inferred default, ON", R),
    M("value-yes-on", "T3b", [('if (_stricmp(text.c_str(), "true") == 0 || text == "1") r.state = State::On;', 'if (_stricmp(text.c_str(), "true") == 0 || text == "1" || _stricmp(text.c_str(), "yes") == 0) r.state = State::On;')], "yes is a value", R),
    M("value-one-lost", "T3b", [('if (_stricmp(text.c_str(), "true") == 0 || text == "1") r.state = State::On;', 'if (_stricmp(text.c_str(), "true") == 0) r.state = State::On;')], "1 is not on", R),
    M("value-true-case", "T3b", [('if (_stricmp(text.c_str(), "true") == 0 || text == "1") r.state = State::On;', 'if (text == "true" || text == "1") r.state = State::On;')], "True and TRUE are not on", R),
    M("false-is-on", "T3a", [('else if (_stricmp(text.c_str(), "false") == 0 || text == "0") r.state = State::Off;', 'else if (_stricmp(text.c_str(), "false") == 0 || text == "0") r.state = State::On;')], "false reads as ON", R),
    M("comments-read", "T3j", [("flatXmlText(stripXmlComments(xml), kTag, &text)", "flatXmlText(xml, kTag, &text)")], "a commented-out tag is read", R),
    M("settings-comments-read", "T3j", [('flatXmlText(stripXmlComments(settings), "PresetName", &preset)', 'flatXmlText(settings, "PresetName", &preset)')], "a commented-out PresetName is read", R),
    M("open-comment-kept", "T3j", [("            if (end == std::string::npos) break;", "            if (end == std::string::npos) { out += xml[i++]; continue; }")], "a comment that never ends does not take the rest of the file", R),
    M("stock-name-unchecked", "T3l", [("        if (!plausibleStockPreset(preset)) {", "        if (false) {")], "a preset name that could be a path is put in one", R),
    M("stock-name-allows-dot", "T3l", [("|| c == '_' || c == '-';", "|| c == '_' || c == '-' || c == '.' || c == '\\\\';")], "dots and backslashes are file-stem characters", R),
    M("stock-path-wrong", ("T3i", "T3k"), [(r'L"\\OptionDefaults\\"', r'L"\\OptionDefault\\"')], "the stock files are looked for in another folder", R),
    M("stock-missing-is-off", "T3l", [('            setReason(&r, "preset %s has no readable %s beside the game, so it is not a stock preset Elite ships", r.preset, r.source);', "            r.state = State::Off;")],
      "a stock preset with no OptionDefaults file reads as OFF", R),
    M("preset-ignored", ("T3i", "T3k"), [('    if (_stricmp(preset.c_str(), "Custom") == 0) {', "    if (true) {")], "every preset is read as Custom", R),
    M("custom-case-sensitive", "T3m", [('    if (_stricmp(preset.c_str(), "Custom") == 0) {', '    if (preset == "Custom") {')], "custom is not Custom", R),
    M("control-bytes-kept", ("T3l", "T3o"), [("        dst[i] = (c < 0x20 || c == 0x7F) ? '?' : static_cast<char>(c);", "        dst[i] = static_cast<char>(c);")], "a control byte from a file reaches the log's line", R),
    # ---- T4: the monitor's rule ---------------------------------------------------------------------------------------------------------
    M("monitor-no-retry", "T4c", [("        if (cur.state == State::Unknown && pub_.state != State::Unknown && ++strikes_ < kUnknownReads) return false;\n", "")], "one bad read of a half-written file leaves a known state", R),
    M("monitor-three-reads", "T4c", [("++strikes_ < kUnknownReads)", "++strikes_ < kUnknownReads + 1)")], "three bad reads are needed to leave a known state", R),
    M("monitor-count-kept", "T4e", [("        strikes_ = 0;\n        if (cur == pub_) return false;", "        if (cur == pub_) return false;")], "a bad read after a good one counts with the one before it", R),
    M("monitor-first-silent", "T4a", [("            if (stateMoved) *stateMoved = true;\n            return true;", "            if (stateMoved) *stateMoved = false;\n            return true;")], "the first read is not a new version", R),
    M("monitor-detail-silent", "T4h", [("        if (cur == pub_) return false;", "        if (cur.state == pub_.state) return false;")], "a change of preset or file with the same state is not logged", R),
    M("monitor-always-moved", "T4h", [("        const bool moved = cur.state != pub_.state;", "        const bool moved = true;")], "a change that is not a change of the state is published as one (a second toast)", R),
    M("monitor-first-not-kept", "T4i", [("        if (!have_) {\n            pub_ = cur;", "        if (!have_ && cur.state != State::Unknown) {\n            pub_ = cur;")], "a first read that finds nothing is not published", R),
    # ---- T5: the worker thread on a real log --------------------------------------------------------------------------------------------
    M("w-never-publishes", ("T5b", "T5c"), [("        g_published.store(tcn::pack(r.state, tcn::nextVersion(before)), std::memory_order_release);", "        (void)before;")], "the worker reads and logs but publishes nothing", W),
    M("w-version-stuck", ("T5c",), [("tcn::nextVersion(before)", "1u")], "the version never goes up, so a raise is never new", W),
    M("w-flat-starts", ("T5a",), [("    if (!runtimeVrProfile() || g_stopped.load(std::memory_order_relaxed)) return;", "    if (g_stopped.load(std::memory_order_relaxed)) return;")], "the flat profile starts the worker", W),
    M("w-start-after-shutdown", ("T5f",), [("    if (!runtimeVrProfile() || g_stopped.load(std::memory_order_relaxed)) return;", "    if (!runtimeVrProfile()) return;")], "a tick after the shutdown starts the worker", W),
    M("w-restarts-each-tick", ("T5c", "T5d"), [("    if (g_startTried.load(std::memory_order_relaxed)) return;\n", "")], "every tick starts another worker", W),
    M("w-shutdown-ignored", ("T5c",), [("    g_stopped.store(true, std::memory_order_relaxed);\n    if (Session* s = g_session.load(std::memory_order_acquire)) {\n        s->stop.store(true, std::memory_order_release);\n        SetEvent(s->wake);\n    }\n",
                                         "    g_stopped.store(true, std::memory_order_relaxed);\n")], "the shutdown does not stop the worker", W),
    M("w-no-start-line", ("T5b",), [('    tcn::formatStartLine(line, sizeof(line), GetCurrentThreadId(), s.pollMs);\n    Log::get().note("%s", line);\n', "")], "the worker does not say it started", W),
    M("w-logs-nothing", ("T5b",), [('        tcn::formatLog(line, sizeof(line), r.state, r.preset, r.source, r.value, r.reason);\n        Log::get().note("%s", line);\n', "        (void)r;\n")], "the worker publishes and logs no read", W),
    M("w-on-in-flat", ("T5c",), [("    return runtimeVrProfile() && tcn::unpackState(", "    return tcn::unpackState(")], "the live predicate says ON in the flat profile", W),
    # ---- T6: the wiring, as source pins -----------------------------------------------------------------------------------------------
    M("pin-menu-reads-file", "T6a", [("        terrainCheckerboardTick();\n        {\n            tcn::State cbState", "        terrainCheckerboardTick();\n        (void)tcn::readTerrainCheckerboard(L\"\", L\"\");\n        {\n            tcn::State cbState")],
      "the menu's tick reads the files itself", MENU),
    M("pin-menu-includes-reader", "T6a", [('#include "terrain_checkerboard.h"', '#include "terrain_checkerboard.h"\n#include "terrain_checkerboard_reader.h"')], "the menu includes the reader", MENU),
    M("pin-shutdown-dropped", "T6a", [("    terrainCheckerboardShutdown();\n", "")], "the menu's shutdown does not stop the worker", MENU),
    M("pin-field-lost", "T6a", [("    uint32_t    terrainCheckerboardToasted = 0;", "    bool        terrainCheckerboardToasted = false;")], "the latch is a flag, not the version that toasted", MENU),
    M("pin-tick-in-flat-branch", "T6b", [('            s.tickMs = now;\n            drainWrites();\n            if (s.summon.pressed()) {\n                if (s.open) closeMenu("the menu key");',
                                           '            s.tickMs = now;\n            drainWrites();\n            terrainCheckerboardTick();\n            if (s.summon.pressed()) {\n                if (s.open) closeMenu("the menu key");')],
      "the flat panel's tick starts the worker", MENU),
    M("pin-toast-latch-after-test", "T6b", [("            if (tcn::toastOnRaise(&s.terrainCheckerboardToasted, cbState, cbVersion)) {", "            if (s.toasts && tcn::toastOnRaise(&s.terrainCheckerboardToasted, cbState, cbVersion)) {")],
      "the latch is set only when menu.toasts is on, so turning toasts on later says it late", MENU),
    M("pin-toast-ignores-menu-toasts", "T6b", [("                if (s.toasts) s.toastQueue.push_back(terrainToast);", "                s.toastQueue.push_back(terrainToast);")], "the toast is queued whatever menu.toasts says", MENU),
    M("pin-note-dropped", "T6b", [('                Log::get().note("%s", terrainNote);', "                (void)terrainNote;")], "no line says the notice was queued", MENU),
    M("pin-note-one-branch", "T6b", [("tcn::formatQueuedLog(terrainNote, sizeof(terrainNote), s.toasts, terrainToast);", "tcn::formatQueuedLog(terrainNote, sizeof(terrainNote), true, terrainToast);")], "the note always says a toast was queued", MENU),
    M("pin-hint-line-added", "T6c", [("                tcn::formatStatusHint(c.hint, sizeof(c.hint));", '                statusLine(c, "Terrain checkerboard", "on");')], "the notice takes a line of the Status page (the bitmap's 2048 px guard)", MENU),
    M("pin-hint-latch-not-live", "T6c", [("terrainCheckerboardOn(), s.tickMs)", "s.terrainCheckerboardToasted != 0, s.tickMs)")], "the hint follows the toast's latch, which never ends", MENU),
    M("pin-hint-overwritten", "T6c", [("                tcn::formatStatusHint(c.hint, sizeof(c.hint));", "                tcn::formatStatusHint(c.hint, sizeof(c.hint));\n            vrss::formatStatusHint(c.hint, sizeof(c.hint));")],
      "the supersampling hint is written after this one and always wins the slot", MENU),
    M("pin-flat-runtime-mentions-it", "T6d", [('#include "flat_runtime.h"', '#include "flat_runtime.h"\n#include "terrain_checkerboard.h"')], "the flat runtime includes it", FLAT_RUNTIME),
    M("pin-flat-settings-reads-tag", "T6d", [("inline bool flatEndsWithNoCase(const std::string& s, const char* suffix) {", "// TerrainCheckerboardRenderingEnabled\ninline bool flatEndsWithNoCase(const std::string& s, const char* suffix) {")],
      "the flat panel's Elite settings know the tag", FLAT_SETTINGS),
    M("pin-start-from-dllmain", "T6d", [("BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved) {", "BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved) {\n    edvr::terrainCheckerboardTick();")],
      "the worker is started from DllMain", PROXY),
    M("pin-start-from-hook", "T6d", [("void presentFrameBoundary() {", "void presentFrameBoundary() {\n    terrainCheckerboardTick();")], "the worker is started from the device hook's frame boundary", HOOK),
    M("pin-worker-has-dllmain", "T6d", [("namespace edvr {\n\nnamespace {\n", "BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) { return TRUE; }\n\nnamespace edvr {\n\nnamespace {\n")], "the worker's source has a DllMain", WORKER),
    M("pin-worker-joins", "T6e", [("std::thread(workerMain, &s).detach();", "std::thread(workerMain, &s).join();")], "the worker is joined, which would block the frame", WORKER),
    M("pin-worker-no-try", "T6e", [("    try {\n        std::thread(workerMain, &s).detach();\n        return true;\n    } catch (...) {\n        return false;\n    }", "    std::thread(workerMain, &s).detach();\n    return true;")],
      "a thread that cannot be created is an exception on the render thread", WORKER),
    M("pin-tick-always-starts", "T6e", [("    if (g_startTried.load(std::memory_order_relaxed)) return;\n", "")], "the tick does not return at once when it has started", WORKER),
    M("pin-tick-no-vr-gate", "T6e", [("    if (!runtimeVrProfile() || g_stopped.load(std::memory_order_relaxed)) return;", "    if (g_stopped.load(std::memory_order_relaxed)) return;")], "the tick does not check the profile", WORKER),
    M("pin-no-stop-event", "T6e", [("        s->stop.store(true, std::memory_order_release);\n        SetEvent(s->wake);\n    }\n}\n\nvoid terrainCheckerboardTestSetPaths(", "        s->stop.store(true, std::memory_order_release);\n    }\n}\n\nvoid terrainCheckerboardTestSetPaths(")],
      "the shutdown does not wake the worker", WORKER),
    M("pin-no-exception-net", "T6e", [("    } catch (...) {\n        try {\n            char line[400];", "    } catch (int) {\n        try {\n            char line[400];"), ("        } catch (...) {\n        }\n    }\n    s.exited.store", "        } catch (int) {\n        }\n    }\n    s.exited.store")],
      "an exception leaves the worker's thread, which is std::terminate", WORKER),
    M("pin-no-fault-budget", "T6e", [("        if (!guardedBudget(s.budget, [&] { pass(monitor, &logged); }) && !s.budget.shouldRun()) {", "        pass(monitor, &logged);\n        if (false) {")], "a pass runs outside the fault budget", WORKER),
    M("pin-publish-before-log", "T6e", [("    char line[1100];\n    switch (tcn::logDue(logged)) {", "    g_published.store(tcn::pack(r.state, 1u), std::memory_order_release);\n    char line[1100];\n    switch (tcn::logDue(logged)) {")],
      "the state is published before the read is logged, so a toast line can come first", WORKER),
    M("pin-log-unbounded", "T6e", [("    switch (tcn::logDue(logged)) {", "    switch (tcn::LogDue::Line) {")], "the worker's log has no bound", WORKER),
    M("pin-publish-every-poll", "T6e", [("    if (stateMoved) {\n        const uint32_t before", "    if (true) {\n        const uint32_t before")], "every change is published as a new version, not only a change of the state", WORKER),
    M("pin-worker-reads-config", "T6e", [('#include "../common/config.h"', '#include "../common/config.h"\n// Config::get().getBool("terrain", false);')], "the worker reads a config key (the feature has none)", WORKER),
    M("pin-reader-settings-tag", "T6f", [("flatXmlText(stripXmlComments(xml), kTag, &text)", "flatXmlText(stripXmlComments(settings), kTag, &text)")], "the toggle is asked of Settings.xml", R),
    M("pin-reader-third-search", "T6f", [("    readerCopy(r.value, sizeof(r.value), text);", "    readerCopy(r.value, sizeof(r.value), text);\n    std::string again;\n    flatXmlText(settings, kTag, &again);")], "Settings.xml is asked for the tag as well", R),
    M("pin-reader-copies-listing", "T6f", [("    const EliteFolderListing listing = flatListEliteGraphics(folder);", "    WIN32_FIND_DATAW fd{};\n    FindFirstFileW(L\"\", &fd);\n    const EliteFolderListing listing = flatListEliteGraphics(folder);")],
      "the folder listing is written again here instead of called", R),
    M("pin-reader-pick-copied", "T6f", [("const int pick = flatPickCustomFxcfg(versioned);", "const int pick = versioned.empty() ? -1 : 0;")], "the highest-version pick is not the flat panel's helper", R),
    M("pin-reader-writes", "T6f", [("    std::string text;\n    if (!flatXmlText(stripXmlComments(xml), kTag, &text)) {", "    DeleteFileW(L\"\");\n    std::string text;\n    if (!flatXmlText(stripXmlComments(xml), kTag, &text)) {")], "the reader writes or deletes a file", R),
    M("pin-notice-includes-windows", "T6g", [("#include <cstdio>", "#include <cstdio>\n#include <windows.h>")], "the words header depends on Windows", N),
    M("pin-notice-names-config", "T6g", [("namespace edvr {\nnamespace tcn {", "namespace edvr {\nclass Config;\nnamespace tcn {")], "the words header names the config", N),
    M("pin-notice-includes-string", "T6g", [("#include <cstdint>", "#include <cstdint>\n#include <string>")], "the words header grows a dependency", N),
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


def read_source(rel):
    return (ROOT / rel).read_bytes().decode("utf-8").replace("\r\n", "\n")


def parse_fail(output):
    """The text after the last 'FAIL: ' line of the rig's output, or None when it printed none."""
    found = None
    for line in output.splitlines():
        if line.startswith("FAIL: "):
            found = line[len("FAIL: "):]
    return found


def rig_cases(rig_text):
    return set(re.findall(r'\{"(T\d+)", case\w+\}', rig_text))


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


def build_rig(tc, outdir, extra=(), sources=None):
    """Compile and link the rig into `outdir`; (exit code, output, exe). `sources` defaults to the rig and the worker with the real Config, Log, guard."""
    exe = outdir / "rig.exe"
    srcs = sources if sources is not None else [RIG, ROOT / WORKER] + [ROOT / p for p in REAL_SOURCES]
    cmd = [tc.cl] + CL_FLAGS + list(extra) + ["/Fo" + str(outdir) + os.sep, "/Fe" + str(exe)] + [str(s) for s in srcs] + ["/link", "/INCREMENTAL:NO"] + LINK_LIBS
    done = subprocess.run(cmd, capture_output=True, text=True, env=tc.env, errors="replace")
    return done.returncode, (done.stdout + done.stderr), exe


def run_rig(exe, rule, tc, root=None):
    """(outcome, detail) of one run of the rig: 'pass', 'fail' (detail = the label), 'crash', 'timeout'."""
    cmd = [str(exe), "--self-test"] + (["--only", rule] if rule else []) + (["--root", str(root)] if root else [])
    try:
        done = subprocess.run(cmd, capture_output=True, text=True, env=tc.env, errors="replace", timeout=RUN_TIMEOUT, cwd=str(ROOT))
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
    lines = ["%d mutation(s); each is compiled or staged in a temp directory outside the repo and run on its rule's case:" % len(mutants)]
    for m in mutants:
        lines.append("  %-30s %-6s rule %-3s caught by %-8s %-46s %s" % (m.name, m.kind, m.rule, "/".join(m.caught), m.target, m.why))
    lines.append("cl.exe flags: " + " ".join(CL_FLAGS) + "; libs: " + " ".join(LINK_LIBS))
    return "\n".join(lines)


def stage_root(work, name, target, text, files):
    """A temp repo root holding `files`, with `target` replaced by `text`."""
    root = work / ("root_" + name)
    for rel in files:
        dst = root / rel
        dst.parent.mkdir(parents=True, exist_ok=True)
        if rel == target:
            dst.write_text(text, encoding="utf-8", newline="\n")
        else:
            shutil.copyfile(ROOT / rel, dst)
    return root


def run_all(only=None, jobs=None, keep=False, dry_run=False, out=sys.stdout):
    mutants = select(only)
    if dry_run:
        print(plan_text(mutants), file=out)
        print("dry-run: nothing compiled, started or written", file=out)
        return 0
    tc = Toolchain()
    work = Path(tempfile.mkdtemp(prefix="terrain_checkerboard_mutants_"))
    try:
        # The controls: the rig built from the real sources, every case; and the header flavour (no worker) built from the real headers.
        cdir = work / "control"
        cdir.mkdir()
        code, text, control_exe = build_rig(tc, cdir)
        if code != 0:
            print(text, file=out)
            print("the rig does not compile", file=out)
            return 1
        outcome, detail = run_rig(control_exe, None, tc)
        print("control (the unmutated sources, every case): %s %s" % (outcome, detail), file=out)
        if outcome != "pass":
            print("the rig does not pass on the unmutated tree; nothing below means anything", file=out)
            return 1
        hdir = work / "control_headers"
        hdir.mkdir()
        code, text, header_exe = build_rig(tc, hdir, extra=["/DTCB_MUTANT"], sources=[RIG])
        if code != 0:
            print(text, file=out)
            print("the rig does not compile as a header mutant host (-DTCB_MUTANT)", file=out)
            return 1
        outcome, detail = run_rig(header_exe, "T1,T2,T3,T4,T6", tc)
        print("control (the header flavour, T1..T4 and T6): %s %s" % (outcome, detail), file=out)
        if outcome != "pass":
            print("the header flavour does not pass on the unmutated tree; nothing below means anything", file=out)
            return 1

        def edited(m):
            return apply_edits(read_source(m.target), m.edits, m.name)

        def code_mutant(m):
            try:
                text = edited(m)
            except ValueError as error:
                return m, "badedit", str(error)
            root = stage_root(work, m.name, m.target, text, HEADER_FILES)
            define = ['/DTCB_NOTICE_HEADER="%s"' % str(root / NOTICE).replace("\\", "/"), '/DTCB_READER_HEADER="%s"' % str(root / READER).replace("\\", "/"), "/DTCB_MUTANT"]
            d = work / ("build_" + m.name)
            d.mkdir()
            code, output, exe = build_rig(tc, d, extra=define, sources=[RIG])
            if code != 0:
                return m, "nocompile", (output.strip().splitlines()[-1] if output.strip() else "")
            outcome, detail = run_rig(exe, m.rule, tc)
            return m, outcome, detail

        def worker_mutant(m):
            try:
                text = edited(m)
            except ValueError as error:
                return m, "badedit", str(error)
            root = stage_root(work, m.name, m.target, text, WORKER_FILES)
            d = work / ("build_" + m.name)
            d.mkdir()
            code, output, exe = build_rig(tc, d, sources=[RIG, root / WORKER] + [ROOT / p for p in REAL_SOURCES])
            if code != 0:
                return m, "nocompile", (output.strip().splitlines()[-1] if output.strip() else "")
            outcome, detail = run_rig(exe, m.rule, tc)
            return m, outcome, detail

        def pin_mutant(m):
            try:
                text = edited(m)
            except ValueError as error:
                return m, "badedit", str(error)
            root = stage_root(work, m.name, m.target, text, PIN_FILES)
            outcome, detail = run_rig(control_exe, m.rule, tc, root=root)
            return m, outcome, detail

        def one(m):
            return {"code": code_mutant, "worker": worker_mutant, "pin": pin_mutant}[m.kind](m)

        results = []
        with concurrent.futures.ThreadPoolExecutor(max_workers=jobs or min(8, os.cpu_count() or 2)) as pool:
            for m, outcome, detail in pool.map(one, mutants):
                if outcome == "fail" and any(detail.startswith(p) for p in m.caught):
                    verdict = "caught"
                elif outcome == "fail":
                    verdict = "OTHER"
                else:
                    verdict = {"pass": "SURVIVED", "crash": "CRASH", "timeout": "TIMEOUT", "nocompile": "NOCOMPILE", "badedit": "BADEDIT"}[outcome]
                results.append((m, verdict, detail))
                print("%-9s %-30s %-3s %s" % (verdict, m.name, m.rule, detail[:150]), file=out)
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
    check(parse_fail("a\nFAIL: T1c: x\nb\n") == "T1c: x" and parse_fail("PASS: 3\n") is None, "parse_fail reads the last FAIL: line")
    check(apply_edits("a b c", [("b", "B")]) == "a B c", "apply_edits applies one edit")
    for bad, why in ((("a b b", [("b", "B")]), "a repeated anchor"), (("a b", [("x", "y")]), "a missing anchor"), (("a b", [("b", "b")]), "a no-op edit")):
        try:
            apply_edits(*bad)
            check(False, "apply_edits refuses " + why)
        except ValueError:
            pass
    joined = label_block(":rig_a\nx ^\n y\nexit /b 0\n:rig_b\nz\n", ":rig_a")
    check(joined is not None and [" ".join(l.split()) for l in joined] == ["x y", "exit /b 0"], "label_block joins continuations and stops at the next label")
    check(rig_cases('{"T1", caseT1}, {"T10", caseT10}') == {"T1", "T10"}, "rig_cases reads the case table")

    # every mutation against the sources as they are now, and against the rig
    rig = RIG.read_bytes().decode("utf-8").replace("\r\n", "\n")
    cases = rig_cases(rig)
    names = [m.name for m in MUTANTS]
    check(len(names) == len(set(names)), "mutation names are unique")
    check(len(MUTANTS) >= 95, "the mutation list did not shrink (%d)" % len(MUTANTS))
    sources = {}
    for m in MUTANTS:
        if m.target not in sources:
            try:
                sources[m.target] = read_source(m.target)
            except OSError as error:
                failures.append("%s: cannot read %s: %s" % (m.name, m.target, error))
                continue
        try:
            mutated = apply_edits(sources[m.target], m.edits, m.name)
            check(mutated != sources[m.target], "%s changes the source" % m.name)
        except ValueError as error:
            failures.append(str(error))
        for p in m.caught:
            check(rig_label_exists(rig, p), "%s: the rig has no check labelled %s" % (m.name, p))
        check(m.rule in cases, "%s: the rig has no case %s" % (m.name, m.rule))
        if m.kind == "pin":
            check(m.target in PIN_FILES, "%s: %s is not a file the pins' temp root copies" % (m.name, m.target))
        if m.kind == "worker":
            check(m.rule == "T5", "%s: a worker mutation is caught by T5 (the thread and the real log)" % m.name)
        if m.kind == "code":
            check(m.rule in ("T1", "T2", "T3", "T4"), "%s: a header mutation is caught by T1..T4, which the header flavour runs" % m.name)
    rules = {m.rule for m in MUTANTS}
    check(rules == cases, "every rule of the rig has a mutation, and only rules of the rig: %s vs %s" % (sorted(rules), sorted(cases)))
    labels = set(re.findall(r'"(T\d+[a-z]):', rig))
    caught_labels = {p for m in MUTANTS for p in m.caught}
    check(len(caught_labels) >= 18, "the mutations name %d check labels; they should cover the rig's rules widely" % len(caught_labels))
    check(not (caught_labels - labels), "labels named by a mutation and not in the rig: %s" % sorted(caught_labels - labels))

    # build.bat compiles the rig the way this tool does, links the worker into the DLL, and runs this tool's self-test
    bat = BUILD_BAT.read_bytes().decode("utf-8", errors="replace")
    block = label_block(bat, RIG_LABEL)
    check(block is not None, "build.bat has %s" % RIG_LABEL)
    if block:
        text = "\n".join(block)
        cl = next((l for l in block if l.strip().lower().startswith("cl.exe")), "")
        for flag in CL_FLAGS:
            check(flag in cl, "build.bat's rig compile has %s" % flag)
        for part in ("tools\\terrain_checkerboard_test\\terrain_checkerboard_test.cpp", "src\\d3d11\\terrain_checkerboard.cpp", "src\\common\\config.cpp", "src\\common\\log.cpp",
                     "src\\common\\guard.cpp", "src\\common\\proxy.cpp"):
            check(part in cl, "build.bat's rig compile has %s" % part)
        for lib in LINK_LIBS:
            check(lib in cl, "build.bat's rig link has %s" % lib)
        check("terrain_checkerboard_test.exe\" --dry-run" in text and "terrain_checkerboard_test.exe\" --self-test \"%ROOT%\"" in text, "build.bat runs the rig's --dry-run and --self-test with the repo root")
        check("mutants.py\" --self-test" in text, "build.bat runs this tool's --self-test")
    dll = next((l for l in bat.replace("\r\n", "\n").split("\n") if "terrain_checkerboard.cpp" in l and "journal_watch.cpp" in l and "tools" not in l), "")
    check("src\\d3d11\\terrain_checkerboard.cpp" in dll, "build.bat compiles src\\d3d11\\terrain_checkerboard.cpp into the d3d11 DLL")

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
    print("PASS: terrain_checkerboard_test mutants.py self-test (%d mutations over %d rules, every anchor found once, build.bat wired)" % (len(MUTANTS), len(rules)))
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
        print(plan_text(select(args.only)))
        return 0
    if args.run:
        return run_all(args.only, args.jobs, args.keep, args.dry_run)
    parser.print_help()
    return 2


if __name__ == "__main__":
    sys.exit(main())
