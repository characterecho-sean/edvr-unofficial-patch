#!/usr/bin/env python3
"""The mutation proof for tools\\skin_clone_test: the rig fails when a rule of the second skin's vertex shader patch is broken.

The rig (skin_clone_test.cpp, with tools\\engine_velocity_test\\skin_clone_tests.h) holds the patch's rules (src\\d3d11\\dxbc_skin_clone.h and the pixel
half in dxbc_engine_velocity.h): the token walker (K1), what the patch makes (K2), what it declines (K3), and on WARP, through a vertex shader written
to the game's shape, the properties that make E exact -- identity gives exactly zero, a moved pose or palette gives 100 x the move, a swap negates,
no history is zero and invalid, a different previous state is nonzero, a jump past the limit is no motion (K4), the same with the displacement block
the game's shaders have after the pose chain (K5). Every check carries a label "K<case>.<what>". A rig that passes proves little until it is seen to
FAIL on a source that breaks the rule it pins; for each mutation below the machinery (tools\\rig_mutants_lib.py) copies the sources into a temp
directory OUTSIDE the repo, applies the edit, rebuilds the rig from that copy and requires a FAIL on a check of the case that belongs to the rule.

  python tools\\skin_clone_test\\mutants.py --self-test       text only: every anchor is found exactly once, every case named is in the rig, and
                                                              build.bat compiles the rig the way the machinery does and runs this self-test
  python tools\\skin_clone_test\\mutants.py --run [--only a,b] [--jobs N] [--keep] [--verbose]
  python tools\\skin_clone_test\\mutants.py --list
  python tools\\skin_clone_test\\mutants.py --run --dry-run   the plan; writes nothing, starts nothing

K9 (the game's own five skinned shaders, --corpus) cannot run in a build: it is exempt from the coverage rule and is checked by hand on a flight log
dump (docs\\kinematic-motion-injection-2026-09-19.md, "F2 built").
"""
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
import rig_mutants_lib as lib  # noqa: E402

ROOT = HERE.parents[1]
FILES = {
    "clone": ROOT / "src" / "d3d11" / "dxbc_skin_clone.h",
    "ev": ROOT / "src" / "d3d11" / "dxbc_engine_velocity.h",
}
# the rig includes its sources by relative path, so the rig itself and everything it includes sit in the temp tree beside the mutated headers
TREE_EXTRA = [
    ("tools/skin_clone_test/skin_clone_test.cpp", HERE / "skin_clone_test.cpp"),
    ("tools/skin_clone_test/synthetic_skin.h", HERE / "synthetic_skin.h"),
    ("tools/engine_velocity_test/skin_clone_tests.h", ROOT / "tools" / "engine_velocity_test" / "skin_clone_tests.h"),
    ("tools/engine_velocity_test/corpus_identity.h", ROOT / "tools" / "engine_velocity_test" / "corpus_identity.h"),
    ("third_party/dxbc_hash/DxilHash.cpp", ROOT / "third_party" / "dxbc_hash" / "DxilHash.cpp"),
    ("src/d3d11/dxbc_container.h", ROOT / "src" / "d3d11" / "dxbc_container.h"),
]
CL_FLAGS = lib.DEFAULT_CL_FLAGS + ["/DUNICODE", "/D_UNICODE", "/utf-8"]
CONFIG = lib.Config(
    here=__file__, files=FILES, header_keys=("clone", "ev"), pin_keys=(), rig=HERE / "skin_clone_test.cpp",
    rig_label=":rig_skin_clone_test", rig_source_in_bat="tools\\skin_clone_test\\skin_clone_test.cpp", rig_exe_in_bat="skin_clone_test.exe",
    case_prefix="K", include_dirs=(), cl_flags=CL_FLAGS, link_args=["d3d11.lib", "d3dcompiler.lib", "dxguid.lib"], min_mutants=22,
    run_timeout=240.0, tree_extra=TREE_EXTRA, rig_in_tree=True, ignore_cases=("K9",))

M = lib.M
# ---- the mutations: edits of the production sources, anchors verbatim (the self-test finds each exactly once) --------------------
MUTANTS = [
    # the clone's resources
    M("pose-load-not-redirected", "K4", "clone", [("else if (copy[idx] == 33 && poseLoad) copy[idx] = kSkinPoseSlot;", "else if (copy[idx] == 33 && poseLoad) copy[idx] = 33;")],
      "the previous pose is read from the current pose table: a moved previous pose is not seen"),
    M("palette-not-redirected", "K4", "clone", [("if (copy[idx] == 38) copy[idx] = kSkinPrevPaletteSlot;", "if (copy[idx] == 38) copy[idx] = 38;")],
      "the clone skins with the current palette: a moved previous palette is not seen"),
    M("pose-address-current-base", "K4", "clone", [("copy[a] = kTempSelect[1];", "copy[a] = kTempSelect[0];")],
      "the previous pose is looked up by the current base, not the joined previous base"),
    M("pose-stride-not-patched", "K2", "clone",
      [("copy[pos] = (copy[pos] & ~(0xFFFu << 11)) | (kSkinPoseStrideBytes << 11);", "copy[pos] = copy[pos];")],
      "the load's stride token still says 336 bytes against a 32-byte table"),
    M("temps-not-renamed", "K4", "clone", [("if (type == kTypeTemp) copy[idx] += N;", "if (type == kTypeTemp) copy[idx] += 0;")],
      "the clone overwrites the game's own temporaries"),
    M("output-writes-cloned", "K2", "clone",
      [("if (shape->dst && typeOf(t[in.operand[0]]) == kTypeOutput) continue;   // output writes are not cloned", "if (false) continue;   // output writes are not cloned")],
      "the clone writes the game's outputs from the previous state"),
    M("temp-count-not-doubled", "K2", "clone", [("out.push_back(2 * N + 2);", "out.push_back(N + 2);")],
      "the temporaries the clone needs are not declared"),
    # E itself
    M("difference-sign-flipped", "K4", "clone",
      [("E, swz, cloneReg, swz | 0x80000000u, 0x00000041u, plan.posReg});", "E, swz, plan.posReg, swz | 0x80000000u, 0x00000041u, cloneReg});")],
      "E is current - previous"),
    M("centimetres-wrong", "K4", "clone",
      [("put({0x0A000038u, 0x00100072u, E, kXyzw, E, 0x00004002u, 0x42C80000u, 0x42C80000u, 0x42C80000u, 0x42C80000u});",
        "put({0x0A000038u, 0x00100072u, E, kXyzw, E, 0x00004002u, 0x41200000u, 0x41200000u, 0x41200000u, 0x41200000u});")],
      "E is 10 x, not 100 x"),
    M("no-history-test-wrong", "K4", "clone", [("put({0x07000027u, kTempMask[3], S, kTempSelect[1], S, 0x00004001u, 0u});", "put({0x07000027u, kTempMask[3], S, kTempSelect[1], S, 0x00004001u, 1u});")],
      "a character with no previous base is valid"),
    M("limit-infinite", "K4", "clone", [("constexpr float kSkinLimitSquared = 3.6e9f;", "constexpr float kSkinLimitSquared = 3.6e19f;")],
      "a million-metre jump is E"),
    M("limit-tiny", "K4", "clone", [("constexpr float kSkinLimitSquared = 3.6e9f;", "constexpr float kSkinLimitSquared = 3.6e5f;")],
      "a 400 m move is no motion"),
    M("invalid-not-zeroed", "K4", "clone",
      [("kXyzw, E, 0x00004002u, 0u, 0u, 0u, 0u});", "kXyzw, E, 0x00004002u, 0x3F800000u, 0x3F800000u, 0x3F800000u, 0u});")],
      "an invalid pixel carries a motion of one metre per axis"),
    M("valid-flag-inverted", "K4", "clone",
      [("put({0x09000037u, kTempMask[3], E, kTempSelect[3], S, 0x00004001u, 0x3F800000u, 0x00004001u, 0u});",
        "put({0x09000037u, kTempMask[3], E, kTempSelect[3], S, 0x00004001u, 0u, 0x00004001u, 0x3F800000u});")],
      "valid is zero where the character is joined"),
    # where the clone ends
    M("anchor-after-displacement", "K5", "clone",
      [("if (depth[i] == 0 && (body[i].opcode == kIf || body[i].opcode == kLoop)) { limit = i; break; }", "if (false) { limit = i; break; }")],
      "the clone includes the game's cosmetic displacement block"),
    # the opcode whitelist
    M("whitelist-loses-an-opcode", "K1", "clone", [("{kUbfe, true, 4}, {kBfi, true, 5},  {kLdStructured, true, 4},", "{kUbfe, true, 4}, {kLdStructured, true, 4},")],
      "an opcode the five measured shaders use is declined"),
    M("whitelist-gains-sqrt", "K1", "clone", [("{kUbfe, true, 4}, {kBfi, true, 5},  {kLdStructured, true, 4},", "{kUbfe, true, 4}, {kBfi, true, 5},  {kLdStructured, true, 4}, {75, true, 2},")],
      "a square root is cloned (its input is not part of the measured set)"),
    # the declines
    M("occupied-slots-not-refused", "K3", "clone",
      [("t[at + 2] >= kSkinPrevPaletteSlot && t[at + 2] <= kSkinPoseSlot)\n                throw std::runtime_error(\"skin resource slot occupied\");",
        "false)\n                throw std::runtime_error(\"skin resource slot occupied\");")],
      "a shader that already uses t108..t110 is patched"),
    M("pose-loads-not-counted", "K1", "clone", [("if (baseLoads != 1 || posLoads != 1) throw", "if (baseLoads != 1 || posLoads != 2) throw")],
      "the pose loads are required twice: the plain shape is declined"),
    M("relative-index-allowed", "K3", "clone", [("if (repr != 0) throw std::runtime_error(\"relative or 64-bit operand index\");", "if (repr == 1) throw std::runtime_error(\"relative or 64-bit operand index\");")],
      "a computed constant buffer index inside the range is cloned"),
    M("second-return-allowed", "K3", "clone", [("if (retCount != 1 || body.empty()", "if (retCount < 1 || body.empty()")],
      "a program with two returns is patched"),
    M("unbalanced-flow-allowed", "K3", "clone", [("if (d != 0) throw std::runtime_error(\"unbalanced control flow\");", "if (false) throw std::runtime_error(\"unbalanced control flow\");")],
      "a program whose control flow does not balance is patched"),
    M("pool-stride-unchecked", "K3", "clone", [("if (t[at + 2] == 33 && t[at + 3] == 336) t33 = true;", "if (t[at + 2] == 33 && t[at + 3] > 0) t33 = true;")],
      "a t33 that is not the 336-byte pool is accepted"),
    # the pixel half and the signature
    M("export-target-moved", "K2", "clone", [("constexpr uint32_t kSkinTarget = 7;", "constexpr uint32_t kSkinTarget = 5;")],
      "E is exported to target 5"),
    M("semantic-renamed", "K2", "clone", [("constexpr char kSkinSemantic[] = \"EDVRSKINPREV\";", "constexpr char kSkinSemantic[] = \"EDVRSKINPREX\";")],
      "the output signature names another semantic"),
    # the "no history" variant of the pixel half (a skinned family's pixel shader that exports no E)
    M("zero-writes-a-value", "K2", "ev", [("const uint32_t skinZeroTail[] = {0x08000036u, 0x001020F2u, kSkinTarget, 0x00004002u, 0u, 0u, 0u, 0u};",
                                          "const uint32_t skinZeroTail[] = {0x08000036u, 0x001020F2u, kSkinTarget, 0x00004002u, 0x3F800000u, 0u, 0u, 0u};")],
      "the no-history write puts a value in E's x"),
    M("zero-writes-valid-one", "K2", "ev", [("const uint32_t skinZeroTail[] = {0x08000036u, 0x001020F2u, kSkinTarget, 0x00004002u, 0u, 0u, 0u, 0u};",
                                            "const uint32_t skinZeroTail[] = {0x08000036u, 0x001020F2u, kSkinTarget, 0x00004002u, 0u, 0u, 0u, 0x3F800000u};")],
      "the no-history write sets the valid flag"),
    M("zero-tail-skipped", "K2", "ev", [("else if (in.skinZero) out.insert(out.end(), skinZeroTail, skinZeroTail + 8);", "else if (false) out.insert(out.end(), skinZeroTail, skinZeroTail + 8);")],
      "the no-history pixel shader never writes target 7"),
    M("zero-output-undeclared", "K2", "ev", [("            out.insert(out.end(), outDecl, outDecl + 3);\n            if (in.skinExport || in.skinZero) out.insert(out.end(), skinOutDecl, skinOutDecl + 3);",
                                              "            out.insert(out.end(), outDecl, outDecl + 3);\n            if (in.skinExport) out.insert(out.end(), skinOutDecl, skinOutDecl + 3);")],
      "the no-history pixel shader writes target 7 without declaring it"),
    M("zero-signature-missing", "K2", "ev", [("                if (psInputs.skinExport || psInputs.skinZero) {\n                    SignatureElement skin;\n                    skin.name = \"SV_TARGET\";",
                                              "                if (psInputs.skinExport) {\n                    SignatureElement skin;\n                    skin.name = \"SV_TARGET\";")],
      "the output signature of the no-history pixel shader lacks target 7"),
    M("zero-and-export-allowed", "K2", "ev", [("if (inputs.skinZero && (inputs.skinExport || guardOverlayDepth", "if (false && (inputs.skinExport || guardOverlayDepth")],
      "E and the no-history write are asked for together, or with a guard or marker, and accepted"),
]

if __name__ == "__main__":
    sys.exit(lib.main(CONFIG, MUTANTS))
