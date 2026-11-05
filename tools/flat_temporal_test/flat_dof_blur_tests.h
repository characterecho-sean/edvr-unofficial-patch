#pragma once
#include "../../src/d3d11/flat_dof_blur_math.h"
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>

// The depth-of-field blur's average: the shipped decisions, and source checks that connect them to the draw scope. The shader itself runs in flat_mono_resolve_test.
namespace flat_dof_blur_tests {
template<class Check> void run(Check check) {
    using namespace edvr;

    check(flatDofBlurComposite(0xBF2302BCC7B434DBull) && flatDofBlurComposite(0xDE65BFFF2F12ECC6ull),
          "dof blur: the separate composite and the tone that blends the blur are the two draws");
    check(!flatDofBlurComposite(0) && !flatDofBlurComposite(0xE8948389E8948389ull), "dof blur: no other pixel shader is taken");

    const auto step = [](bool jittered, bool hdr, bool known, bool moved) {
        FlatDofBlurAsk a;
        a.jittered = jittered;
        a.hdrRouteTreated = hdr;
        a.cameraKnown = known;
        a.cameraMoved = moved;
        return flatDofBlurDecide(a);
    };
    check(step(true, false, true, false) == FlatDofBlurStep::Average, "dof blur: a jittered copy-route frame with a still camera is averaged");
    check(step(false, false, true, false) == FlatDofBlurStep::NotJittered, "dof blur: a frame with no jitter keeps the game's blur");
    check(step(true, true, true, false) == FlatDofBlurStep::HdrRoute, "dof blur: a frame the HDR route resolved before the blur keeps the game's blur");
    check(step(true, false, false, false) == FlatDofBlurStep::CameraUnknown, "dof blur: no comparison from last frame keeps the game's blur");
    check(step(true, false, true, true) == FlatDofBlurStep::CameraMoved, "dof blur: a moving camera keeps the game's blur");

    const float f[3] = {0.0f, -0.1745f, 0.9846f}, o[3] = {-0.5f, 1.887f, -4.118f};
    check(!flatDofBlurCameraMoved(f, f, o, o), "dof blur: a camera that did not move is still");
    {
        // The 3D menu's sway, about two 1080p pixels a second at 90 fps: about 1e-5 radians a frame.
        const float sway[3] = {1e-5f, -0.1745f, 0.9846f};
        check(!flatDofBlurCameraMoved(sway, f, o, o), "dof blur: the menu's slow sway counts as still");
        const float turn[3] = {2e-3f, -0.1745f, 0.9846f};
        check(flatDofBlurCameraMoved(turn, f, o, o), "dof blur: a turn of a blur pixel a frame counts as moved");
        const float step1[3] = {-0.5f, 1.887f, -4.108f};
        check(flatDofBlurCameraMoved(f, f, step1, o), "dof blur: 1 cm a frame counts as moved");
        const float rebase[3] = {71.6f, -5.77f, 39.8f};
        check(flatDofBlurCameraMoved(f, f, rebase, o), "dof blur: a jump to a new view counts as moved");
        const float nan = std::numeric_limits<float>::quiet_NaN();
        const float broken[3] = {nan, nan, nan};
        check(flatDofBlurCameraMoved(broken, f, o, o), "dof blur: a NaN camera counts as moved");
    }

    {
        FlatDofBlurRing r;
        bool ok = true;
        for (uint32_t i = 0; i < 8; ++i) ok = ok && r.push(8) == i && r.count == i + 1;
        check(ok, "dof blur: the ring fills slots 0 to 7, counting up");
        check(r.push(8) == 0 && r.count == 8, "dof blur: the ninth frame replaces the oldest and the count holds");
        r.restart();
        check(r.count == 0 && r.push(8) == 0 && r.count == 1, "dof blur: a restart begins again at slot 0");
    }
    {
        FlatDofBlurRing r;
        r.push(8);
        r.push(8);
        check(r.push(4) == 0 && r.count == 1 && r.window == 4, "dof blur: a new jitter cycle restarts the run at its own window");
        r.push(4);
        r.push(4);
        r.push(4);
        check(r.push(4) == 0 && r.count == 4, "dof blur: a cycle of 4 wraps at 4");
    }
    {
        FlatDofBlurRing r;
        r.push(0);
        check(r.window == 8, "dof blur: an unset cycle reads as the whole ring");
        r.push(32);
        check(r.window == 8 && r.count == 2, "dof blur: a cycle past the ring stays at the ring's size");
    }

    FlatDofBlurShape good;
    good.tex2dView = true;
    good.viewFormat = 10;
    good.mipLevels = good.arraySize = good.sampleCount = 1;
    good.width = 960;
    good.height = 540;
    check(flatDofBlurShapeOk(good), "dof blur: the lobby's blur, 960x540 half-float, is taken");
    {
        auto s = good;
        s.viewFormat = 26;
        check(flatDofBlurShapeOk(s), "dof blur: R11G11B10 is taken");
        s = good;
        s.viewFormat = 28;
        check(!flatDofBlurShapeOk(s), "dof blur: an LDR view is refused");
        s = good;
        s.tex2dView = false;
        check(!flatDofBlurShapeOk(s), "dof blur: a view that is not 2D is refused");
        s = good;
        s.mipLevels = 2;
        check(!flatDofBlurShapeOk(s), "dof blur: a texture with mips is refused");
        s = good;
        s.mostDetailedMip = 1;
        check(!flatDofBlurShapeOk(s), "dof blur: a view of a lower mip is refused");
        s = good;
        s.arraySize = 2;
        check(!flatDofBlurShapeOk(s), "dof blur: an array is refused");
        s = good;
        s.sampleCount = 4;
        check(!flatDofBlurShapeOk(s), "dof blur: MSAA is refused");
        s = good;
        s.width = 0;
        check(!flatDofBlurShapeOk(s), "dof blur: an empty texture is refused");
    }

    {
        // A failed make of the averaged images is not retried every composite.
        FlatDofBlurRetry r;
        const FlatDofBlurShapeKey a{1920, 1080, 10, 10}, b{1280, 720, 10, 10}, c{1920, 1080, 26, 26};
        check(flatDofBlurRetryAllowed(r, a, 100), "dof blur retry: with no failure on record the images are tried");
        check(flatDofBlurRetryFailed(r, a, 100), "dof blur retry: the first failure of a shape is logged");
        check(!flatDofBlurRetryAllowed(r, a, 101) && !flatDofBlurRetryAllowed(r, a, 100 + kFlatDofBlurIdleFrames - 1), "dof blur retry: the same shape waits");
        check(flatDofBlurRetryAllowed(r, a, 100 + kFlatDofBlurIdleFrames), "dof blur retry: the wait ends at kFlatDofBlurIdleFrames frames");
        check(flatDofBlurRetryAllowed(r, a, 50), "dof blur retry: a frame counter that went backwards tries again");
        check(!flatDofBlurRetryFailed(r, a, 100 + kFlatDofBlurIdleFrames), "dof blur retry: a second failure of the same shape is not logged again");
        check(!flatDofBlurRetryAllowed(r, a, 100 + kFlatDofBlurIdleFrames + 1), "dof blur retry: a retry that failed starts a new wait");
        check(flatDofBlurRetryAllowed(r, b, 101) && flatDofBlurRetryAllowed(r, c, 101), "dof blur retry: a changed size or format tries at once");
        check(flatDofBlurRetryFailed(r, b, 101), "dof blur retry: a failure of a new shape is logged");
        check(flatDofBlurRetryAllowed(r, a, 102), "dof blur retry: only the last failed shape is remembered");
        flatDofBlurRetryClear(r);
        check(flatDofBlurRetryAllowed(r, b, 102), "dof blur retry: a clear (resize, device change) tries at once");
    }

    const auto read = [](const char* path) {
        std::ifstream in(std::filesystem::path(path), std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    };
    const std::string runtime = read("src/d3d11/flat_runtime.cpp"), buildBat = read("build.bat"), shaders = read("tools/temporal_shader_build/temporal_shader_build.cpp"),
                      scope = read("src/d3d11/flat_runtime.h");
    check(!runtime.empty() && !buildBat.empty() && !shaders.empty() && !scope.empty(), "dof blur wiring: source inputs are readable");
    {
        // The hooks call beginActualDraw only for a scope that says it needs it: the first build of this set the flag and never asked, so the average never ran.
        const size_t needs = scope.find("bool needsActualDraw() const");
        const size_t bodyEnd = needs == std::string::npos ? needs : scope.find('}', needs);
        check(needs != std::string::npos && bodyEnd != std::string::npos && scope.substr(needs, bodyEnd - needs).find("dofBlur") != std::string::npos,
              "dof blur wiring: the composite's scope asks for beginActualDraw");
    }
    check(buildBat.find("\"src\\d3d11\\flat_dof_blur.cpp\"") != std::string::npos, "dof blur wiring: build.bat compiles the module into the DLL");
    check(shaders.find("kFlatDofBlurBytecode") != std::string::npos, "dof blur wiring: the shader tool makes its bytecode");
    {
        const size_t begin = runtime.find("void FlatRuntimeDrawScope::beginActualDraw"), end = runtime.find("void FlatRuntimeDrawScope::endActualDraw"),
                     dtor = runtime.find("FlatRuntimeDrawScope::~FlatRuntimeDrawScope");
        const size_t call = runtime.find("flatDofBlurBegin(", begin);
        check(begin != std::string::npos && call != std::string::npos && call < end, "dof blur wiring: beginActualDraw binds the mean");
        const size_t back = runtime.find("flatDofBlurEnd(", end);
        check(back != std::string::npos && back < dtor, "dof blur wiring: endActualDraw puts the game's view back");
        check(runtime.find("flatDofBlurEnd(", dtor) != std::string::npos, "dof blur wiring: the destructor puts it back when endActualDraw never ran");
    }
    check(runtime.find("flatDofBlurNoteCamera(") != std::string::npos, "dof blur wiring: the copy route's resolve notes the camera");
    check(runtime.find("flatDofBlurFrame(") != std::string::npos, "dof blur wiring: Present lets idle images go");
    check(runtime.find("flatDofBlurRelease(") != std::string::npos, "dof blur wiring: a reset releases the images");
    check(runtime.find("flatDofBlurLogWindow(") != std::string::npos, "dof blur wiring: the 5 s window says what it did");
}
}  // namespace flat_dof_blur_tests
