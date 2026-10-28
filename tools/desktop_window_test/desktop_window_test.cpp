// visual.desktop_window, in the build's gate (docs/desktop-window.md).
//
//   THE PURE RULES (src/common/desktop_window_math.h). The setting's words, the picture's shape from the eye and its bounds, the fit, where the picture goes in the swap
//   chain's buffer under each scaling (a window of the picture's shape is filled exactly; any other is undistorted with bars), the pixel rectangles of two eyes, the
//   window's plan on a monitor, the format answers and the three log lines tools\edvr_log.py --health reads.
//   THE WIRING (source pins, each with a control that trips it): the key is read as one word with the default game, the flat profile's allow-list does not carry it,
//   the window is moved only from the thread-pool callback, the Present hook calls the feature before the census span and the real Present, the draw skip sits in the
//   verdict and in the three thunks the verdict does not cover, the host hands the eye over after it is captured, the export and the source are built, and edvr.ini
//   documents the key in [visual] after world_scale.
// No device and no window. argv[1] is the repository root.
#include "../../src/common/desktop_window_math.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

using namespace edvr;

static unsigned g_checks = 0;
static unsigned g_failed = 0;
static void check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) {
        ++g_failed;
        std::printf("FAIL: %s\n", what);
    }
}
static bool closeTo(double a, double b, double eps = 1e-6) { return std::fabs(a - b) <= eps; }
static bool sameRect(const DwRect& r, double x, double y, double w, double h, double eps = 1e-6) {
    return closeTo(r.x, x, eps) && closeTo(r.y, y, eps) && closeTo(r.w, w, eps) && closeTo(r.h, h, eps);
}
static bool sameInts(const DwPixelRect& r, int x, int y, int w, int h) { return r.x == x && r.y == y && r.w == w && r.h == h; }

static std::string readFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}
static size_t at(const std::string& text, const char* needle, size_t from = 0) { return text.find(needle, from); }
static size_t count(const std::string& text, const char* needle) {
    size_t n = 0, p = 0;
    while ((p = text.find(needle, p)) != std::string::npos) { ++n; p += std::strlen(needle); }
    return n;
}
static std::string replaced(std::string text, const char* from, const char* to) {
    const size_t p = text.find(from);
    if (p != std::string::npos) text.replace(p, std::strlen(from), to);
    return text;
}

static void rules() {
    bool known = false;
    check(desktopWindowParse("game", &known) == DesktopWindowMode::Game && known, "game is game");
    check(desktopWindowParse("", &known) == DesktopWindowMode::Game && known, "the empty word is the default, and known");
    check(desktopWindowParse(nullptr, &known) == DesktopWindowMode::Game && known, "a missing word is the default, and known");
    check(desktopWindowParse("left_eye", &known) == DesktopWindowMode::LeftEye && known, "left_eye");
    check(desktopWindowParse("RIGHT_EYE", &known) == DesktopWindowMode::RightEye && known, "right_eye in capitals");
    check(desktopWindowParse("both_eyes", &known) == DesktopWindowMode::BothEyes && known, "both_eyes");
    // The flat profile's getString answers "off" for a key it does not allow: an unknown word, and so game.
    check(desktopWindowParse("off", &known) == DesktopWindowMode::Game && !known, "off (the flat profile's answer) is game and unknown");
    check(desktopWindowParse("left", &known) == DesktopWindowMode::Game && !known, "an unfinished word is game and unknown");
    check(desktopWindowParse("both_eyes ", &known) == DesktopWindowMode::Game && !known, "trailing text is unknown");
    check(!std::strcmp(desktopWindowModeName(DesktopWindowMode::BothEyes), "both_eyes") && !std::strcmp(desktopWindowModeName(DesktopWindowMode::Game), "game"), "mode names round-trip");
    check(desktopWindowEyeMask(DesktopWindowMode::Game) == 0 && desktopWindowEyeMask(DesktopWindowMode::LeftEye) == 1 && desktopWindowEyeMask(DesktopWindowMode::RightEye) == 2 &&
              desktopWindowEyeMask(DesktopWindowMode::BothEyes) == 3,
          "the eyes each mode needs");

    // The eye inside its bounds.
    const float whole[4] = {0, 0, 1, 1}, half[4] = {0, 0, 0.5f, 1}, flipped[4] = {1, 1, 0, 0};
    DwSize s = desktopWindowEyeSize(2000, 1000, whole);
    check(closeTo(s.w, 2000) && closeTo(s.h, 1000), "an eye with whole bounds is the texture");
    s = desktopWindowEyeSize(2000, 1000, half);
    check(closeTo(s.w, 1000) && closeTo(s.h, 1000), "an eye with half-width bounds is the half");
    s = desktopWindowEyeSize(2000, 1000, flipped);
    check(closeTo(s.w, 2000) && closeTo(s.h, 1000), "flipped bounds have the same size");

    // The picture's shape.
    DwSize eyes[2] = {{1456, 1560}, {1456, 1560}};
    double aspect = 0;
    check(desktopWindowAspect(DesktopWindowMode::LeftEye, eyes, &aspect) && closeTo(aspect, 1456.0 / 1560.0), "one eye's shape is its bounds in pixels");
    check(desktopWindowAspect(DesktopWindowMode::BothEyes, eyes, &aspect) && closeTo(aspect, 2912.0 / 1560.0), "both eyes: the sum of the widths over the height");
    eyes[1] = {1000, 1700};
    check(desktopWindowAspect(DesktopWindowMode::BothEyes, eyes, &aspect) && closeTo(aspect, 2456.0 / 1700.0), "both eyes of two sizes: the larger height");
    check(desktopWindowAspect(DesktopWindowMode::RightEye, eyes, &aspect) && closeTo(aspect, 1000.0 / 1700.0), "the right eye alone is the right eye's shape");
    eyes[1] = {0, 0};
    check(!desktopWindowAspect(DesktopWindowMode::BothEyes, eyes, &aspect) && desktopWindowAspect(DesktopWindowMode::LeftEye, eyes, &aspect), "both eyes need both sizes; one eye needs only its own");
    check(!desktopWindowAspect(DesktopWindowMode::Game, eyes, &aspect), "game has no shape");

    // The fit.
    check(sameRect(desktopWindowFit(1000, 500, 1.0), 250, 0, 500, 500), "a square in a wide region is centred, full height");
    check(sameRect(desktopWindowFit(500, 1000, 2.0), 0, 375, 500, 250), "a wide picture in a tall region is centred, full width");
    check(sameRect(desktopWindowFit(800, 400, 2.0), 0, 0, 800, 400), "a picture of the region's own shape fills it");
    check(sameRect(desktopWindowFit(0, 400, 2.0), 0, 0, 0, 0) && sameRect(desktopWindowFit(400, 400, 0), 0, 0, 0, 0), "nothing fits nowhere");

    // Where the picture goes in the buffer. A window of the picture's shape is filled exactly, under STRETCH whatever the buffer is.
    const double eyeAspect = 1456.0 / 1560.0;
    DwRect r = desktopWindowShown(DwScaling::Stretch, 1920, 1080, 972, 1041.5, eyeAspect);
    check(closeTo(r.x, 0, 0.5) && closeTo(r.y, 0, 0.5) && closeTo(r.w, 1920, 0.5) && closeTo(r.h, 1080, 0.5), "STRETCH: a client of the picture's shape takes the whole buffer");
    r = desktopWindowShown(DwScaling::Stretch, 1920, 1080, 1000, 1000, 1.0);
    check(sameRect(r, 0, 0, 1920, 1080), "STRETCH: a square client with a square picture takes the whole (stretched) buffer");
    r = desktopWindowShown(DwScaling::Stretch, 1920, 1080, 1600, 800, 1.0);
    check(closeTo((r.w * 1600.0 / 1920.0) / (r.h * 800.0 / 1080.0), 1.0, 1e-9) && r.w > 0 && r.h > 0,
          "STRETCH: after the client's stretch the picture is square (undistorted), with bars");
    r = desktopWindowShown(DwScaling::None, 1920, 1080, 1456, 1560, eyeAspect);
    check(sameRect(r, 224, 0, 1008, 1080), "NONE: the picture is fitted into the top-left client-sized region of the buffer");
    r = desktopWindowShown(DwScaling::None, 800, 600, 1000, 1000, 1.0);
    check(sameRect(r, 100, 0, 600, 600), "NONE: a client larger than the buffer shows the whole buffer");
    r = desktopWindowShown(DwScaling::AspectRatio, 1920, 1080, 1000, 1000, eyeAspect);
    check(closeTo(r.h, 1080) && closeTo(r.w, 1080 * eyeAspect) && closeTo(r.x, (1920 - r.w) / 2), "ASPECT_RATIO_STRETCH: the picture is fitted into the buffer's own shape");
    check(sameRect(desktopWindowShown(DwScaling::Stretch, 1920, 1080, 0, 100, 1.0), 0, 0, 0, 0), "an empty client draws nothing");

    // Whole pixels.
    DwPixelRect px = desktopWindowSnap(DwRect{0.4, 0.6, 99.7, 49.2}, 100, 50);
    check(sameInts(px, 0, 1, 100, 49), "snap rounds the edges and clamps to the buffer");
    check(!desktopWindowSnap(DwRect{0, 0, 0, 5}, 10, 10).ok(), "an empty rectangle stays empty");

    // Two eyes.
    DwSize two[2] = {{1000, 1000}, {1000, 1000}};
    DwPixelRect out[2];
    unsigned idx[2];
    unsigned n = desktopWindowEyeRects(DesktopWindowMode::BothEyes, DwPixelRect{0, 0, 2000, 1000}, two, out, idx);
    check(n == 2 && sameInts(out[0], 0, 0, 1000, 1000) && sameInts(out[1], 1000, 0, 1000, 1000) && idx[0] == 0 && idx[1] == 1, "two equal eyes are two halves, left on the left");
    two[1] = {500, 500};
    n = desktopWindowEyeRects(DesktopWindowMode::BothEyes, DwPixelRect{10, 20, 1500, 1000}, two, out, idx);
    check(n == 2 && sameInts(out[0], 10, 20, 1000, 1000) && sameInts(out[1], 1010, 270, 500, 500), "two unequal eyes keep their sizes and are centred vertically");
    two[1] = {1000, 1000};
    n = desktopWindowEyeRects(DesktopWindowMode::BothEyes, DwPixelRect{0, 0, 2001, 1000}, two, out, idx);
    check(n == 2 && out[0].w + out[1].w == 2001 && out[0].x + out[0].w == out[1].x, "an odd width still tiles with no gap");
    n = desktopWindowEyeRects(DesktopWindowMode::RightEye, DwPixelRect{3, 4, 50, 60}, two, out, idx);
    check(n == 1 && sameInts(out[0], 3, 4, 50, 60) && idx[0] == 1, "the right eye alone fills the shown rectangle and is eye 1");
    check(desktopWindowEyeRects(DesktopWindowMode::Game, DwPixelRect{0, 0, 10, 10}, two, out, idx) == 0, "game draws no eye");

    // The window.
    DwWindowPlan plan = desktopWindowPlan(eyeAspect, 0, 0, 1920, 1080, 16, 39);
    check(plan.ok && plan.clientHeight == 1041 && plan.clientWidth == 972 && plan.right - plan.left == 988 && plan.bottom - plan.top == 1080 && plan.left == 466 && plan.top == 0,
          "a tall eye on a 1080p monitor: as tall as the bound allows, centred");
    check(std::fabs(static_cast<double>(plan.clientWidth) / plan.clientHeight - eyeAspect) < 0.001, "the client keeps the eye's shape");
    plan = desktopWindowPlan(2.0, 0, 0, 1920, 1080, 16, 39);
    check(plan.ok && plan.clientWidth == 1904 && plan.clientHeight == 952 && plan.top == 44 && plan.left == 0, "a wide picture is as wide as the bound allows");
    plan = desktopWindowPlan(1.0, 100, 50, 1380, 1010, 0, 0);
    check(plan.ok && plan.clientWidth == 960 && plan.clientHeight == 960 && plan.left == 260 && plan.top == 50 && plan.right == 1220 && plan.bottom == 1010, "a bound that does not start at the origin; a borderless frame");
    check(!desktopWindowPlan(1.0, 0, 0, 10, 10, 16, 39).ok && !desktopWindowPlan(0.0, 0, 0, 1000, 1000, 0, 0).ok, "a bound the frame fills, and a shape of zero, plan nothing");
    const int a[4] = {10, 20, 110, 220}, b[4] = {12, 18, 108, 222}, c[4] = {13, 20, 110, 220};
    check(desktopWindowRectsNear(a, b, 2) && !desktopWindowRectsNear(a, c, 2) && desktopWindowRectsNear(a, a, 0), "two window rectangles are the same place within two pixels");

    // Formats.
    DwFormats f = desktopWindowFormats(DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM);
    check(f.ok && f.view == DXGI_FORMAT_R8G8B8A8_UNORM && f.target == DXGI_FORMAT_R8G8B8A8_UNORM && !f.srgb, "Elite's typeless eye into an RGBA buffer: UNORM both ways");
    f = desktopWindowFormats(DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_B8G8R8A8_UNORM);
    check(f.ok && f.view == DXGI_FORMAT_R8G8B8A8_UNORM && f.target == DXGI_FORMAT_B8G8R8A8_UNORM, "an RGBA eye into a BGRA buffer is the same logical colour");
    f = desktopWindowFormats(DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_R10G10B10A2_UNORM);
    check(f.ok && f.view == DXGI_FORMAT_B8G8R8A8_UNORM && f.target == DXGI_FORMAT_R10G10B10A2_UNORM, "an 8-bit eye into a 10-bit buffer");
    f = desktopWindowFormats(DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_R8G8B8A8_UNORM);
    check(f.ok && f.srgb && f.view == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB && f.target == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, "a typed sRGB eye gets an sRGB target on an 8-bit buffer");
    f = desktopWindowFormats(DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_R10G10B10A2_UNORM);
    check(!f.ok && f.reason[0], "an sRGB eye into a 10-bit buffer is refused, with a reason");
    f = desktopWindowFormats(DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R8G8B8A8_UNORM);
    check(!f.ok && std::strstr(f.reason, "eye image's format"), "a float eye is refused, and the reason names the eye");
    f = desktopWindowFormats(DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R16G16B16A16_FLOAT);
    check(!f.ok && std::strstr(f.reason, "buffer format"), "a float buffer is refused, and the reason names the buffer");
    f = desktopWindowFormats(DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
    check(f.ok && f.target == DXGI_FORMAT_R8G8B8A8_UNORM, "an sRGB-typed buffer with a UNORM eye asks for a UNORM view (the device decides whether it may)");
}

static void lines() {
    char line[400];
    desktopWindowSettingLine(line, sizeof line, "left_eye", nullptr);
    check(!std::strcmp(line, "desktop window: setting left_eye"), "the setting line");
    desktopWindowSettingLine(line, sizeof line, "both_eyes", "game");
    check(!std::strcmp(line, "desktop window: setting both_eyes (was game)"), "the setting line, changed");
    desktopWindowStatsLine(line, sizeof line, 600, 610, 1220, 610, 610, 3, 972, 1041, 1920, 1080);
    check(!std::strcmp(line, "desktop window: 10 s: drawn=600 seen=610 skipped_draws=1220 eyes_L=610 eyes_R=610 stale=3 client=972x1041 swap_chain=1920x1080"), "the 10 s line");
    desktopWindowStoppedLine(line, sizeof line, "no eye image for 500 ms (3D is off, or no frames arrive)", 600, 610, "restored to 100,100 1280x720");
    check(!std::strcmp(line, "desktop window: stopped (no eye image for 500 ms (3D is off, or no frames arrive)); drawn=600 of 610 Presents; window restored to 100,100 1280x720"),
          "the stopped line");
}

static void wiring(const std::string& root) {
    const std::string cpp = readFile(root + "\\src\\d3d11\\desktop_window.cpp");
    const std::string hook = readFile(root + "\\src\\d3d11\\device_hook.cpp");
    const std::string vscreen = readFile(root + "\\src\\d3d11\\vscreen.cpp");
    const std::string host = readFile(root + "\\src\\openxr\\native_runtime_host.h");
    const std::string build = readFile(root + "\\build.bat");
    const std::string ini = readFile(root + "\\edvr.ini");
    const std::string flatKeys = readFile(root + "\\src\\common\\flat_profile_keys.h");
    const std::string shaderTool = readFile(root + "\\tools\\temporal_shader_build\\temporal_shader_build.cpp");
    check(!cpp.empty() && !hook.empty() && !vscreen.empty() && !host.empty() && !build.empty() && !ini.empty() && !flatKeys.empty() && !shaderTool.empty(),
          "every file the wiring pins read is where they look");

    // The key: one word, default game, read live under the configuration generation; the flat profile never reads it and does not allow it.
    auto keyRead = [](const std::string& t) {
        return count(t, "cfg.getString(\"visual.desktop_window\", \"game\")") == 1 && at(t, "cfg.generation()") != std::string::npos && at(t, "desktopWindowParse(raw.c_str(), &known)") != std::string::npos;
    };
    check(keyRead(cpp), "desktop_window.cpp reads visual.desktop_window as one word, default game, under the configuration generation");
    check(!keyRead(replaced(cpp, "\"visual.desktop_window\", \"game\"", "\"visual.desktop_window\", \"left_eye\"")), "control: a key that defaults to a mode trips the pin");
    check(at(flatKeys, "desktop_window") == std::string::npos, "the flat profile's allow-list does not carry visual.desktop_window");
    check(at(cpp, "if (flat) {") != std::string::npos && at(cpp, "S.mode = DesktopWindowMode::Game;", at(cpp, "if (flat) {")) != std::string::npos, "the flat profile is game before any read");

    // The window is moved from the pool callback and nowhere else; the hook only reads it.
    auto movedOnlyByPool = [](const std::string& t) {
        const size_t set = at(t, "SetWindowPos(");
        const size_t cb = at(t, "VOID CALLBACK moveWindowCallback");
        return count(t, "SetWindowPos(") == 1 && cb != std::string::npos && set > cb && at(t, "TrySubmitThreadpoolCallback(moveWindowCallback") != std::string::npos &&
               at(t, "SWP_ASYNCWINDOWPOS | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER") != std::string::npos && at(t, "MoveWindow(") == std::string::npos &&
               at(t, "ShowWindow(") == std::string::npos;
    };
    check(movedOnlyByPool(cpp), "the window is moved by SetWindowPos in the thread-pool callback only, asynchronously, and by nothing else");
    check(!movedOnlyByPool(cpp + "\nvoid f(HWND h){ SetWindowPos(h, nullptr, 0, 0, 1, 1, 0); }\n"), "control: a SetWindowPos from the hook trips the pin");
    check(at(cpp, "(style & WS_CAPTION) == WS_CAPTION ? mi.rcWork : mi.rcMonitor") != std::string::npos, "the bound is the work area for a captioned window, else the monitor");

    // The Present hook: before the census span and before the real Present, inside hookedPresent.
    auto presentOrder = [](const std::string& t) {
        const size_t fn = at(t, "HRESULT STDMETHODCALLTYPE hookedPresent(");
        const size_t call = at(t, "desktopWindowPresent(self, dev, flags, runtimeFlatProfile())", fn);
        const size_t span = at(t, "gpuCensusFrameWorkEnd(workContext)", fn);
        const size_t real = at(t, "g_state->realPresent(self, syncInterval, flags);", span);   // the first one is the other swap chains' early return, before the feature
        return fn != std::string::npos && call != std::string::npos && span != std::string::npos && real != std::string::npos && call < span && span < real &&
               at(t, "frameTick(\"desktop_window\")", fn) != std::string::npos;
    };
    check(presentOrder(hook), "hookedPresent calls the feature, and ticks it, before the census span and the real Present");
    check(!presentOrder(replaced(hook, "desktopWindowPresent(self, dev, flags, runtimeFlatProfile())", "false")), "control: no call in the hook trips the pin");

    // The draw skip: the verdict (four thunks) and the three that do not ask it; each skip is after the foreign-context decline in the verdict.
    auto skips = [](const std::string& t) {
        const size_t verdict = at(t, "DrawVerdict beginPanelOverride(");
        const size_t station = at(t, "optimizedStationSkipsDraw(bindingShaderHash(BindSlot::Vs)", verdict);
        const size_t mine = at(t, "if (desktopWindowSkipsDraw(s)) return DrawVerdict::kSkip;", verdict);
        return verdict != std::string::npos && station != std::string::npos && mine != std::string::npos && station < mine && count(t, "desktopWindowSkipsDraw(g_state)") == 3 &&
               at(t, "void* back = desktopWindowBackIdentity();") != std::string::npos && at(t, "rt->resource != back") != std::string::npos;
    };
    check(skips(vscreen), "the verdict and the DrawAuto and indirect thunks skip a draw whose render target 0 is the swap chain's buffer");
    check(!skips(replaced(vscreen, "if (desktopWindowSkipsDraw(s)) return DrawVerdict::kSkip;", "")), "control: no skip in the verdict trips the pin");
    check(!skips(replaced(vscreen, "if (self == g_state->ownerCtx && desktopWindowSkipsDraw(g_state)) return;   // visual.desktop_window", "")), "control: a thunk without the skip trips the pin");
    check(at(cpp, "VrWorldInternalScope internal;") != std::string::npos && at(cpp, "saved.restore(ctx);") != std::string::npos, "the draw runs under the internal scope and puts the context's state back");
    check(at(cpp, "back.Get(), std::memory_order_relaxed);") != std::string::npos && at(cpp, "ComPtr<ID3D11Texture2D> back;") != std::string::npos, "the buffer is a local reference, its identity a raw pointer");

    // The host hands the eye over after it is captured, and the provider is optional.
    auto handsOver = [](const std::string& t) {
        const size_t cap = at(t, "captured.capture(eye,selected,region,flags,true)");
        const size_t note = at(t, "desktopWindow.noteEye(unsigned(eye),treated.Get(),treatedBounds);", cap);
        const size_t end = at(t, "return r;", note);
        return cap != std::string::npos && note != std::string::npos && end != std::string::npos && end - note < 200 &&
               at(t, "native_desktop_window,unavailable=") != std::string::npos && at(t, "desktopWindow.acquire(options.graphicsProvider,externalDevice,runtimeGeneration)") != std::string::npos &&
               at(t, "desktopWindow.close();") != std::string::npos;
    };
    check(handsOver(host), "the host hands each captured eye's finished texture over, acquires the provider as optional and closes it");
    check(!handsOver(replaced(host, "desktopWindow.noteEye(unsigned(eye),treated.Get(),treatedBounds);", "")), "control: no handoff trips the pin");

    // The build: the export, the source, the shaders, and the rig itself.
    check(at(build, "--extra-export edvrAcquireDesktopWindow") != std::string::npos && at(build, "\"src\\d3d11\\desktop_window.cpp\"") != std::string::npos &&
              at(build, ":rig_desktop_window_test") != std::string::npos,
          "build.bat exports edvrAcquireDesktopWindow, compiles desktop_window.cpp and runs this rig");
    check(at(shaderTool, "fixed_desktop_window_variants.h") != std::string::npos && at(shaderTool, "kDesktopWindowPsBytecode") != std::string::npos, "the shader build registers the two desktop window shaders");

    // edvr.ini: [visual], after world_scale, one ui: line with the four choices, live, and the sentence the menu reads.
    const size_t visual = at(ini, "\n[visual]\n");
    const size_t world = at(ini, "\nworld_scale = 100\n", visual);
    const size_t key = at(ini, "\ndesktop_window = game\n", world);
    const size_t ui = ini.rfind("# ui: Desktop window | choices game=Game, left_eye=Left eye, right_eye=Right eye, both_eyes=Both eyes | live | menu visual\n", key);
    check(visual != std::string::npos && world != std::string::npos && key != std::string::npos && ui != std::string::npos && ui > world && key - ui < 160 &&
              count(ini, "\ndesktop_window = ") == 1,
          "edvr.ini: [visual], after world_scale, the key ships game with its ui: line");
    const size_t says = ini.rfind("VR only. Live.\n# ui: Desktop window", key);
    check(says != std::string::npos, "edvr.ini: the comment says VR only and live");
}

int main(int argc, char** argv) {
    rules();
    lines();
    if (argc > 1) wiring(argv[1]);
    else check(false, "argv[1] is the repository root");
    std::printf("desktop_window_test: %u checks, %u failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
