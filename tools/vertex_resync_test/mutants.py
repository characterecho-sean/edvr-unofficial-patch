#!/usr/bin/env python3
"""The mutation proof for tools\\vertex_resync_test: the rig fails when a rule of the vertex-buffer resync is broken.

The rig (vertex_resync_test.cpp) holds the pure repair (src\\d3d11\\vertex_resync_core.h) to a fake f3d command list laid out at the real offsets and the failing
sequence replayed on it (V1), the gate and the 16 prologue bytes of FlushIA (V2), the key and the flat profile's allow-list (V3), the instruments' exact lines and
windows (V4), and the production hook (src\\d3d11\\vertex_resync_hook.cpp, compiled in with EDVR_VERTEX_RESYNC_TEST and the real CodeHook) on a synthetic FlushIA (V5);
the glue in device_hook.cpp and resolve_bind_fix.cpp is read as text (V6). Every failure carries a label "V<case>.<what>". A rig that passes proves little until it is
seen to FAIL on a source that breaks the rule it pins: for each mutation below the machinery (tools\\rig_mutants_lib.py) copies the sources into a temp directory OUTSIDE
the repo, applies the edit, rebuilds the rig against the copy (or, for the glue it reads as text, runs it against an edited copy) and requires a FAIL on a check of the
case that belongs to the rule.

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
    "profile": ROOT / "src" / "common" / "runtime_profile.h",            # the flat allow-list
    "device": ROOT / "src" / "d3d11" / "device_hook.cpp",                # read by the rig as text (V6)
    "resolve": ROOT / "src" / "d3d11" / "resolve_bind_fix.cpp",          # ...and this
    "hooktext": ROOT / "src" / "d3d11" / "vertex_resync_hook.cpp",       # ...and the hook's own text (the key it reads)
}
COMMON = ["code_hook.h", "code_hook.cpp", "guard.h", "guard.cpp", "log.h", "log.cpp", "config.h", "config.cpp", "ini_name.h", "proxy.h", "game_call_probe.h",
          "call_probe_budget.h"]
TREE_EXTRA = [("src/common/" + name, ROOT / "src" / "common" / name) for name in COMMON] + [
    ("src/d3d11/vertex_resync_hook.h", ROOT / "src" / "d3d11" / "vertex_resync_hook.h"),
]
UNITS = [("src/d3d11/vertex_resync_hook.cpp", "hook")] + [("src/common/" + n, None) for n in ("code_hook.cpp", "guard.cpp", "log.cpp", "config.cpp")]
CL_FLAGS = lib.DEFAULT_CL_FLAGS + ["/DUNICODE", "/D_UNICODE", "/DEDVR_VERTEX_RESYNC_TEST"]
CONFIG = lib.Config(
    here=__file__, files=FILES, header_keys=("core", "hook", "profile"), pin_keys=("device", "resolve", "hooktext"), rig=HERE / "vertex_resync_test.cpp",
    rig_label=":rig_vertex_resync_test", rig_source_in_bat="tools\\vertex_resync_test\\vertex_resync_test.cpp", rig_exe_in_bat="vertex_resync_test.exe",
    case_prefix="V", include_dirs=("src/d3d11", "src/common"), cl_flags=CL_FLAGS, units=UNITS, link_args=["kernel32.lib", "user32.lib"], min_mutants=30,
    run_timeout=120.0, tree_extra=TREE_EXTRA)

PROLOGUE = "0x48, 0x89, 0x5C, 0x24, 0x18, 0x56, 0x57, 0x41, 0x56, 0x48, 0x83, 0xEC, 0x30, 0x49, 0x8B, 0xF8}"
SLOTS = "const uint32_t slots = r.layoutCount < kMaxSlots ? r.layoutCount : kMaxSlots;"
OFFSET_COPY = "            store32(applied + kAppliedOffsets + 4u * i, load32(desired + kDesiredOffsets + 4u * i));\n"

M = lib.M
MUTANTS = [
    # ---- V1: the repair on a fake list at the real offsets ----
    M("repair-write-dropped", "V1", "core", [("            store64(appliedBuffer, native);\n", "")],
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
    M("off-still-writes", "V1", "core", [("        if (repair) {\n            store64", "        if (true) {\n            store64")],
      "with the key off the cache is rewritten all the same: the A/B would compare nothing"),
    M("healthy-slots-counted", "V1", "core", [("        if (was == native) continue;\n", "")], "a slot whose applied buffer is right is counted as a desync and rewritten"),
    # ---- V2: the gate and the prologue ----
    M("prologue-byte-wrong", "V2", "core", [(PROLOGUE, PROLOGUE.replace("0xF8}", "0xF9}"))], "the hook's idea of FlushIA's prologue differs from the exe's in the last byte"),
    M("prologue-check-shortened", "V2", "core", [("std::memcmp(bytes, kFlushIaPrologue, kPrologueBytes) == 0", "std::memcmp(bytes, kFlushIaPrologue, kPrologueBytes - 1) == 0")],
      "the last byte of the prologue is not compared"),
    M("timestamp-ignored", "V2", "core", [("return timestamp == kExpectedTimestamp && imageSize == kExpectedImageSize;", "return imageSize == kExpectedImageSize;")],
      "a build with the right size and another timestamp passes the gate"),
    M("rva-moved", "V2", "core", [("constexpr uintptr_t kFlushIaRva = 0x522A50u;", "constexpr uintptr_t kFlushIaRva = 0x522A51u;")], "the hook aims one byte into the function"),
    # ---- V3: the key and the flat allow-list ----
    M("false-is-not-off", "V3", "core", [(' || std::strcmp(lower, "false") == 0', "")], "false is read as on"),
    M("key-case-sensitive", "V3", "core", [("text[n] >= 'A' && text[n] <= 'Z' ? text[n] + 32 : text[n]", "text[n]")], "OFF is read as on"),
    M("missing-key-is-off", "V3", "core", [("    if (!text) return true;\n", "    if (!text) return false;\n")], "no value reads as off: the default is not on"),
    M("flat-allow-list-entry-dropped", "V3", "profile", [('        std::strcmp(key, "advanced.vertex_resync") == 0);', "        false);")],
      "the flat profile answers off for advanced.vertex_resync whatever the file says"),
    M("flat-allow-list-by-prefix", "V3", "profile", [('std::strcmp(key, "advanced.vertex_resync") == 0)', 'std::strncmp(key, "advanced.vertex_resync", 22) == 0)')],
      "the flat allow-list lets a longer key through"),
    # ---- V4: the instruments ----
    M("window-shortened", "V4", "core", [("constexpr uint64_t kWindowMs = 60000;", "constexpr uint64_t kWindowMs = 6000;")], "the running counts are said every 6 s"),
    M("window-count-not-emptied", "V4", "core", [("return count_.exchange(0, std::memory_order_relaxed);", "return count_.load(std::memory_order_relaxed);")],
      "the window's count is said again in the next window"),
    M("first-tick-says", "V4", "core", [("            startMs_ = nowMs;\n            return 0;", "            startMs_ = nowMs;\n            return count_.exchange(0, std::memory_order_relaxed);")],
      "the tick that starts the window ends it"),
    M("nine-sightings", "V4", "core", [("constexpr uint32_t kMaxSightings = 8;", "constexpr uint32_t kMaxSightings = 9;")], "nine first-sighting lines"),
    M("resync-line-reworded", "V4", "core", [("stale vertex-buffer bindings in the last 60 s", "stale bindings in the last 60 s")], "the 60 s line is not the one asked for"),
    M("left-reworded", "V4", "core", [("left as the game had them", "left alone")], "the repair-off wording differs"),
    M("lent-line-reworded", "V4", "core", [("lent the other eye's buffer %u times", "lent the buffer %u times")], "the scanner-body running line is not the one asked for"),
    M("sighting-drops-the-thread", "V4", "core", [("layout slots %u, thread %u, %s;", "layout slots %u, %u, %s;")], "a first-sighting line does not name its thread"),
    # ---- V5: the production hook on a synthetic FlushIA ----
    M("repair-flag-ignored", "V5", "hook", [("const bool repair = g_repair.load(std::memory_order_relaxed);", "const bool repair = true;")], "the live key is ignored: off still repairs"),
    M("original-not-called", "V5", "hook", [("    return forward(pso, context, desired, applied);\n}", "    return 0;\n}")], "the game's flush never runs"),
    M("prologue-unchecked", "V5", "hook", [("if (!sehCheckBytes(target, vresync::kFlushIaPrologue, vresync::kPrologueBytes)) {", "if (false) {")],
      "a function whose prologue is not build 332841's is patched"),
    M("refusal-retried", "V5", "hook", [("    if (!g_attempted.compare_exchange_strong(expected, true)) return;   // once, ever: a refusal is final\n", "")],
      "a second call tries again"),
    M("sighting-cap-dropped", "V5", "hook", [("if (!r.hasFirst || !g_sightings.take()) return;", "if (!r.hasFirst) return;")], "every desync writes a sighting line"),
    M("fault-stand-down-dropped", "V5", "hook", [("    if (n == 8) {\n        g_relayGate", "    if (false) {\n        g_relayGate")], "a function that keeps faulting is never stood down"),
    M("fault-unreported", "V5", "hook", [("    if (n == 1) say(", "    if (false) say(")], "a fault is absorbed in silence"),
    M("window-not-fed", "V5", "hook", [("            g_window.add(r.desynced);\n", "")], "the 60 s count never sees a desync"),
    M("window-never-said", "V5", "hook", [("if (const uint32_t n = g_window.take(nowMs)) {", "if (const uint32_t n = static_cast<uint32_t>(g_window.take(nowMs) * 0u)) {")],
      "the 60 s line is never written"),
    M("key-not-live", "V5", "hook", [("const bool changed = g_repair.exchange(want, std::memory_order_relaxed) != want;", "const bool changed = false;")],
      "the poll never moves the repair flag"),
    M("relay-gate-never-opened", "V5", "hook", [("    g_relayGate.store(1, std::memory_order_release);\n    g_installed", "    g_installed")],
      "the hook is patched in and the relay forwards straight to the original"),
    # ---- V6: the glue, read as text ----
    M("install-removed", "V6", "device", [("            vertexResyncInstall();\n", "")], "nothing installs the hook"),
    M("poll-removed", "V6", "device", [("            vertexResyncPoll(Config::get(), stampMs());\n", "")], "the key is never read live and the 60 s count is never said"),
    M("lent-tick-removed", "V6", "device", [("            resolveBindTick(stampMs());\n", "")], "the scanner-body running count is never said"),
    M("lend-not-counted", "V6", "resolve", [("        g_lentWindow.add();\n", "")], "a lend does not count in the running window"),
    M("engaged-line-gone", "V6", "resolve", [("scanner body fix: ENGAGED", "scanner body fix: engaged")], "the first ENGAGED line is changed"),
    M("key-default-off", "V6", "hooktext", [('cfg.getString("advanced.vertex_resync", "on")', 'cfg.getString("advanced.vertex_resync", "off")')], "the key defaults to off"),
]

if __name__ == "__main__":
    sys.exit(lib.main(CONFIG, MUTANTS))
