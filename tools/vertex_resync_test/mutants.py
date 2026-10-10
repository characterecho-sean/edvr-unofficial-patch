#!/usr/bin/env python3
"""The mutation proof for tools\\vertex_resync_test: the rig fails when a rule of the vertex-buffer resync is broken.

The rig (vertex_resync_test.cpp) holds the pure repair (src\\d3d11\\vertex_resync_core.h) to a fake f3d command list laid out at the real offsets and the failing
sequence replayed on it (V1), the gate and the 16 prologue bytes of FlushIA (V2), the instruments' exact lines, windows and flush counter (V3), the production hook
(src\\d3d11\\vertex_resync_hook.cpp, compiled in with EDVR_VERTEX_RESYNC_TEST and the real CodeHook) on a synthetic FlushIA (V4), what the log says while it is armed -- the
60 s count, the ten-minute heartbeat, the session line, and the process-exit line written by the real Log (src\\common\\log.cpp) (V5), faults and the stand-down (V6); the
glue in device_hook.cpp, the hook's own text and the flush counter's text are read as text (V7). Every failure carries a label "V<case>.<what>". A rig that passes proves
little until it is seen to FAIL on a source that breaks the rule it pins: for each mutation below the machinery (tools\\rig_mutants_lib.py) copies the sources into a temp
directory OUTSIDE the repo, applies the edit, rebuilds the rig against the copy (or, for the glue it reads as text, runs it against an edited copy) and requires a FAIL on a
check of the case that belongs to the rule.

  python tools\\vertex_resync_test\\mutants.py --self-test    text only: every anchor is found exactly once, every case named is in the rig, and build.bat
                                                              compiles the rig the way the machinery does and runs this self-test
  python tools\\vertex_resync_test\\mutants.py --run [--only a,b] [--jobs N] [--keep] [--verbose]
  python tools\\vertex_resync_test\\mutants.py --list
  python tools\\vertex_resync_test\\mutants.py --run --dry-run   the plan; writes nothing, starts nothing
"""
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
import rig_mutants_lib as lib  # noqa: E402

ROOT = HERE.parents[1]
FILES = {
    "core": ROOT / "src" / "d3d11" / "vertex_resync_core.h",
    "hook": ROOT / "src" / "d3d11" / "vertex_resync_hook.cpp",          # compiled into the rig
    "log": ROOT / "src" / "common" / "log.cpp",                          # ...and this: the process-exit line is written here
    "device": ROOT / "src" / "d3d11" / "device_hook.cpp",                # read by the rig as text (V7)
    "hooktext": ROOT / "src" / "d3d11" / "vertex_resync_hook.cpp",       # ...and the hook's own text (it registers its exit line, it reads no key)
    "coretext": ROOT / "src" / "d3d11" / "vertex_resync_core.h",         # ...and the flush counter's text (a load and a store)
}
COMMON = ["code_hook.h", "code_hook.cpp", "guard.h", "guard.cpp", "log.h", "config.h", "config.cpp", "ini_name.h", "proxy.h", "game_call_probe.h",
          "call_probe_budget.h", "runtime_profile.h"]
TREE_EXTRA = [("src/common/" + name, ROOT / "src" / "common" / name) for name in COMMON] + [
    ("src/d3d11/vertex_resync_hook.h", ROOT / "src" / "d3d11" / "vertex_resync_hook.h"),
]
UNITS = [("src/d3d11/vertex_resync_hook.cpp", "hook"), ("src/common/log.cpp", "log")] + [("src/common/" + n, None) for n in ("code_hook.cpp", "guard.cpp", "config.cpp")]
CL_FLAGS = lib.DEFAULT_CL_FLAGS + ["/DUNICODE", "/D_UNICODE", "/DEDVR_VERTEX_RESYNC_TEST"]
CONFIG = lib.Config(
    here=__file__, files=FILES, header_keys=("core", "hook", "log"), pin_keys=("device", "hooktext", "coretext"), rig=HERE / "vertex_resync_test.cpp",
    rig_label=":rig_vertex_resync_test", rig_source_in_bat="tools\\vertex_resync_test\\vertex_resync_test.cpp", rig_exe_in_bat="vertex_resync_test.exe",
    case_prefix="V", include_dirs=("src/d3d11", "src/common"), cl_flags=CL_FLAGS, units=UNITS, link_args=["kernel32.lib", "user32.lib"], min_mutants=40,
    run_timeout=120.0, tree_extra=TREE_EXTRA)

PROLOGUE = "0x48, 0x89, 0x5C, 0x24, 0x18, 0x56, 0x57, 0x41, 0x56, 0x48, 0x83, 0xEC, 0x30, 0x49, 0x8B, 0xF8}"
SLOTS = "const uint32_t slots = r.layoutCount < kMaxSlots ? r.layoutCount : kMaxSlots;"
OFFSET_COPY = "        store32(applied + kAppliedOffsets + 4u * i, load32(desired + kDesiredOffsets + 4u * i));\n"
BUMP = "n.store(n.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);"
REGISTER_EXIT = "    Log::get().setExitLine(&exitLine);   // the game never unloads this DLL: its process exit says the session line\n"
HEARTBEAT_GATE = "if (g_installed.load(std::memory_order_acquire) && g_relayGate.load(std::memory_order_acquire) != 0) {\n            char line[200];"
CRLF = "                    msg[n++] = '\\r';\n                    msg[n++] = '\\n';\n"

M = lib.M
MUTANTS = [
    # ---- V1: the repair on a fake list at the real offsets ----
    M("repair-write-dropped", "V1", "core", [("        store64(appliedBuffer, native);\n", "")],
      "the applied buffer is never rewritten: the second eye's resolve is bound NULL"),
    M("offset-copy-dropped", "V1", "core", [(OFFSET_COPY, "")], "the applied offset is not copied from the desired state: a buffer bound at an offset is bound at 0"),
    M("offset-of-slot-zero-for-all", "V1", "core", [("load32(desired + kDesiredOffsets + 4u * i)", "load32(desired + kDesiredOffsets)")],
      "every repaired slot takes slot 0's offset"),
    M("count-bound-dropped", "V1", "core", [(SLOTS, "const uint32_t slots = r.layoutCount;")],
      "a layout that declares more than 16 slots walks past the arrays"),
    M("slots-above-the-layout-checked", "V1", "core", [(SLOTS, "const uint32_t slots = kMaxSlots;")],
      "every one of the 16 slots is repaired, whatever the draw's layout declares"),
    M("null-wrapper-followed", "V1", "core", [("        if (!wrapper) continue;\n", "")], "an empty desired slot is dereferenced"),
    M("null-pointers-unchecked", "V1", "core", [("    if (!pso || !desired || !applied) return r;\n", "")], "a null pso, desired state or applied cache is read"),
    M("null-layout-unchecked", "V1", "core", [("    if (!layoutAddress) return r;\n", "")], "a pso without a layout object is read through"),
    M("healthy-slots-rewritten", "V1", "core", [("        if (was == native) continue;\n", "")], "a slot whose applied buffer is right is counted as repaired and rewritten"),
    M("first-sighting-overwritten", "V1", "core", [("        if (!r.hasFirst) {\n", "        if (true) {\n")], "the report names the last repaired slot, not the first"),
    # ---- V2: the gate and the prologue ----
    M("prologue-byte-wrong", "V2", "core", [(PROLOGUE, PROLOGUE.replace("0xF8}", "0xF9}"))], "the hook's idea of FlushIA's prologue differs from the exe's in the last byte"),
    M("prologue-check-shortened", "V2", "core", [("std::memcmp(bytes, kFlushIaPrologue, kPrologueBytes) == 0", "std::memcmp(bytes, kFlushIaPrologue, kPrologueBytes - 1) == 0")],
      "the last byte of the prologue is not compared"),
    M("timestamp-ignored", "V2", "core", [("return timestamp == kExpectedTimestamp && imageSize == kExpectedImageSize;", "return imageSize == kExpectedImageSize;")],
      "a build with the right size and another timestamp passes the gate"),
    M("image-size-ignored", "V2", "core", [("return timestamp == kExpectedTimestamp && imageSize == kExpectedImageSize;", "return timestamp == kExpectedTimestamp;")],
      "a build with the right timestamp and another size passes the gate"),
    M("rva-moved", "V2", "core", [("constexpr uintptr_t kFlushIaRva = 0x522A50u;", "constexpr uintptr_t kFlushIaRva = 0x522A51u;")], "the hook aims one byte into the function"),
    # ---- V3: the instruments ----
    M("window-shortened", "V3", "core", [("constexpr uint64_t kWindowMs = 60000;", "constexpr uint64_t kWindowMs = 6000;")], "the running count is said every 6 s"),
    M("heartbeat-shortened", "V3", "core", [("constexpr uint64_t kHeartbeatMs = 600000;", "constexpr uint64_t kHeartbeatMs = 60000;")], "the heartbeat is said every minute"),
    M("window-count-not-emptied", "V3", "core", [("*count = count_.exchange(0, std::memory_order_relaxed);", "*count = count_.load(std::memory_order_relaxed);")],
      "the window's count is said again in the next window"),
    M("first-tick-says", "V3", "core", [("            startMs_ = nowMs;\n            return false;\n", "            startMs_ = nowMs;\n            *count = 0;\n            return true;\n")],
      "the tick that starts the window ends it"),
    M("zero-window-silent", "V3", "core", [("        *count = count_.exchange(0, std::memory_order_relaxed);\n        return true;", "        *count = count_.exchange(0, std::memory_order_relaxed);\n        return *count != 0;")],
      "a window with nothing in it ends silently: the heartbeat could not say zero"),
    M("window-not-restarted", "V3", "core", [("        startMs_ = nowMs;\n        *count = count_.exchange", "        *count = count_.exchange")],
      "a window that ended never restarts: every tick after it ends one"),
    M("nine-sightings", "V3", "core", [("constexpr uint32_t kMaxSightings = 8;", "constexpr uint32_t kMaxSightings = 9;")], "nine first-sighting lines"),
    M("resync-line-reworded", "V3", "core", [("%u stale vertex-buffer bindings in the last 60 s", "%u stale bindings in the last 60 s")], "the 60 s line is not the one asked for"),
    M("heartbeat-reworded", "V3", "core", [("vertex resync: armed; %llu stale vertex-buffer bindings repaired in the last 10 min", "vertex resync: %llu stale vertex-buffer bindings repaired in the last 10 min")],
      "the heartbeat does not say it is armed"),
    M("heartbeat-drops-the-flush-count", "V3", "core", [("repaired in the last 10 min (%llu flushes seen)", "repaired in the last 10 min")],
      "the heartbeat does not say how many flushes it saw: quiet could not be told from absent"),
    M("session-reworded", "V3", "core", [("repaired this session (%llu flushes seen)", "repaired this run (%llu flushes seen)")], "the session line is not the one asked for"),
    M("sighting-drops-the-thread", "V3", "core", [("layout slots %u, thread %u, ", "layout slots %u, %u, ")], "a first-sighting line does not name its thread"),
    M("flush-count-unpadded", "V3", "core",
      [("struct alignas(64) FlushCount {", "struct FlushCount {"), ('static_assert(sizeof(FlushCount) == 64, "the flush counter owns its cache line");\n', "")],
      "the flush counter shares its cache line with whatever is next to it"),
    M("flush-count-bump-wrong", "V3", "core", [(BUMP, "n.store(n.load(std::memory_order_relaxed) + 2, std::memory_order_relaxed);")], "a flush counts twice"),
    M("flush-count-reset-missing", "V3", "core", [("{ n.store(0, std::memory_order_relaxed); }", "{ }")], "reset leaves the count"),
    # ---- V4: the production hook on a synthetic FlushIA ----
    M("original-not-called", "V4", "hook", [("    return forward(pso, context, desired, applied);\n}", "    return 0;\n}")], "the game's flush never runs"),
    M("repair-never-run", "V4", "hook", [("    if (pso && desired && applied) {\n        vresync::Report r;", "    if (false) {\n        vresync::Report r;")],
      "the callback forwards without repairing"),
    M("prologue-unchecked", "V4", "hook", [("if (!sehCheckBytes(target, vresync::kFlushIaPrologue, vresync::kPrologueBytes)) {", "if (false) {")],
      "a function whose prologue is not build 332841's is patched"),
    M("refusal-retried", "V4", "hook", [("    if (!g_attempted.compare_exchange_strong(expected, true)) return;   // once, ever: a refusal is final\n", "")],
      "a second call tries again"),
    M("sighting-cap-dropped", "V4", "hook", [("if (!r.hasFirst || !g_sightings.take()) return;", "if (!r.hasFirst) return;")], "every repair writes a sighting line"),
    M("relay-gate-never-opened", "V4", "hook", [("    g_relayGate.store(1, std::memory_order_release);\n    g_installed", "    g_installed")],
      "the hook is patched in and the relay forwards straight to the original"),
    M("flush-not-counted", "V4", "hook", [("    g_flushes.bump();\n", "")], "the flushes seen never move"),
    M("arm-line-forgets-the-heartbeat", "V4", "hook", [("a heartbeat every 10 min (zero counts included)", "a count every 10 min (zero counts included)")],
      "the arming line does not promise the heartbeat"),
    M("repaired-total-not-fed", "V4", "hook", [("            g_repairedTotal.fetch_add(r.repaired, std::memory_order_relaxed);\n", "")], "the session total never sees a repair"),
    # ---- V5: what the log says while armed ----
    M("window-not-fed", "V5", "hook", [("            g_window.add(r.repaired);\n", "")], "the 60 s count never sees a repair"),
    M("window-never-said", "V5", "hook", [("        vresync::formatResyncLine(line, sizeof(line), n);\n        say(line);", "        vresync::formatResyncLine(line, sizeof(line), n);")],
      "the 60 s line is never written"),
    M("zero-window-said", "V5", "hook", [("if (g_window.tick(nowMs, &n) && n) {", "if (g_window.tick(nowMs, &n)) {")], "the 60 s line is written for a window with nothing in it"),
    M("heartbeat-never-fed", "V5", "hook", [("            g_beat.add(r.repaired);\n", "")], "the heartbeat never sees a repair"),
    M("heartbeat-never-said", "V5", "hook", [("            vresync::formatHeartbeatLine(line, sizeof(line), repairedInBeat, seen);\n            say(line);",
                                             "            vresync::formatHeartbeatLine(line, sizeof(line), repairedInBeat, seen);")], "the heartbeat is built and never written"),
    M("heartbeat-flushes-cumulative", "V5", "hook", [("const uint64_t seen = flushes - g_flushesAtLastBeat;", "const uint64_t seen = flushes;")],
      "the heartbeat's flushes seen are the session's, not the window's"),
    M("session-line-dropped", "V5", "hook", [("if (exitLine(line, sizeof(line)) > 0) say(line);", "if (exitLine(line, sizeof(line)) > 0) line[0] = 0;")], "shutdown builds the session line and never writes it"),
    M("exit-line-registration-dropped", "V5", "hook", [(REGISTER_EXIT, "")], "the game's process exit says nothing: the log never gets its session line"),
    M("exit-line-not-written", "V5", "log", [("const int m = exitLine(msg + n, sizeof(msg) - static_cast<size_t>(n) - 3);", "const int m = 0;")],
      "the log asks for no exit line at process exit"),
    M("exit-line-unterminated", "V5", "log", [(CRLF, "")], "the exit line is written without its line break"),
    # ---- V6: faults and the stand-down ----
    M("fault-stand-down-dropped", "V6", "hook", [("    if (n == 8) {\n        g_relayGate", "    if (false) {\n        g_relayGate")], "a function that keeps faulting is never stood down"),
    M("fault-unreported", "V6", "hook", [("    if (n == 1) say(", "    if (false) say(")], "a fault is absorbed in silence"),
    M("heartbeat-without-arming", ("V4", "V6"), "hook", [(HEARTBEAT_GATE, "if (true) {\n            char line[200];")],
      "the heartbeat is said by a hook that never armed or was stood down"),
    M("exit-line-ignores-the-gate", ("V4", "V6"), "hook", [("if (!g_installed.load(std::memory_order_acquire) || g_relayGate.load(std::memory_order_acquire) == 0) return 0;", "if (false) return 0;")],
      "a hook that never armed or was stood down still says a session line"),
    # ---- V7: the glue, read as text ----
    M("install-removed", "V7", "device", [("            vertexResyncInstall();\n", "")], "nothing installs the hook"),
    M("poll-removed", "V7", "device", [("            vertexResyncPoll(stampMs());\n", "")], "the 60 s count and the heartbeat are never said"),
    M("shutdown-removed", "V7", "device", [("vertexResyncShutdown();", "")], "a FreeLibrary teardown says no session line"),
    M("exit-registration-removed-in-text", "V7", "hooktext", [(REGISTER_EXIT, "")], "the hook's text no longer registers its process-exit line"),
    M("key-reintroduced", "V7", "hooktext", [("    g_flushes.bump();\n", "    g_flushes.bump();\n    const bool on = Config::get().getBool(\"x\", true); (void)on;\n")],
      "the hook reads a key again: the repair is no longer always on"),
    M("flush-counted-after-the-repair", "V7", "hooktext", [("    g_flushes.bump();\n", ""), ("    return forward(pso, context, desired, applied);\n}", "    g_flushes.bump();\n    return forward(pso, context, desired, applied);\n}")],
      "the flush is counted after the repair, so a faulting flush is not counted"),
    M("flush-count-locked", "V7", "coretext", [(BUMP, "n.fetch_add(1, std::memory_order_relaxed);")], "the render thread's flush counter takes a lock-prefixed read-modify-write"),
]

if __name__ == "__main__":
    sys.exit(lib.main(CONFIG, MUTANTS))
