#!/usr/bin/env python3
"""The mutation proof for tools\\openxr_stereo_test's fully black pass (case B1): the rig fails when a fully black frame draws the scene again.

The rig (openxr_stereo_test.cpp, B1) renders a captured pair and a skybox at fade 0, a fractional fade, and 1 and more, on the owned-immediate and the deferred
submission, with a pipeline-statistics query around the render call: a fade of 1 or more must show zero vertex, pixel and primitive work (a clear and nothing else),
its output must be pure black, and the per-frame accounting (the runtime calls, the observer's begin/end, the layer) must equal the plain frame's. The pixels alone
cannot tell a clear-only pass from one that drew the scene and cleared over it; the counts can. For each mutation below the machinery (tools\\rig_mutants_lib.py)
copies src\\openxr\\d3d11_stereo.cpp into a temp directory OUTSIDE the repo, applies the edit, rebuilds the rig against the copy (the other units and every header are the
repo's own) and requires a FAIL on a B1 check.

  python tools\\openxr_stereo_test\\mutants.py --self-test        text only: every anchor is found exactly once, every case named is in the rig, and
                                                                  build.bat compiles the rig the way the machinery does and runs this self-test
  python tools\\openxr_stereo_test\\mutants.py --run [--only a,b] [--jobs N] [--keep] [--verbose]
  python tools\\openxr_stereo_test\\mutants.py --list
  python tools\\openxr_stereo_test\\mutants.py --run --dry-run    the plan; writes nothing, starts nothing
"""
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
import rig_mutants_lib as lib  # noqa: E402

ROOT = HERE.parents[1]
FILES = {
    "stereo": ROOT / "src" / "openxr" / "d3d11_stereo.cpp",
}
# The rest of the compile line in build.bat's :rig_openxr_native_tests (the stereo rig is built in that loop beside the native one), in the repo, unmutated.
OTHER_UNITS = [
    "src/openxr/session_binding.cpp", "src/openxr/openvr_system.cpp", "src/openxr/eye_capture.cpp", "src/openxr/skybox_capture.cpp",
    "src/openxr/shared_texture_transfer.cpp", "src/openxr/producer_gpu_timing.cpp", "src/openxr/device_gpu_timing.cpp", "src/d3d11/gpu_span_d3d11.cpp",
    "src/openxr/openvr_compositor.cpp", "tools/openxr_native_test/compositor_caller.cpp", "src/openxr/openvr_auxiliary.cpp", "src/openxr/runtime_exports.cpp",
    "src/common/frame_flag.cpp",
]
CONFIG = lib.Config(
    here=__file__, files=FILES, header_keys=("stereo",), pin_keys=(), rig=HERE / "openxr_stereo_test.cpp",
    rig_label=":rig_openxr_native_tests", rig_source_in_bat="tools\\openxr_%%T_test\\openxr_%%T_test.cpp", rig_exe_in_bat="openxr_%%T_test.exe",
    case_prefix="B", include_dirs=("third_party/openxr/include", "build/gen"), include_aliases={"build/gen": '/I"%GEN%"'},
    cl_flags=["/nologo", "/W4", "/O2", "/EHsc", "/std:c++17", "/MT", "/D_CRT_SECURE_NO_WARNINGS"],
    min_mutants=6, run_timeout=300.0, rig_args=lambda root: ["--self-test"])


def build_rig(cfg, tc, tree, exe):
    """The rig as build.bat builds it, with the mutated d3d11_stereo.cpp from the temp tree. Every header, and the other units, come from the repo: the quoted includes
    of the copy that name a sibling (or ../common/...) are found through the /I of the repo's own directories."""
    mutated = tree / "src" / "openxr" / "d3d11_stereo.cpp"
    includes = ["third_party/openxr/include", "build/gen", "src/openxr", "src/common", "src/d3d11", "src/openvr/compat"]
    cmd = ([tc.cl] + cfg.cl_flags + ["/I" + str(ROOT / d) for d in includes] + ["/Fo" + str(exe.parent) + "\\", "/Fe" + str(exe), str(cfg.rig), str(mutated)] +
           [str(ROOT / u) for u in OTHER_UNITS] + ["/link", "/INCREMENTAL:NO", "dxgi.lib", "d3dcompiler.lib", "user32.lib"])
    done = subprocess.run(cmd, capture_output=True, text=True, env=tc.env, errors="replace", cwd=str(exe.parent))
    return done.returncode, done.stdout + done.stderr


lib.build_rig = build_rig

CAPTURED_BLACK = "    auto* rtv=eyes_[i].rtvs[index].Get();\n    if(fade>=1.f) {"
SKYBOX_BLACK = "    auto* rtv=e.rtvs[index].Get();\n    if(fade>=1.f) {"
CAPTURED_CLEAR = "      const float black[]={0,0,0,1};drawContext->ClearRenderTargetView(rtv,black);\n    } else {"
SKYBOX_CLEAR = "      const float black[]={0,0,0,1};context_->ClearRenderTargetView(rtv,black);\n    } else {"

M = lib.M
MUTANTS = [
    M("black-draws-the-scene-captured", "B1", "stereo", [(CAPTURED_BLACK, CAPTURED_BLACK.replace("if(fade>=1.f) {", "if(false) {"))],
      "a fully black captured frame is drawn as before (the scene, then the fade pass at level 1) and comes out black all the same: only the counts see it"),
    M("black-draws-the-scene-skybox", "B1", "stereo", [(SKYBOX_BLACK, SKYBOX_BLACK.replace("if(fade>=1.f) {", "if(false) {"))],
      "a fully black skybox frame is drawn as before and comes out black all the same"),
    M("black-not-cleared-captured", "B1", "stereo", [(CAPTURED_CLEAR, "      (void)rtv;\n    } else {")],
      "a fully black captured frame draws nothing and clears nothing: the image is whatever the swapchain held"),
    M("black-not-cleared-skybox", "B1", "stereo", [(SKYBOX_CLEAR, "      (void)rtv;\n    } else {")],
      "a fully black skybox frame draws nothing and clears nothing"),
    M("fade-pass-dropped-captured", "B1", "stereo", [("      if(fade>0.f) {\n        // The same triangle", "      if(false) {\n        // The same triangle")],
      "a fractional fade is never darkened: the plain image under every level below 1"),
    M("fade-pass-dropped-skybox", "B1", "stereo", [("      if(fade>0.f) {\n        const float factor", "      if(false) {\n        const float factor")],
      "a fractional fade over the skybox is never darkened"),
]

if __name__ == "__main__":
    sys.exit(lib.main(CONFIG, MUTANTS))
