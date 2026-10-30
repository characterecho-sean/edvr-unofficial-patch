// visual.desktop_window, in the build's gate (docs/desktop-window.md).
//
//   THE PURE RULES (src/common/desktop_window_math.h). The setting's words, the picture's shape from the eye and its bounds, the fit, where the picture goes in the swap
//   chain's buffer under each scaling (a window of the picture's shape is filled exactly; any other is undistorted with bars), the pixel rectangles of two eyes, the
//   window's plan on a monitor, the format answers and the three log lines tools\edvr_log.py --health reads.
//   THE WINDOW RULES (round 2): the framed, sizable style (0x94020000 -> 0x14CC0000), which style bits are ours, when a client is off the picture's shape, the correction that
//   keeps the dimension the user changed more and clamps to the work area (minimum client height 240), and the census of the skipped draws.
//   THE CURSOR (rounds 3 to 5): the client-centre test, the DirectInput cooperative level's words and
//   the cursor line; and the pins that hold the hooks to passing through everything but that one call.
//   THE HIDDEN CURSOR (round 4): the rule (foreground and inside the client and active), the show-count protocol against a fake ShowCursor (the game sees the counts it would
//   see without EDVR, a game "show" never leaves the real count at 0 or above while hiding is wanted, the hides are given back), and the pins on the thread check and the undo.
//   THE PICTURE'S SHAPE AND THE STYLE (round 6): the aspect and style words, the centre crop (an off-centre centre, the clamp at an image
//   edge, a flipped sub-rect), both eyes as halves, the borderless style and rectangle (0x94020000 and 0x14CC0000 both become 0x94000000), a shape change keeping the client height,
//   and the pins on the page, the keys, the version 2 export and the live change.
//   THE CROP RULE (round 8, replacing round 6's projection-centre crop): visual.desktop_window_crop = native | center. center: the shape around the middle of the eye's bounds, no
//   zoom. native: what Elite's own mirror does (analysis\decomp\verify_20261028_vr_mirror_crop.txt, build 334925): the same as center without a hidden-area mesh, and with one a
//   1.32x zoom centred at u 0.515 for the left eye, mirrored (0.485) for the right. full is never cropped or zoomed. The words, the constants, the plan, the log words, the numbers
//   on a 4032x4034 eye (the note's own Pimax figures), both eyes as halves, a sub-rect, a flipped sub-rect, the vertical shift on top of each rule; the mesh flag through the
//   export (a fake provider behind the real client: version 3 carries it, version 1 does not, version 2 is gone), and the pins on the key, the plan's use, the handoff and the
//   runtime's answer to GetHiddenAreaMesh.
//   THE VERTICAL SHIFT (round 7): the crop moved up (+) or down (-) a fraction of the way from its default position to the picture's top or bottom edge: 0 is the crop without a
//   shift, +-100 reach the edges from any default, +-50 are halfway from an off-centre default, no travel is no change, out of range is held to +-100, a sub-rect works in the
//   texture's UVs, a flipped sub-rect moves as the viewer sees it; and the pins on the key (one whole number, -100..100, default 0, flat resets it), the call that gives it to the
//   crop, the log lines, the ini words (0 is the default centre) and the ini order. Each check has a control that trips.
//   THE WIRING (source pins, each with a control that trips it): the key is read as one word with the default left_eye (game until F173), the flat profile's allow-list does not carry it,
//   the window is moved only from the thread-pool callback, the Present hook calls the feature before the census span and the real Present, the draw skip sits in the
//   verdict and in the three thunks the verdict does not cover, the host hands the eye over after it is captured, the export and the source are built, and edvr.ini
//   documents the key in [visual] after world_scale.
// No device and no window. argv[1] is the repository root.
#include "../../src/common/desktop_window_census.h"
#include "../../src/common/desktop_window_math.h"
#include "../../src/openxr/native_desktop_window_client.h"
#include "../../src/d3d11/desktop_vsync.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
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
    check(desktopWindowParse("", &known) == DesktopWindowMode::Game && known, "the empty word reads as game (the parser's own, not the key's default), and known");
    check(desktopWindowParse(nullptr, &known) == DesktopWindowMode::Game && known, "a missing word reads as game (the parser's own, not the key's default), and known");
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

static void shapeRules() {
    bool known = false;
    // The words.
    check(desktopWindowParseAspect("full", &known) == DesktopWindowAspect::Full && known && desktopWindowParseAspect("", &known) == DesktopWindowAspect::Full && known &&
              desktopWindowParseAspect(nullptr, &known) == DesktopWindowAspect::Full && known && desktopWindowParseAspect("FULL", &known) == DesktopWindowAspect::Full && known,
          "full is the default word, and the missing one");
    check(desktopWindowParseAspect("1:1", &known) == DesktopWindowAspect::Square && known && desktopWindowParseAspect("4:3", &known) == DesktopWindowAspect::FourThree && known &&
              desktopWindowParseAspect("16:10", &known) == DesktopWindowAspect::SixteenTen && known && desktopWindowParseAspect("16:9", &known) == DesktopWindowAspect::SixteenNine && known &&
              desktopWindowParseAspect("21:9", &known) == DesktopWindowAspect::TwentyOneNine && known,
          "the five shapes, with their colons");
    check(desktopWindowParseAspect("16x9", &known) == DesktopWindowAspect::Full && !known && desktopWindowParseAspect("16:9 ", &known) == DesktopWindowAspect::Full && !known &&
              desktopWindowParseAspect("off", &known) == DesktopWindowAspect::Full && !known,
          "any other word is full and unknown (the flat profile's \"off\" included)");
    check(!std::strcmp(desktopWindowAspectName(DesktopWindowAspect::SixteenNine), "16:9") && !std::strcmp(desktopWindowAspectName(DesktopWindowAspect::Full), "full") &&
              !std::strcmp(desktopWindowAspectName(DesktopWindowAspect::TwentyOneNine), "21:9"),
          "the shapes' names round-trip");
    check(desktopWindowAspectRatio(DesktopWindowAspect::Full) == 0.0 && closeTo(desktopWindowAspectRatio(DesktopWindowAspect::SixteenNine), 16.0 / 9.0) &&
              closeTo(desktopWindowAspectRatio(DesktopWindowAspect::SixteenTen), 1.6) && closeTo(desktopWindowAspectRatio(DesktopWindowAspect::FourThree), 4.0 / 3.0) &&
              closeTo(desktopWindowAspectRatio(DesktopWindowAspect::Square), 1.0) && closeTo(desktopWindowAspectRatio(DesktopWindowAspect::TwentyOneNine), 21.0 / 9.0),
          "width over height of each shape, 0 for full");
    check(desktopWindowParseStyle("window", &known) == DesktopWindowStyle::Window && known && desktopWindowParseStyle("borderless", &known) == DesktopWindowStyle::Borderless && known &&
              desktopWindowParseStyle("", &known) == DesktopWindowStyle::Window && known && desktopWindowParseStyle("fullscreen", &known) == DesktopWindowStyle::Window && !known &&
              desktopWindowParseStyle("off", &known) == DesktopWindowStyle::Window && !known,
          "the style words: window is the default, borderless the other, anything else is window and unknown");
    check(!std::strcmp(desktopWindowStyleName(DesktopWindowStyle::Borderless), "borderless") && !std::strcmp(desktopWindowStyleName(DesktopWindowStyle::Window), "window"), "the style names round-trip");

    // The borderless style and its rectangle.
    check(desktopWindowBorderlessStyle(0x94020000u) == 0x94000000u, "the game's popup 0x94020000 becomes the bare popup 0x94000000 (the minimize box goes)");
    check(desktopWindowBorderlessStyle(0x14CC0000u) == 0x94000000u, "the framed window 0x14CC0000 becomes 0x94000000");
    check(desktopWindowBorderlessStyle(0x94000000u) == 0x94000000u, "the bare popup is its own fixed point");
    check(desktopWindowBorderlessStyle(0x17CF0000u) == 0x96000000u && desktopWindowBorderlessStyle(0x16CF0000u) == 0x96000000u, "Elite's own windowed style loses its frame and boxes, keeps visible and the clip bits");
    check(desktopWindowBorderlessStyle(0x84020000u) == 0x84000000u, "a window that is not visible stays not visible");
    check(desktopWindowStyleFor(DesktopWindowStyle::Borderless, 0x94020000u) == 0x94000000u && desktopWindowStyleFor(DesktopWindowStyle::Window, 0x94020000u) == 0x14CC0000u,
          "each style mode's style for the game's popup");
    check(desktopWindowStyleIsOurs(0x94000000u, 0x94000000u) && !desktopWindowStyleIsOurs(0x94020000u, 0x94000000u) && !desktopWindowStyleIsOurs(0x14CC0000u, 0x94000000u) &&
              !desktopWindowStyleIsOurs(0x94000000u, 0x14CC0000u),
          "the borderless style is told from the game's popup (its minimize box) and from the framed one, both ways");
    DwWindowPlan full = desktopWindowBorderlessPlan(-1920, 0, 0, 1080);
    check(full.ok && full.left == -1920 && full.top == 0 && full.right == 0 && full.bottom == 1080 && full.clientWidth == 1920 && full.clientHeight == 1080,
          "the borderless rectangle is the whole monitor, client and all, on a monitor to the left too");
    check(!desktopWindowBorderlessPlan(0, 0, 0, 1080).ok && !desktopWindowBorderlessPlan(10, 10, 5, 20).ok, "a monitor with no size has no borderless rectangle");

    // The crop. A 2000x2000 image, 16:9: the whole width, 1125 rows, the centre decides which.
    const float whole[4] = {0, 0, 1, 1};
    float out[4];
    desktopWindowCrop(whole, 2000, 2000, 16.0 / 9.0, true, 0.5, 0.3, out);
    check(closeTo(out[0], 0.0, 1e-6) && closeTo(out[2], 1.0, 1e-6) && closeTo(out[1], 0.01875, 1e-6) && closeTo(out[3], 0.58125, 1e-6),
          "16:9 of a square image, centred on v=0.3: the full width and 1125 rows from 0.01875");
    desktopWindowCrop(whole, 2000, 2000, 16.0 / 9.0, true, 0.5, 0.05, out);
    check(closeTo(out[1], 0.0, 1e-6) && closeTo(out[3], 0.5625, 1e-6), "a centre near the top: the crop is moved down to the image's edge, no further");
    desktopWindowCrop(whole, 2000, 2000, 16.0 / 9.0, true, 0.5, 0.95, out);
    check(closeTo(out[1], 0.4375, 1e-6) && closeTo(out[3], 1.0, 1e-6), "a centre near the bottom: the crop is moved up to the image's edge");
    desktopWindowCrop(whole, 2000, 2000, 16.0 / 9.0, true, 0.5, 3.0, out);
    check(closeTo(out[1], 0.4375, 1e-6) && closeTo(out[3], 1.0, 1e-6), "a centre outside the image is held at its edge");
    desktopWindowCrop(whole, 2000, 2000, 16.0 / 9.0, false, 0.9, 0.9, out);
    check(closeTo(out[1], 0.21875, 1e-6) && closeTo(out[3], 0.78125, 1e-6), "no centre: the middle of the bounds");
    desktopWindowCrop(whole, 2000, 2000, 0.5, true, 0.2, 0.5, out);
    check(closeTo(out[0], 0.0, 1e-6) && closeTo(out[2], 0.5, 1e-6) && closeTo(out[1], 0.0, 1e-6) && closeTo(out[3], 1.0, 1e-6),
          "a tall target (1:2) of a square image, centred on u=0.2: 1000 columns, clamped to the left edge");
    const float half[4] = {0, 0, 0.5f, 1};
    desktopWindowCrop(half, 4000, 1000, 1.0, true, 0.6, 0.5, out);
    check(closeTo(out[0], 0.175, 1e-6) && closeTo(out[2], 0.425, 1e-6) && closeTo(out[1], 0.0, 1e-6) && closeTo(out[3], 1.0, 1e-6),
          "a sub-rect of a texture: 1000x1000 of its 2000x1000, centred on u=0.6, in texture UVs");
    desktopWindowCrop(half, 4000, 2000, 2.0, true, 0.25, 0.5, out);
    check(closeTo(out[0], 0.0, 1e-6) && closeTo(out[2], 0.5, 1e-6) && closeTo(out[1], 0.25, 1e-6) && closeTo(out[3], 0.75, 1e-6), "a wider target than the sub-rect crops its height, in the texture's UVs");
    const float flipped[4] = {1, 1, 0, 0};
    desktopWindowCrop(flipped, 2000, 1000, 1.0, true, 0.25, 0.5, out);
    check(closeTo(out[0], 1.0, 1e-6) && closeTo(out[2], 0.5, 1e-6) && closeTo(out[1], 1.0, 1e-6) && closeTo(out[3], 0.0, 1e-6), "a flipped sub-rect stays flipped, and the centre runs along the flip");
    desktopWindowCrop(whole, 1600, 900, 16.0 / 9.0, true, 0.3, 0.3, out);
    check(closeTo(out[0], 0.0, 1e-6) && closeTo(out[2], 1.0, 1e-6) && closeTo(out[1], 0.0, 1e-6) && closeTo(out[3], 1.0, 1e-6), "a target of the image's own shape leaves the bounds");
    desktopWindowCrop(whole, 2000, 2000, 0.0, true, 0.3, 0.3, out);
    check(out[0] == 0 && out[1] == 0 && out[2] == 1 && out[3] == 1, "a target of 0 (full) leaves the bounds as they are");

    // ROUND 7, THE VERTICAL SHIFT. 2000x2000, 16:9: 2000 x 1125 pixels, so 875 rows of travel (top 0..875 = out[1] 0..0.4375). Centre v=0.3 puts the default top at 37.5 rows
    // (0.01875), centre v=0.6 at 637.5 (0.31875). The shift moves a fraction of the way from the default to the top edge (positive) or the bottom edge (negative).
    float shifted[4], base[4];
    auto cropOf = [&](const float* b, uint32_t tw, uint32_t th, double target, double cu, double cv, int shift, float* o) { desktopWindowCrop(b, tw, th, target, true, cu, cv, o, shift); };
    auto sameBits = [](const float* a, const float* b) { return a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3]; };
    // 0 changes nothing: bit for bit the crop without the argument, for an upright, a shifted-default, a sub-rect and a flipped case.
    {
        const float rects[4][4] = {{0, 0, 1, 1}, {0, 0, 1, 1}, {0, 0, 0.5f, 1}, {1, 1, 0, 0}};
        const uint32_t sizes[4][2] = {{2000, 2000}, {2000, 2000}, {4000, 2000}, {1000, 2000}};
        const double targets[4] = {16.0 / 9.0, 16.0 / 9.0, 2.0, 1.0}, centreV[4] = {0.3, 0.6, 0.4, 0.5};
        bool identity = true, control = false;
        for (int i = 0; i < 4; ++i) {
            desktopWindowCrop(rects[i], sizes[i][0], sizes[i][1], targets[i], true, 0.5, centreV[i], base);
            cropOf(rects[i], sizes[i][0], sizes[i][1], targets[i], 0.5, centreV[i], 0, shifted);
            identity = identity && sameBits(base, shifted);
            cropOf(rects[i], sizes[i][0], sizes[i][1], targets[i], 0.5, centreV[i], 5, shifted);
            control = control || !sameBits(base, shifted);
        }
        check(identity, "a shift of 0 is the crop without a shift, bit for bit, in an upright, an off-centre, a sub-rect and a flipped case");
        check(control, "control: a shift of 5 is not that crop (the check can tell a shift from none)");
    }
    // +-100 reach the edges, whatever the default was.
    desktopWindowCrop(whole, 2000, 2000, 16.0 / 9.0, true, 0.5, 0.3, base);
    cropOf(whole, 2000, 2000, 16.0 / 9.0, 0.5, 0.3, 100, out);
    check(closeTo(out[1], 0.0, 1e-6) && closeTo(out[3], 0.5625, 1e-6) && closeTo(out[0], 0.0, 1e-6) && closeTo(out[2], 1.0, 1e-6), "+100 puts the crop's top edge at the picture's top (v 0..0.5625), all the width");
    cropOf(whole, 2000, 2000, 16.0 / 9.0, 0.5, 0.3, -100, shifted);
    check(closeTo(shifted[1], 0.4375, 1e-6) && closeTo(shifted[3], 1.0, 1e-6) && closeTo(shifted[0], 0.0, 1e-6) && closeTo(shifted[2], 1.0, 1e-6),
          "-100 puts the crop's bottom edge at the picture's bottom (v 0.4375..1), all the width");
    check(!closeTo(base[1], 0.0, 1e-6) && !closeTo(base[3], 1.0, 1e-6) && !closeTo(out[1], shifted[1], 1e-3), "control: the default crop is at neither edge, and +100 is not -100");
    cropOf(whole, 2000, 2000, 16.0 / 9.0, 0.5, 0.6, 100, out);
    cropOf(whole, 2000, 2000, 16.0 / 9.0, 0.5, 0.6, -100, shifted);
    check(closeTo(out[1], 0.0, 1e-6) && closeTo(shifted[1], 0.4375, 1e-6) && closeTo(shifted[3], 1.0, 1e-6), "the edges are the same from another default (v=0.6)");
    // +-50 from an off-centre default: halfway from where it is to the edge. v=0.6 -> default top 0.31875; +50 -> 0.159375; -50 -> 0.378125 (637.5 + (875 - 637.5) / 2 = 756.25 rows).
    desktopWindowCrop(whole, 2000, 2000, 16.0 / 9.0, true, 0.5, 0.6, base);
    cropOf(whole, 2000, 2000, 16.0 / 9.0, 0.5, 0.6, 50, out);
    cropOf(whole, 2000, 2000, 16.0 / 9.0, 0.5, 0.6, -50, shifted);
    check(closeTo(base[1], 0.31875, 1e-6) && closeTo(out[1], 0.159375, 1e-6) && closeTo(out[3], 0.721875, 1e-6) && closeTo(shifted[1], 0.378125, 1e-6) && closeTo(shifted[3], 0.940625, 1e-6),
          "+50 and -50 from the default top 0.31875: halfway to the top edge (0.159375) and halfway to the bottom edge (0.378125), the height kept");
    check(out[1] < base[1] && shifted[1] > base[1] && !closeTo(out[1], 0.21875, 1e-3),
          "control: positive is up, negative is down, and the shift is relative to the default, not a place in the travel (+50 as a place would be 0.21875)");
    cropOf(whole, 2000, 2000, 16.0 / 9.0, 0.5, 0.3, 50, out);
    cropOf(whole, 2000, 2000, 16.0 / 9.0, 0.5, 0.3, -50, shifted);
    check(closeTo(out[1], 0.009375, 1e-6) && closeTo(shifted[1], 0.228125, 1e-6) && closeTo(shifted[3] - shifted[1], 0.5625, 1e-6), "+-50 from the default top 0.01875: 0.009375 up, 0.228125 down");
    // The horizontal position is untouched by any shift, and a crop that fills the picture's height has no travel.
    cropOf(whole, 2000, 2000, 0.5, 0.2, 0.5, 0, base);
    cropOf(whole, 2000, 2000, 0.5, 0.2, 0.5, 100, out);
    cropOf(whole, 2000, 2000, 0.5, 0.2, 0.5, -100, shifted);
    check(sameBits(base, out) && sameBits(base, shifted) && closeTo(out[0], 0.0, 1e-6) && closeTo(out[2], 0.5, 1e-6) && closeTo(out[1], 0.0, 1e-6) && closeTo(out[3], 1.0, 1e-6),
          "a tall target (1:2) of a square fills the picture's height: no travel, so +-100 leave the crop where it was, columns included");
    cropOf(whole, 3000, 2000, 16.0 / 9.0, 0.8, 0.5, 0, base);
    cropOf(whole, 3000, 2000, 16.0 / 9.0, 0.8, 0.5, 100, out);
    cropOf(whole, 3000, 2000, 16.0 / 9.0, 0.8, 0.5, -100, shifted);
    check(closeTo(base[1], 0.078125, 1e-6) && closeTo(out[1], 0.0, 1e-6) && closeTo(out[3], 0.84375, 1e-6) && closeTo(shifted[1], 0.15625, 1e-6) && closeTo(shifted[3], 1.0, 1e-6) &&
              closeTo(out[0], base[0], 1e-9) && closeTo(out[2], base[2], 1e-9) && closeTo(shifted[0], base[0], 1e-9) && closeTo(shifted[2], base[2], 1e-9),
          "a 16:9 out of a 3:2 picture (312.5 rows of travel, default top 156.25): +100 is at the top, -100 at the bottom, the columns are the default's");
    // No travel, no change: a target of the picture's own shape, and full (target 0).
    cropOf(whole, 2000, 2000, 1.0, 0.3, 0.3, 0, base);
    cropOf(whole, 2000, 2000, 1.0, 0.3, 0.3, 100, out);
    cropOf(whole, 2000, 2000, 1.0, 0.3, 0.3, -100, shifted);
    check(sameBits(base, out) && sameBits(base, shifted) && closeTo(base[1], 0.0, 1e-9) && closeTo(base[3], 1.0, 1e-9), "a target of the picture's own shape has no room up or down: +-100 change nothing");
    cropOf(whole, 1600, 900, 16.0 / 9.0, 0.3, 0.3, 0, base);
    cropOf(whole, 1600, 900, 16.0 / 9.0, 0.3, 0.3, 100, out);
    cropOf(whole, 1600, 900, 16.0 / 9.0, 0.3, 0.3, -100, shifted);
    check(closeTo(out[1], base[1], 1e-9) && closeTo(out[3], base[3], 1e-9) && closeTo(shifted[1], base[1], 1e-9) && closeTo(shifted[3], base[3], 1e-9), "the same for a 16:9 out of a 16:9 picture (to rounding)");
    cropOf(whole, 2000, 2000, 0.0, 0.3, 0.3, 75, out);
    check(out[0] == 0 && out[1] == 0 && out[2] == 1 && out[3] == 1, "full (target 0) with a shift leaves the bounds as they are");
    desktopWindowCrop(whole, 2000, 2000, 16.0 / 9.0, false, 0.9, 0.9, base);
    desktopWindowCrop(whole, 2000, 2000, 16.0 / 9.0, false, 0.9, 0.9, out, 100);
    desktopWindowCrop(whole, 2000, 2000, 16.0 / 9.0, false, 0.9, 0.9, shifted, -100);
    check(closeTo(base[1], 0.21875, 1e-6) && closeTo(out[1], 0.0, 1e-6) && closeTo(out[3], 0.5625, 1e-6) && closeTo(shifted[1], 0.4375, 1e-6) && closeTo(shifted[3], 1.0, 1e-6),
          "with no centre the default is the middle (0.21875) and the shift moves it from there");
    // Out of range is held to +-100.
    float up100[4], down100[4], theDefault[4];
    desktopWindowCrop(whole, 2000, 2000, 16.0 / 9.0, true, 0.5, 0.3, theDefault);
    cropOf(whole, 2000, 2000, 16.0 / 9.0, 0.5, 0.3, 100, up100);
    cropOf(whole, 2000, 2000, 16.0 / 9.0, 0.5, 0.3, -100, down100);
    cropOf(whole, 2000, 2000, 16.0 / 9.0, 0.5, 0.3, 101, out);
    cropOf(whole, 2000, 2000, 16.0 / 9.0, 0.5, 0.3, 250, shifted);
    check(sameBits(up100, out) && sameBits(up100, shifted), "a shift of 101 or 250 is 100");
    cropOf(whole, 2000, 2000, 16.0 / 9.0, 0.5, 0.3, 2147483647, out);
    check(sameBits(up100, out), "the largest int is 100");
    cropOf(whole, 2000, 2000, 16.0 / 9.0, 0.5, 0.3, -101, out);
    cropOf(whole, 2000, 2000, 16.0 / 9.0, 0.5, 0.3, -250, shifted);
    check(sameBits(down100, out) && sameBits(down100, shifted), "a shift of -101 or -250 is -100");
    cropOf(whole, 2000, 2000, 16.0 / 9.0, 0.5, 0.3, -2147483647 - 1, out);
    check(sameBits(down100, out), "the smallest int is -100");
    check(!sameBits(up100, theDefault) && !sameBits(down100, theDefault) && !sameBits(up100, down100),
          "control: the held shifts are neither the default crop (an out-of-range shift ignored or wrapped) nor each other");
    // A sub-rect of a texture: the shift runs in the texture's own UVs, the columns stay. 4000x2000 texture, left half, 2:1: 2000 x 1000 pixels, 1000 rows of travel.
    desktopWindowCrop(half, 4000, 2000, 2.0, true, 0.25, 0.5, base);
    cropOf(half, 4000, 2000, 2.0, 0.25, 0.5, 100, out);
    cropOf(half, 4000, 2000, 2.0, 0.25, 0.5, -100, shifted);
    check(closeTo(base[1], 0.25, 1e-6) && closeTo(out[1], 0.0, 1e-6) && closeTo(out[3], 0.5, 1e-6) && closeTo(shifted[1], 0.5, 1e-6) && closeTo(shifted[3], 1.0, 1e-6) &&
              closeTo(out[0], 0.0, 1e-6) && closeTo(out[2], 0.5, 1e-6) && closeTo(shifted[0], 0.0, 1e-6) && closeTo(shifted[2], 0.5, 1e-6),
          "a sub-rect (the left half of a 4000x2000 texture): +-100 reach v 0 and v 1 in the texture's UVs, u stays 0..0.5");
    // A flipped sub-rect: "up" is the side the draw puts at the window's top, which is where the bounds start (bounds[1], here v=1); it mirrors the upright crop.
    const float tallWhole[4] = {0, 0, 1, 1}, tallFlipped[4] = {1, 1, 0, 0};
    bool mirrors = true, differs = false;
    for (int s = -100; s <= 100; s += 25) {
        cropOf(tallWhole, 1000, 2000, 1.0, 0.5, 0.5, s, base);
        cropOf(tallFlipped, 1000, 2000, 1.0, 0.5, 0.5, s, out);
        mirrors = mirrors && closeTo(out[1], 1.0 - base[1], 1e-6) && closeTo(out[3], 1.0 - base[3], 1e-6) && closeTo(out[0], 1.0 - base[0], 1e-6) && closeTo(out[2], 1.0 - base[2], 1e-6);
        differs = differs || !closeTo(out[1], base[1], 1e-3);
    }
    check(mirrors, "a flipped sub-rect moves the way an upright one does as the viewer sees it: every shift from -100 to 100 is the upright crop mirrored");
    check(differs, "control: the flipped crop is not the upright one's numbers (the mirror is real)");
    cropOf(tallFlipped, 1000, 2000, 1.0, 0.5, 0.5, 100, out);
    cropOf(tallFlipped, 1000, 2000, 1.0, 0.5, 0.5, -100, shifted);
    check(closeTo(out[1], 1.0, 1e-6) && closeTo(out[3], 0.5, 1e-6) && closeTo(shifted[1], 0.5, 1e-6) && closeTo(shifted[3], 0.0, 1e-6) && closeTo(out[0], 1.0, 1e-6) && closeTo(out[2], 0.0, 1e-6),
          "a flipped 1000x2000 sub-rect: +100 starts at the bounds' start (v=1) and ends at 0.5; -100 ends at the bounds' end (v=0)");
    // Both eyes get the same shift: the crop of two eyes with the same bounds and different centres shifts by the same rows (top edge at 0 for both at +100).
    cropOf(whole, 2000, 2000, 16.0 / 9.0, 0.45, 0.35, 100, base);
    cropOf(whole, 2000, 2000, 16.0 / 9.0, 0.55, 0.35, 100, out);
    check(closeTo(base[1], 0.0, 1e-6) && closeTo(out[1], 0.0, 1e-6) && closeTo(base[3], out[3], 1e-9), "two eyes with the same shift: the same rows");

    // ROUND 8, THE CROP RULE. The words.
    check(desktopWindowParseCropRule("native", &known) == DesktopWindowCropRule::Native && known && desktopWindowParseCropRule("", &known) == DesktopWindowCropRule::Native && known &&
              desktopWindowParseCropRule(nullptr, &known) == DesktopWindowCropRule::Native && known && desktopWindowParseCropRule("NATIVE", &known) == DesktopWindowCropRule::Native && known,
          "native is the default word, and the missing one");
    check(desktopWindowParseCropRule("center", &known) == DesktopWindowCropRule::Center && known && desktopWindowParseCropRule("Center", &known) == DesktopWindowCropRule::Center && known,
          "center is the other word (so native is not every word)");
    check(desktopWindowParseCropRule("centre", &known) == DesktopWindowCropRule::Native && !known && desktopWindowParseCropRule("off", &known) == DesktopWindowCropRule::Native && !known &&
              desktopWindowParseCropRule("native ", &known) == DesktopWindowCropRule::Native && !known,
          "any other word is native and unknown (the menu says Centre, the file says center; the flat profile's \"off\" too)");
    check(!std::strcmp(desktopWindowCropRuleName(DesktopWindowCropRule::Native), "native") && !std::strcmp(desktopWindowCropRuleName(DesktopWindowCropRule::Center), "center"),
          "the rules' names round-trip");
    // Elite's constants.
    auto constantsOk = [](double zoom, double pan, double centre) {
        return closeTo(zoom, 1.32, 1e-12) && closeTo(pan, -0.03, 1e-12) && closeTo(centre, 0.515, 1e-12) && closeTo(centre, 0.5 - 0.5 * pan, 1e-12);
    };
    check(constantsOk(kDwMirrorZoom, kDwMirrorPan, kDwMirrorCentreU), "Elite's mirror constants: zoom 1.32, pan -0.03, so the left eye's centre is 0.515");
    check(!constantsOk(1.3, kDwMirrorPan, kDwMirrorCentreU) && !constantsOk(kDwMirrorZoom, -0.02, kDwMirrorCentreU) && !constantsOk(kDwMirrorZoom, kDwMirrorPan, 0.5) &&
              !constantsOk(kDwMirrorZoom, kDwMirrorPan, 0.51),
          "control: a different zoom, pan or centre trips the constants pin");
    // The plan.
    {
        auto plain = [](const DwCropPlan& p) { return p.zoom == 1.0 && p.u == 0.5 && p.v == 0.5 && !p.zoomed; };
        const DwCropPlan centerLeft = desktopWindowCropPlan(DesktopWindowCropRule::Center, true, 0), centerRight = desktopWindowCropPlan(DesktopWindowCropRule::Center, true, 1);
        const DwCropPlan noMeshLeft = desktopWindowCropPlan(DesktopWindowCropRule::Native, false, 0), noMeshRight = desktopWindowCropPlan(DesktopWindowCropRule::Native, false, 1);
        const DwCropPlan meshLeft = desktopWindowCropPlan(DesktopWindowCropRule::Native, true, 0), meshRight = desktopWindowCropPlan(DesktopWindowCropRule::Native, true, 1);
        check(plain(centerLeft) && plain(centerRight), "center: the texture's centre and no zoom, for either eye, even when a mesh was served");
        check(plain(noMeshLeft) && plain(noMeshRight), "native with no mesh is the same as center (Elite's mode 7)");
        check(meshLeft.zoomed && closeTo(meshLeft.zoom, 1.32, 1e-12) && closeTo(meshLeft.u, 0.515, 1e-12) && meshLeft.v == 0.5, "native with a mesh, left eye: 1.32x at u 0.515, v 0.5 (Elite's mode 9)");
        check(meshRight.zoomed && closeTo(meshRight.zoom, 1.32, 1e-12) && closeTo(meshRight.u, 0.485, 1e-12) && meshRight.v == 0.5 && closeTo(meshLeft.u + meshRight.u, 1.0, 1e-12),
              "native with a mesh, right eye: the mirror of the left, u 0.485");
        check(!plain(meshLeft) && !plain(meshRight) && meshLeft.u != meshRight.u && plain(noMeshLeft),
              "control: a mesh changes the plan under native, and the two eyes' centres differ");
    }
    // The log words.
    {
        char text[160];
        desktopWindowCropRuleText(text, sizeof text, DesktopWindowCropRule::Center, true, 0);
        check(!std::strcmp(text, "center: texture centre, no zoom"), "the center line");
        desktopWindowCropRuleText(text, sizeof text, DesktopWindowCropRule::Native, false, 0);
        const std::string none = text;
        check(none == "native: hidden-area mesh served no, as the game, no mesh", "the native line without a mesh");
        desktopWindowCropRuleText(text, sizeof text, DesktopWindowCropRule::Native, true, 0);
        const std::string left = text;
        desktopWindowCropRuleText(text, sizeof text, DesktopWindowCropRule::Native, true, 1);
        const std::string right = text;
        check(left == "native: hidden-area mesh served yes, as the game, mesh: 1.32x at u 0.515" && right == "native: hidden-area mesh served yes, as the game, mesh: 1.32x at u 0.485",
              "the native line with a mesh, for each eye");
        check(none != left && left != right, "control: the answer and the eye change the words");
    }
    // The numbers on the Pimax eye of the note: 4032x4034, a 16:9 window. Mode 7: u 0..1, v 0.5 +- 0.5/k. Mode 9: u 0.515 +- 0.5/1.32, v 0.5 +- 0.5/(1.32 k).
    {
        const float eyeBounds[4] = {0, 0, 1, 1};
        const double k = (16.0 / 9.0) / (4032.0 / 4034.0);
        auto cropWith = [&](DesktopWindowCropRule rule, bool mesh, unsigned eye, int shift, float* o, double target = 16.0 / 9.0) {
            const DwCropPlan p = desktopWindowCropPlan(rule, mesh, eye);
            desktopWindowCrop(eyeBounds, 4032, 4034, target, true, p.u, p.v, o, shift, p.zoom);
        };
        float centre[4], noMesh[4], meshL[4], meshR[4], cmesh[4];
        cropWith(DesktopWindowCropRule::Center, false, 0, 0, centre);
        cropWith(DesktopWindowCropRule::Center, true, 0, 0, cmesh);
        cropWith(DesktopWindowCropRule::Native, false, 0, 0, noMesh);
        cropWith(DesktopWindowCropRule::Native, true, 0, 0, meshL);
        cropWith(DesktopWindowCropRule::Native, true, 1, 0, meshR);
        check(closeTo(centre[0], 0.0, 1e-6) && closeTo(centre[2], 1.0, 1e-6) && closeTo(centre[1], 0.5 - 0.5 / k, 1e-5) && closeTo(centre[3], 0.5 + 0.5 / k, 1e-5) &&
                  closeTo(centre[1], 0.2189, 1e-4) && closeTo(centre[3], 0.7811, 1e-4),
              "center on a 4032x4034 eye, 16:9: all the width, v 0.5 +- 0.5/k (the note's mode 7: 0.2189..0.7811)");
        check(sameBits(centre, noMesh) && sameBits(centre, cmesh), "native with no mesh is exactly the center crop; center ignores the mesh");
        check(closeTo(meshL[0], 0.515 - 0.5 / 1.32, 1e-5) && closeTo(meshL[2], 0.515 + 0.5 / 1.32, 1e-5) && closeTo(meshL[1], 0.5 - 0.5 / (1.32 * k), 1e-5) &&
                  closeTo(meshL[3], 0.5 + 0.5 / (1.32 * k), 1e-5) && closeTo(meshL[0], 0.1362, 1e-4) && closeTo(meshL[2], 0.8938, 1e-4) && closeTo(meshL[1], 0.2870, 1e-4) &&
                  closeTo(meshL[3], 0.7130, 1e-4),
              "native with a mesh, left eye: u 0.515 +- 0.5/1.32, v 0.5 +- 0.5/(1.32 k) (the note's mode 9: u 0.1362..0.8938, v 0.2870..0.7130)");
        check(closeTo((meshL[2] - meshL[0]) * 4032.0, 3054.55, 0.01) && closeTo((meshL[3] - meshL[1]) * 4034.0, 1718.18, 0.01), "that is 3055x1718 pixels, the size the note gives");
        check(closeTo(meshR[0], 0.485 - 0.5 / 1.32, 1e-5) && closeTo(meshR[2], 0.485 + 0.5 / 1.32, 1e-5) && closeTo(meshR[1], meshL[1], 1e-7) && closeTo(meshR[3], meshL[3], 1e-7) &&
                  closeTo(meshR[0], 1.0 - meshL[2], 1e-5) && closeTo(meshR[2], 1.0 - meshL[0], 1e-5),
              "native with a mesh, right eye: the left eye's crop mirrored (u centre 0.485), the same rows");
        check(!sameBits(meshL, noMesh) && !sameBits(meshL, meshR) && (meshL[2] - meshL[0]) < 0.8 && (centre[2] - centre[0]) > 0.99 && meshL[1] > centre[1] && meshL[3] < centre[3],
              "control: the mesh crop is smaller than the center crop, inside it, and the eyes differ");
        // Both eyes: each eye is half the shape (8:9); the zoom divides both sides by 1.32 and the shape stays.
        float halfC[4], halfM[4];
        cropWith(DesktopWindowCropRule::Center, false, 0, 0, halfC, 16.0 / 9.0 * 0.5);
        cropWith(DesktopWindowCropRule::Native, true, 0, 0, halfM, 16.0 / 9.0 * 0.5);
        auto pixelShape = [](const float* c) { return ((c[2] - c[0]) * 4032.0) / ((c[3] - c[1]) * 4034.0); };
        check(closeTo(pixelShape(halfC), 8.0 / 9.0, 1e-4) && closeTo(pixelShape(halfM), 8.0 / 9.0, 1e-4) && closeTo((halfC[2] - halfC[0]) / (halfM[2] - halfM[0]), 1.32, 1e-4) &&
                  closeTo((halfC[3] - halfC[1]) / (halfM[3] - halfM[1]), 1.32, 1e-4),
              "both eyes (8:9 halves): the center and the mesh crops are both 8:9, the mesh one 1.32 times smaller each way");
        check(!closeTo(pixelShape(halfC), 16.0 / 9.0, 0.1), "control: a half is not the whole shape");
        // full: no crop and no zoom, under either rule, with any shift.
        float fullC[4], fullM[4];
        const DwCropPlan zoomed = desktopWindowCropPlan(DesktopWindowCropRule::Native, true, 0);
        desktopWindowCrop(eyeBounds, 4032, 4034, 0.0, true, zoomed.u, zoomed.v, fullM, 75, zoomed.zoom);
        desktopWindowCrop(eyeBounds, 4032, 4034, 0.0, true, 0.5, 0.5, fullC, -75, 1.0);
        check(sameBits(fullM, eyeBounds) && sameBits(fullC, eyeBounds), "full (target 0) is never cropped or zoomed, whatever the rule, the zoom or the shift");
        check(!sameBits(meshL, eyeBounds) && !sameBits(centre, eyeBounds), "control: a chosen shape does change the bounds");
        // The vertical shift on top of each rule, from that rule's default position (Pimax eye: center top row 883, mesh top row 1157.909).
        float up[4], down[4], half50[4];
        cropWith(DesktopWindowCropRule::Center, false, 0, 100, up);
        cropWith(DesktopWindowCropRule::Center, false, 0, -100, down);
        cropWith(DesktopWindowCropRule::Center, false, 0, 50, half50);
        check(closeTo(up[1], 0.0, 1e-6) && closeTo(up[3], 2268.0 / 4034.0, 1e-5) && closeTo(down[3], 1.0, 1e-6) && closeTo(down[1], 1.0 - 2268.0 / 4034.0, 1e-5) && closeTo(half50[1], 441.5 / 4034.0, 1e-5) &&
                  up[0] == centre[0] && up[2] == centre[2] && down[0] == centre[0] && down[2] == centre[2],
              "center + shift: +100 at the top edge, -100 at the bottom edge, +50 halfway from row 883, the columns unchanged");
        float mup[4], mdown[4], mhalf[4], mdhalf[4];
        cropWith(DesktopWindowCropRule::Native, true, 0, 100, mup);
        cropWith(DesktopWindowCropRule::Native, true, 0, -100, mdown);
        cropWith(DesktopWindowCropRule::Native, true, 0, 50, mhalf);
        cropWith(DesktopWindowCropRule::Native, true, 0, -50, mdhalf);
        check(closeTo(mup[1], 0.0, 1e-6) && closeTo(mup[3], 1718.18 / 4034.0, 1e-4) && closeTo(mdown[3], 1.0, 1e-6) && closeTo(mdown[1], 1.0 - 1718.18 / 4034.0, 1e-4) &&
                  closeTo(mhalf[1], 1157.909 / 2.0 / 4034.0, 1e-4) && closeTo(mdhalf[1], (1157.909 + (4034.0 - 1718.18 - 1157.909) / 2.0) / 4034.0, 1e-4),
              "native with a mesh + shift: +-100 reach the edges, +-50 are halfway from the zoomed default (row 1157.9)");
        check(mup[0] == meshL[0] && mup[2] == meshL[2] && mdown[0] == meshL[0] && mdown[2] == meshL[2] && mhalf[0] == meshL[0] && mdhalf[2] == meshL[2],
              "with the zoom there is room across too, but the shift stays vertical: the columns do not move");
        check(mdown[1] > down[1] && (mdown[3] - mdown[1]) < (down[3] - down[1]) && !sameBits(mhalf, half50),
              "control: the zoom changes the travel (the lowest mesh crop starts lower and is shorter) and the same shift lands in a different place");
        float rup[4];
        cropWith(DesktopWindowCropRule::Native, true, 1, 100, rup);
        check(closeTo(rup[1], mup[1], 1e-7) && closeTo(rup[3], mup[3], 1e-7) && closeTo(rup[0], meshR[0], 1e-7) && !closeTo(rup[0], mup[0], 1e-3), "the right eye takes the same shift: the same rows, its own columns");
        // A sub-rect of a texture: the centre is a fraction of the sub-rect, in the texture's UVs.
        const float leftHalf[4] = {0, 0, 0.5f, 1}, rightHalf[4] = {0.5f, 0, 1, 1};
        float sl[4], sr[4];
        const DwCropPlan pl = desktopWindowCropPlan(DesktopWindowCropRule::Native, true, 0), pr = desktopWindowCropPlan(DesktopWindowCropRule::Native, true, 1);
        desktopWindowCrop(leftHalf, 8064, 4034, 16.0 / 9.0, true, pl.u, pl.v, sl, 0, pl.zoom);
        desktopWindowCrop(rightHalf, 8064, 4034, 16.0 / 9.0, true, pr.u, pr.v, sr, 0, pr.zoom);
        check(closeTo(sl[0], 0.5 * meshL[0], 1e-5) && closeTo(sl[2], 0.5 * meshL[2], 1e-5) && closeTo(sl[1], meshL[1], 1e-6) && closeTo(sr[0], 0.5 + 0.5 * meshR[0], 1e-5) &&
                  closeTo(sr[2], 0.5 + 0.5 * meshR[2], 1e-5) && closeTo(sr[1], meshR[1], 1e-6),
              "the eyes' sub-rects of one wide texture: the same crop, in the texture's UVs");
        // A flipped sub-rect stays flipped, and with the zoom and any shift is the upright crop mirrored.
        const float flippedBounds[4] = {1, 1, 0, 0};
        bool mirroredToo = true;
        for (int shift = -100; shift <= 100; shift += 50) {
            float upright[4], flip[4];
            desktopWindowCrop(eyeBounds, 4032, 4034, 16.0 / 9.0, true, pl.u, pl.v, upright, shift, pl.zoom);
            desktopWindowCrop(flippedBounds, 4032, 4034, 16.0 / 9.0, true, pl.u, pl.v, flip, shift, pl.zoom);
            for (int i = 0; i < 4; ++i) mirroredToo = mirroredToo && closeTo(flip[i], 1.0 - upright[i], 1e-6);
        }
        check(mirroredToo, "a flipped sub-rect with the zoom: every shift from -100 to 100 is the upright crop mirrored");
    }

    // The layout: full keeps each eye's own size; a shape gives each eye its half of it.
    DwSize eyes[2] = {{1456, 1560}, {1456, 1560}}, shape[2];
    desktopWindowShapes(DesktopWindowAspect::Full, DesktopWindowMode::BothEyes, eyes, shape);
    check(shape[0].w == 1456 && shape[1].h == 1560, "full: each eye as it is");
    desktopWindowShapes(DesktopWindowAspect::SixteenNine, DesktopWindowMode::BothEyes, eyes, shape);
    double aspect = 0;
    check(closeTo(shape[0].w, 8.0 / 9.0) && shape[0].h == 1.0 && closeTo(shape[1].w, 8.0 / 9.0) && desktopWindowAspect(DesktopWindowMode::BothEyes, shape, &aspect) && closeTo(aspect, 16.0 / 9.0),
          "16:9 with both eyes: two 8:9 halves, one 16:9 picture");
    desktopWindowShapes(DesktopWindowAspect::SixteenNine, DesktopWindowMode::LeftEye, eyes, shape);
    check(closeTo(shape[0].w, 16.0 / 9.0) && desktopWindowAspect(DesktopWindowMode::LeftEye, shape, &aspect) && closeTo(aspect, 16.0 / 9.0), "16:9 with one eye: the whole picture");
    DwSize unequal[2] = {{1000, 1000}, {400, 900}};
    desktopWindowShapes(DesktopWindowAspect::FourThree, DesktopWindowMode::BothEyes, unequal, shape);
    DwPixelRect rects[2];
    unsigned idx[2];
    const unsigned n = desktopWindowEyeRects(DesktopWindowMode::BothEyes, DwPixelRect{0, 0, 1600, 900}, shape, rects, idx);
    check(desktopWindowAspect(DesktopWindowMode::BothEyes, shape, &aspect) && closeTo(aspect, 4.0 / 3.0) && n == 2 && rects[0].w == 800 && rects[1].w == 800 && rects[0].h == rects[1].h &&
              rects[0].x == 0 && rects[1].x == 800,
          "two eyes of different sizes still get exactly half the width each under a shape");
    DwPixelRect halves[2];
    desktopWindowShapes(DesktopWindowAspect::SixteenNine, DesktopWindowMode::BothEyes, eyes, shape);
    desktopWindowEyeRects(DesktopWindowMode::BothEyes, DwPixelRect{0, 0, 1600, 900}, shape, halves, idx);
    check(halves[0].w == 800 && halves[0].h == 900 && halves[1].x == 800 && halves[1].w == 800 && halves[1].h == 900, "16:9 on a 1600x900 picture: two 800x900 (8:9) halves");

    // A shape change alone keeps the client height and the top-left (round 6), then clamps.
    const DwArea work{0, 0, 1920, 1080};
    DwCorrection c = desktopWindowCorrect(16.0 / 9.0, 100, 100, 1000, 800, 1000, 1000, 16, 39, work, kDesktopWindowMinClientHeight, true);
    check(c.ok && c.keptHeight && c.clientHeight == 800 && c.clientWidth == 1422 && c.left == 100 && c.top == 100 && c.right == 1538 && c.bottom == 939,
          "a shape change alone: the client height 800 stays, the width follows (1422), the top-left stays");
    c = desktopWindowCorrect(16.0 / 9.0, 100, 100, 1008, 800, 0, 0, 16, 39, work, kDesktopWindowMinClientHeight, false);
    check(c.ok && !c.keptHeight && c.clientWidth == 1008 && c.clientHeight == 567, "control: without the shape-change flag the width is the one kept");
    c = desktopWindowCorrect(21.0 / 9.0, 1000, 100, 1000, 1000, 1000, 1000, 16, 39, work, kDesktopWindowMinClientHeight, true);
    check(c.ok && c.right <= 1920 && c.bottom <= 1080 && c.left >= 0 && c.top >= 0 && c.clientHeight <= 1041, "a wider shape that would stick out is clamped to the work area");
}

static void windowRules() {
    // The style while active.
    check(desktopWindowFramedStyle(0x94020000u) == 0x14CC0000u, "the flown popup style 0x94020000 becomes 0x14CC0000");
    check(desktopWindowFramedStyle(0x14CC0000u) == 0x14CC0000u, "our style is its own fixed point");
    check(desktopWindowFramedStyle(0x16CF0000u) == 0x16CC0000u, "Elite's own windowed style (overlapped, with both boxes) loses its minimize and maximize boxes");
    check(desktopWindowFramedStyle(0x17CF0000u) == 0x16CC0000u && desktopWindowFramedStyle(0x36CF0000u) == 0x16CC0000u, "the maximized and minimized state bits are dropped");
    check(desktopWindowFramedStyle(0x84020000u) == 0x04CC0000u, "a window that is not visible stays not visible (visible, clip siblings and clip children stay as they were)");
    check((desktopWindowFramedStyle(0x94020000u) & (kDwWsMinimizeBox | kDwWsMaximizeBox | kDwWsPopup)) == 0 &&
              (desktopWindowFramedStyle(0x94020000u) & (kDwWsCaption | kDwWsSysMenu | kDwWsThickFrame)) == (kDwWsCaption | kDwWsSysMenu | kDwWsThickFrame),
          "framed, with a system menu and a sizing frame, and no boxes");
    check(desktopWindowStyleIsOurs(0x14CC0000u, 0x14CC0000u) && desktopWindowStyleIsOurs(0x04CC0000u, 0x14CC0000u) && desktopWindowStyleIsOurs(0x15CC0000u, 0x14CC0000u),
          "a state or clip bit moving is still our style");
    check(!desktopWindowStyleIsOurs(0x94020000u, 0x14CC0000u) && !desktopWindowStyleIsOurs(0x16CF0000u, 0x14CC0000u) && !desktopWindowStyleIsOurs(0x14CD0000u, 0x14CC0000u),
          "the game's popup, its windowed style and a window with a maximize box are not ours");

    // The client's shape.
    const double a = 1456.0 / 1560.0;
    check(!desktopWindowAspectOff(972, 1041, a, 2), "a client of the picture's shape is not off");
    check(desktopWindowAspectOff(972, 1060, a, 2) && desktopWindowAspectOff(975, 1041, a, 2), "a client more than two pixels off on either axis is off");
    check(!desktopWindowAspectOff(972, 1043, a, 2), "two pixels off on both axes is not off");
    check(!desktopWindowAspectOff(1000, 501, 2.0, 2) && desktopWindowAspectOff(1000, 498, 2.0, 2), "a wide picture is held to two pixels on the width as well");
    check(!desktopWindowAspectOff(0, 0, a, 2) && !desktopWindowAspectOff(100, 100, 0.0, 2), "no client and no shape are never off");

    const DwArea work{0, 0, 1920, 1080};
    const int inside[4] = {100, 100, 500, 500}, left[4] = {-1, 0, 100, 100}, tall[4] = {0, 0, 1920, 1081};
    check(desktopWindowRectInside(inside, work) && !desktopWindowRectInside(left, work) && !desktopWindowRectInside(tall, work) && desktopWindowRectInside(inside, DwArea{100, 100, 500, 500}),
          "a window rectangle inside a work area, edges included");
    check(desktopWindowTooLarge(1921, 500, work) && desktopWindowTooLarge(500, 1081, work) && !desktopWindowTooLarge(1920, 1080, work), "a window larger than the work area");

    // The correction.
    DwCorrection c = desktopWindowCorrect(1.0, 100, 100, 1200, 1000, 1000, 1000, 16, 39, work);
    check(c.ok && !c.keptHeight && c.clientWidth == 1041 && c.clientHeight == 1041 && c.left == 100 && c.top == 0 && c.right == 1157 && c.bottom == 1080,
          "the user widened it: the width is kept, the height follows, then it is shrunk to the work area and slid up inside it");
    c = desktopWindowCorrect(1.0, 100, 100, 1000, 800, 1000, 1000, 16, 39, work);
    check(c.ok && c.keptHeight && c.clientWidth == 800 && c.clientHeight == 800 && c.left == 100 && c.top == 100 && c.right == 916 && c.bottom == 939,
          "the user made it shorter: the height is kept, the width follows, the top-left stays");
    c = desktopWindowCorrect(1.0, 100, 100, 1000, 800, 0, 0, 16, 39, work);
    check(c.ok && !c.keptHeight && c.clientWidth == 1000 && c.clientHeight == 1000 && c.top == 41 && c.bottom == 1080, "with no settled size the width is kept");
    c = desktopWindowCorrect(2.0, 0, 0, 300, 120, 600, 300, 16, 39, work);
    check(c.ok && c.keptHeight && c.clientHeight == 240 && c.clientWidth == 480 && c.right == 496 && c.bottom == 279, "a client shorter than 240 is made 240 high, its width from the shape");
    c = desktopWindowCorrect(1.0, 0, 0, 2000, 2000, 1000, 1000, 16, 39, DwArea{0, 0, 800, 600});
    check(c.ok && c.clientWidth == 561 && c.clientHeight == 561 && c.right == 577 && c.bottom == 600, "larger than the work area: shrunk to fit it, in the picture's shape");
    c = desktopWindowCorrect(1.0, -50, 10, 1000, 1000, 1000, 1000, 16, 39, work);
    check(c.ok && c.left == 0 && c.top == 10 && c.right == 1016 && c.bottom == 1049, "a window sticking out on the left is moved inside");
    c = desktopWindowCorrect(1.0, 1500, 900, 600, 600, 600, 600, 16, 39, work);
    check(c.ok && c.right == 1920 && c.left == 1920 - 616 && c.bottom == 1080 && c.top == 1080 - 639, "a window sticking out on the right and the bottom is moved inside");
    check(!desktopWindowCorrect(0.0, 0, 0, 100, 100, 0, 0, 0, 0, work).ok && !desktopWindowCorrect(1.0, 0, 0, 0, 100, 0, 0, 0, 0, work).ok, "no shape and no client correct nothing");
    c = desktopWindowCorrect(a, 466, 0, 972, 1041, 972, 1041, 16, 39, work);
    check(c.ok && c.clientWidth == 972 && c.clientHeight == 1041 && c.left == 466 && c.top == 0, "a window already right is left where it is");

    // The census of the skipped draws.
    check(DesktopWindowCensus::kindOf('D') == DesktopWindowCensus::Draw && DesktopWindowCensus::kindOf('I') == DesktopWindowCensus::Indexed &&
              DesktopWindowCensus::kindOf('N') == DesktopWindowCensus::Instanced && DesktopWindowCensus::kindOf('X') == DesktopWindowCensus::IndexedInstanced &&
              DesktopWindowCensus::kindOf('Y') == DesktopWindowCensus::Indirect && DesktopWindowCensus::kindOf('Z') == DesktopWindowCensus::Indirect &&
              DesktopWindowCensus::kindOf('A') == DesktopWindowCensus::Auto,
          "the hooks' kind letters");
    DesktopWindowCensus census;
    char text[800];
    census.format(text, sizeof text);
    check(!std::strcmp(text, "desktop window: skipped by shader (10 s): (none); other 0; pairs 0; with depth 0; kinds draw 0 indexed 0 instanced 0 indexed_instanced 0 indirect 0 auto 0; "
                             "render target 0 checked on 0, the buffer on 0"),
          "an empty window's line");
    for (int i = 0; i < 5; ++i) census.note(1, 2, 'I', true);
    for (int i = 0; i < 2; ++i) census.note(3, 4, 'D', false);
    for (int i = 0; i < 7; ++i) census.note(1, 9, 'X', false);
    census.note(5, 6, 'A', true);
    census.sample(true);
    census.sample(false);
    census.format(text, sizeof text);
    check(std::strstr(text, "skipped by shader (10 s): 0000000000000001/0000000000000009 x7, 0000000000000001/0000000000000002 x5, 0000000000000003/0000000000000004 x2, "
                            "0000000000000005/0000000000000006 x1; other 0; pairs 4; with depth 6; kinds draw 2 indexed 5 instanced 0 indexed_instanced 7 indirect 0 auto 1; "
                            "render target 0 checked on 2, the buffer on 1") != nullptr,
          "pairs are listed biggest first, with their counts, the depth count, the kinds and the context checks");
    check(census.total() == 15 && census.pairs() == 4 && census.unplaced() == 0 && census.withDepth() == 6, "the totals");
    census.reset();
    check(census.total() == 0 && census.pairs() == 0 && census.sampled() == 0, "a reset empties the table");
    for (uint64_t i = 0; i < 20; ++i) census.note(100 + i, 7, 'D', false);   // twenty pairs, one draw each: the table holds sixteen
    census.note(100, 7, 'D', false);                                          // the first again: it is in the table
    census.format(text, sizeof text);
    check(census.pairs() == 16 && census.unplaced() == 4 && census.total() == 21 && std::strstr(text, "pairs 16+;") && std::strstr(text, "0000000000000064/0000000000000007 x2") &&
              std::strstr(text, "other 14;"),
          "a full table counts the pairs it could not place, says so, and keeps them in other");
    census.reset();
    census.note(1, 1, 'D', false);
    census.note(2, 2, 'D', false);
    census.note(3, 3, 'D', false);
    census.format(text, sizeof text);
    check(std::strstr(text, "0000000000000001/0000000000000001 x1, 0000000000000002/0000000000000002 x1, 0000000000000003/0000000000000003 x1;") != nullptr, "equal counts keep their first-seen order");
    census.reset();
    for (uint64_t i = 0; i < 9; ++i) for (uint64_t j = 0; j <= i; ++j) census.note(i, 0, 'D', false);   // nine pairs, counts 1 to 9
    census.format(text, sizeof text);
    check(std::strstr(text, "0000000000000008/0000000000000000 x9, 0000000000000007/0000000000000000 x8, 0000000000000006/0000000000000000 x7, 0000000000000005/0000000000000000 x6, "
                            "0000000000000004/0000000000000000 x5, 0000000000000003/0000000000000000 x4; other 6;") != nullptr && std::strstr(text, "pairs 9;") != nullptr,
          "only the top six are named, and the rest are other");
}

static void cursorRules() {
    check(desktopWindowAtCentre(960, 540, 960, 540) && desktopWindowAtCentre(961, 539, 960, 540) && !desktopWindowAtCentre(963, 540, 960, 540) && !desktopWindowAtCentre(960, 543, 960, 540),
          "a SetCursorPos is at the client centre within two pixels");
    check(desktopWindowAtCentre(-1919, 1, -1920, 0), "screen coordinates can be negative");
    char words[160];
    desktopWindowCooperativeWords(words, sizeof words, 0x6u);
    check(!std::strcmp(words, "nonexclusive, foreground"), "DISCL_NONEXCLUSIVE | DISCL_FOREGROUND");
    desktopWindowCooperativeWords(words, sizeof words, 0x5u);
    check(!std::strcmp(words, "exclusive, foreground"), "DISCL_EXCLUSIVE | DISCL_FOREGROUND");
    desktopWindowCooperativeWords(words, sizeof words, 0x9u);
    check(!std::strcmp(words, "exclusive, background"), "DISCL_EXCLUSIVE | DISCL_BACKGROUND");
    desktopWindowCooperativeWords(words, sizeof words, 0x15u);
    check(!std::strcmp(words, "exclusive, foreground, no windows key"), "DISCL_NOWINKEY is said");
    desktopWindowCooperativeWords(words, sizeof words, 0u);
    check(!std::strcmp(words, "neither exclusive nor nonexclusive, neither foreground nor background"), "no flags");
    char line[500];
    desktopWindowCursorLine(line, sizeof line, 580, 575, 590, 2, "hide, display count -1", 54, 60, -2, 7);
    check(!std::strcmp(line, "desktop window: cursor (10 s): setpos 580 (at the client centre 575), getpos 590, showcursor 2 (last hide, display count -1), "
                             "hidden on 54 of 60 Presents, offset now -2, pushed down 7"),
          "the cursor line: no clip counts, the hide at its end");
}

// A fake ShowCursor: one count, one thread's.
struct FakeShow {
    int real = 0;
    int calls = 0;
    bool stuck = false;   // a ShowCursor that never goes down
    int operator()(bool visible) {
        ++calls;
        if (!stuck) real += visible ? 1 : -1;
        return real;
    }
};

static void hideRules() {
    // The rule.
    check(desktopWindowHideCursor(true, true, true), "active, foreground and inside the client: hidden");
    check(!desktopWindowHideCursor(true, false, true) && !desktopWindowHideCursor(true, true, false) && !desktopWindowHideCursor(false, true, true) && !desktopWindowHideCursor(false, false, false),
          "not foreground, not inside the client (title bar, frame, another window) or not active: visible");
    check(desktopWindowPointInRect(10, 20, 10, 20, 110, 220) && desktopWindowPointInRect(109, 219, 10, 20, 110, 220) && !desktopWindowPointInRect(110, 100, 10, 20, 110, 220) &&
              !desktopWindowPointInRect(50, 220, 10, 20, 110, 220) && !desktopWindowPointInRect(9, 100, 10, 20, 110, 220) && !desktopWindowPointInRect(50, 19, 10, 20, 110, 220),
          "inside the client is the left and top edges in, the right and bottom out");
    check(desktopWindowPointInRect(-1919, 5, -1920, 0, -1000, 100), "screen coordinates can be negative");

    // The protocol, with the game at count 0 (the cursor visible, as the flight saw it: the game's hide returned 0).
    FakeShow fake;
    DesktopWindowShowCount count;
    count.hide(fake);
    check(fake.real == -1 && count.offset() == -1 && fake.calls == 1, "a Present with hiding wanted and the count not known: one hide, and the real count is below 0");
    count.hide(fake);
    check(fake.calls == 1 && count.offset() == -1, "the count is known to be below 0: nothing more is called");
    int seen = count.afterGameCall(fake(true), true, fake);   // the game calls ShowCursor(TRUE): the real count goes to 0
    check(seen == 1 && fake.real == -1 && count.offset() == -2 && count.takePushed() == 1, "a game show: it sees 1, as it would without EDVR, and the real count is pushed below 0 again");
    seen = count.afterGameCall(fake(false), true, fake);      // the game hides: the real count goes to -2
    check(seen == 0 && fake.real == -2 && count.offset() == -2 && count.takePushed() == 0, "a game hide: it sees 0, and nothing is pushed");
    bool flashed = false;
    for (int i = 0; i < 100; ++i) {   // the game's 50 to 200 calls a second, in pairs
        const int a = count.afterGameCall(fake(true), true, fake);
        flashed = flashed || fake.real >= 0;
        const int b = count.afterGameCall(fake(false), true, fake);
        flashed = flashed || fake.real >= 0;
        if (a != 1 || b != 0) flashed = true;
        if (count.offset() > 0) flashed = true;
    }
    check(!flashed, "a hundred game show and hide pairs: the real count is never 0 or above after a call, the game always sees 1 and 0, the offset is never above 0");
    count.unhide(fake);
    check(count.offset() == 0 && fake.real == 0, "the hides are given back: the real count is the game's, the offset 0");
    check(count.afterGameCall(fake(true), false, fake) == 1 && fake.real == 1, "not wanted: the game's count is the real one, untouched");
    count.unhide(fake);
    check(fake.real == 1, "an unhide at offset 0 calls nothing");

    // The bounds: a count that never goes down is tried 16 times and no more; a deep offset is given back.
    FakeShow stuck;
    stuck.stuck = true;
    stuck.real = 3;
    DesktopWindowShowCount bounded;
    bounded.hide(stuck);
    check(stuck.calls == DesktopWindowShowCount::kPushBound && bounded.offset() == -DesktopWindowShowCount::kPushBound, "a hide that does not take is tried 16 times, no more");
    FakeShow deep;
    DesktopWindowShowCount many;
    many.noteReal(deep(false));   // hiding wanted, the count below 0
    for (int i = 0; i < 40; ++i) many.afterGameCall(deep(true), true, deep);   // 40 game shows in a row, each pushed down
    const int before = many.offset();
    many.unhide(deep);
    check(before < -16 && many.offset() == 0, "an offset deeper than 16 is still given back in one go");
    DesktopWindowShowCount fresh;
    FakeShow high;
    high.real = 5;
    fresh.hide(high);
    check(high.real == -1 && fresh.offset() == -6, "a count above 0 is brought below 0 (the cursor was shown several times over)");
}

static void lines() {
    char line[400];
    desktopWindowSettingLine(line, sizeof line, "left_eye", nullptr);
    check(!std::strcmp(line, "desktop window: setting left_eye"), "the setting line");
    desktopWindowSettingLine(line, sizeof line, "both_eyes", "game");
    check(!std::strcmp(line, "desktop window: setting both_eyes (was game)"), "the setting line, changed");
    desktopWindowStatsLine(line, sizeof line, 600, 610, 1220, 610, 610, 3, 972, 1041, 1920, 1080, 4, 2);
    check(!std::strcmp(line, "desktop window: 10 s: drawn=600 seen=610 skipped_draws=1220 eyes_L=610 eyes_R=610 stale=3 client=972x1041 swap_chain=1920x1080 moves=4 resizes=2"),
          "the 10 s line, with the user's moves and resizes at the end");
    desktopWindowStoppedLine(line, sizeof line, "no eye image for 500 ms (3D is off, or no frames arrive)", 600, 610, "restored to 100,100 1280x720");
    check(!std::strcmp(line, "desktop window: stopped (no eye image for 500 ms (3D is off, or no frames arrive)); drawn=600 of 610 Presents; window restored to 100,100 1280x720"),
          "the stopped line");
}

// The mesh flag through the handoff: the runtime's client against a provider of this rig's own (the same export, the same table), so the negotiation and the call are the
// real ones. The graphics half's own functions are pinned by source (wiring).
namespace fakeprovider {
struct Seen {
    unsigned acquires = 0, calls = 0;
    uint32_t asked[4] = {};
    uint32_t eye = 99, mesh = 77;
    int via = 0;
    float bounds[4] = {};
    int closes = 0;
};
Seen seen;
bool have3 = true, haveAny = true;
int token = 0;
HRESULT WINAPI note1(void* context, uint32_t eye, ID3D11Texture2D*, const float* b) {
    if (context != &token) return E_INVALIDARG;
    ++seen.calls; seen.via = 1; seen.eye = eye; seen.mesh = 77;
    std::memcpy(seen.bounds, b, sizeof seen.bounds);
    return S_OK;
}
HRESULT WINAPI note3(void* context, uint32_t eye, ID3D11Texture2D*, const float* b, uint32_t mesh) {
    if (context != &token) return E_INVALIDARG;
    ++seen.calls; seen.via = 3; seen.eye = eye; seen.mesh = mesh;
    std::memcpy(seen.bounds, b, sizeof seen.bounds);
    return S_OK;
}
HRESULT WINAPI close(void* context) { ++seen.closes; return context == &token ? S_OK : E_INVALIDARG; }
HRESULT WINAPI acquire(const EdvrNativeDesktopWindowRequest* request, EdvrNativeDesktopWindowTable* table) {
    if (!table || table->size != sizeof(*table)) return E_INVALIDARG;
    if (seen.acquires < 4) seen.asked[seen.acquires] = table->version;
    ++seen.acquires;
    const uint32_t version = table->version;
    if (!haveAny || (version != EDVR_NATIVE_DESKTOP_WINDOW_VERSION_1 && !(version == EDVR_NATIVE_DESKTOP_WINDOW_VERSION_3 && have3))) return E_INVALIDARG;
    if (!request || request->version != version) return E_INVALIDARG;
    *table = {sizeof(*table), version};
    table->context = &token;
    table->noteEye = version == EDVR_NATIVE_DESKTOP_WINDOW_VERSION_3 ? reinterpret_cast<EdvrNativeDesktopWindowNoteEye>(&note3) : &note1;
    table->close = &close;
    return S_OK;
}
}  // namespace fakeprovider

static void handoff() {
    using fakeprovider::seen;
    HMODULE self = GetModuleHandleW(nullptr);
    ID3D11Device* device = reinterpret_cast<ID3D11Device*>(static_cast<uintptr_t>(0x1000));        // never dereferenced
    ID3D11Texture2D* texture = reinterpret_cast<ID3D11Texture2D*>(static_cast<uintptr_t>(0x2000));
    const vr::VRTextureBounds_t bounds{0.125f, 0.25f, 0.875f, 0.75f};
    auto sameBox = [&](const float* b) { return b[0] == 0.125f && b[1] == 0.25f && b[2] == 0.875f && b[3] == 0.75f; };
    const void* entry = reinterpret_cast<const void*>(&fakeprovider::acquire);

    seen = {};
    fakeprovider::have3 = fakeprovider::haveAny = true;
    edvr::openxr::NativeDesktopWindowClient client;
    check(client.acquireFrom(self, entry, device, 1) == S_OK && client.version() == 3 && seen.acquires == 1 && seen.asked[0] == 3, "the client asks for version 3 first, and gets it");
    client.noteEye(1, texture, bounds, true);
    check(seen.calls == 1 && seen.via == 3 && seen.eye == 1 && seen.mesh == 1 && sameBox(seen.bounds), "version 3: a served mesh reaches the provider as 1, with the eye and the bounds");
    client.noteEye(0, texture, bounds, false);
    check(seen.calls == 2 && seen.via == 3 && seen.eye == 0 && seen.mesh == 0 && sameBox(seen.bounds), "version 3: no mesh reaches the provider as 0");
    client.noteEye(0, texture, bounds);
    check(seen.calls == 3 && seen.mesh == 0, "version 3: no answer given is no mesh");
    client.noteEye(0, nullptr, bounds, true);
    check(seen.calls == 3, "control: no texture is no call");
    check(client.close() == S_OK && !client.acquired() && seen.closes == 1, "the client closes the provider and lets go of the table");
    client.noteEye(0, texture, bounds, true);
    check(seen.calls == 3, "control: a closed client hands nothing over");

    // A graphics half that has no version 3: the client falls back to 1 and the flag is not carried (the provider takes it as no mesh).
    seen = {};
    fakeprovider::have3 = false;
    edvr::openxr::NativeDesktopWindowClient oldHalf;
    check(oldHalf.acquireFrom(self, entry, device, 1) == S_OK && oldHalf.version() == 1 && seen.acquires == 2 && seen.asked[0] == 3 && seen.asked[1] == 1, "without version 3 the client falls back to version 1 (asks 3, then 1)");
    oldHalf.noteEye(1, texture, bounds, true);
    check(seen.calls == 1 && seen.via == 1 && seen.mesh == 77 && sameBox(seen.bounds), "version 1: the eye and bounds arrive, the flag is not carried");

    // A provider that answers nothing: the error comes back and nothing is held.
    seen = {};
    fakeprovider::haveAny = false;
    edvr::openxr::NativeDesktopWindowClient none;
    check(FAILED(none.acquireFrom(self, entry, device, 1)) && !none.acquired() && none.version() == 0 && seen.acquires == 2, "a provider that answers neither version leaves the client without one");
    none.noteEye(0, texture, bounds, true);
    check(seen.calls == 0, "control: an unacquired client hands nothing over");

    // Version 2 (round 6) is not asked for, and a provider that only knows it would be refused: the client never offers 2.
    check(seen.asked[0] == 3 && seen.asked[1] == 1 && seen.asked[0] != 2 && seen.asked[1] != 2, "the client never asks for version 2");
    seen = {};
    fakeprovider::have3 = fakeprovider::haveAny = true;
    edvr::openxr::NativeDesktopWindowClient again;
    check(again.acquireFrom(nullptr, entry, device, 1) == E_INVALIDARG && again.acquireFrom(self, nullptr, device, 1) == E_NOINTERFACE && again.acquireFrom(self, entry, nullptr, 1) == E_INVALIDARG &&
              again.acquireFrom(self, entry, device, 0) == E_INVALIDARG && seen.acquires == 0,
          "no provider, no export, no device or no generation is refused before the export is called");
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

    // The key: one word, default left_eye (F173; game before), read live under the configuration generation; the flat profile never reads it and does not allow it.
    auto keyRead = [](const std::string& t) {
        return count(t, "cfg.getString(\"visual.desktop_window\", \"left_eye\")") == 1 && at(t, "cfg.generation()") != std::string::npos && at(t, "desktopWindowParse(raw.c_str(), &known)") != std::string::npos;
    };
    check(keyRead(cpp), "desktop_window.cpp reads visual.desktop_window as one word, default left_eye, under the configuration generation");
    check(!keyRead(replaced(cpp, "\"visual.desktop_window\", \"left_eye\"", "\"visual.desktop_window\", \"game\"")), "control: a key that defaults to game (the default before F173) trips the pin");
    check(at(flatKeys, "desktop_window") == std::string::npos, "the flat profile's allow-list does not carry visual.desktop_window");
    check(at(cpp, "if (flat) {") != std::string::npos && at(cpp, "S.mode = DesktopWindowMode::Game;", at(cpp, "if (flat) {")) != std::string::npos, "the flat profile is game before any read");

    // The window's style and rectangle are changed from the pool callback and nowhere else (style first, then the rectangle with SWP_FRAMECHANGED); the hook only reads.
    auto movedOnlyByPool = [](const std::string& t) {
        const size_t set = at(t, "SetWindowPos(");
        const size_t setStyle = at(t, "SetWindowLongPtrW(r->hwnd, GWL_STYLE, r->style);");
        const size_t cb = at(t, "VOID CALLBACK moveWindowCallback");
        return count(t, "SetWindowPos(") == 1 && count(t, "SetWindowLongPtrW(") == 1 && cb != std::string::npos && set > setStyle && setStyle > cb &&
               at(t, "TrySubmitThreadpoolCallback(moveWindowCallback") != std::string::npos &&
               at(t, "UINT flags = SWP_ASYNCWINDOWPOS | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER;") != std::string::npos &&
               at(t, "flags |= SWP_FRAMECHANGED;") != std::string::npos && at(t, "MoveWindow(") == std::string::npos && at(t, "ShowWindow(") == std::string::npos;
    };
    check(movedOnlyByPool(cpp), "the window's style and rectangle are changed in the thread-pool callback only, asynchronously, style first, and by nothing else");
    check(!movedOnlyByPool(cpp + "\nvoid f(HWND h){ SetWindowPos(h, nullptr, 0, 0, 1, 1, 0); }\n"), "control: a SetWindowPos from the hook trips the pin");
    check(!movedOnlyByPool(cpp + "\nvoid f(HWND h){ SetWindowLongPtrW(h, GWL_STYLE, 0); }\n"), "control: a SetWindowLongPtrW from the hook trips the pin");
    auto windowRulesWired = [](const std::string& t) {
        return at(t, "desktopWindowStyleFor(S.activeStyle, static_cast<uint32_t>(style))") != std::string::npos && at(t, "if (windowInMoveSize(hwnd)) return;") != std::string::npos &&
               at(t, "(info.flags & GUI_INMOVESIZE) != 0") != std::string::npos && at(t, "GetGUIThreadInfo(thread, &info)") != std::string::npos &&
               at(t, "\"AdjustWindowRectExForDpi\"") != std::string::npos && at(t, "\"GetDpiForWindow\"") != std::string::npos && at(t, "AdjustWindowRectEx(&probe") != std::string::npos &&
               at(t, "MonitorFromRect(&r, MONITOR_DEFAULTTONEAREST)") != std::string::npos && at(t, "desktopWindowPlan(aspect, work.left, work.top, work.right, work.bottom, frameDx, frameDy)") != std::string::npos &&
               at(t, "desktopWindowCorrect(aspect, cur[0], cur[1], clientW, clientH") != std::string::npos && at(t, "S.styleSeenOurs && !ours") != std::string::npos &&
               at(t, "++S.userResizes") != std::string::npos && at(t, "++S.userMoves") != std::string::npos && at(t, "kUnsettledAllowed = 20") != std::string::npos &&
               at(t, "kFightsAllowed = 10") != std::string::npos;
    };
    check(windowRulesWired(cpp), "the window rules are wired: our style, the move-size loop, the frame from the system then from the window, the settled start, the correction, fights, user moves and resizes");
    check(!windowRulesWired(replaced(cpp, "if (windowInMoveSize(hwnd)) return;", "")), "control: no wait for the user's move or size loop trips the pin");
    check(at(cpp, "(style & WS_CAPTION)") == std::string::npos, "the bound no longer depends on the game's style: the work area, always");

    // The Present hook: before the census span and before the real Present, inside hookedPresent.
    auto presentOrder = [](const std::string& t) {
        const size_t fn = at(t, "HRESULT STDMETHODCALLTYPE hookedPresent(");
        const size_t call = at(t, "desktopWindowPresent(self, dev, flags, runtimeFlatProfile())", fn);
        const size_t span = at(t, "gpuCensusFrameWorkEnd(workContext)", fn);
        const size_t real = at(t, "g_state->realPresent(self, presentSync, flags);", span);   // Elite's own; the other swap chains' early return forwards syncInterval (desktop_vsync.h)
        return fn != std::string::npos && call != std::string::npos && span != std::string::npos && real != std::string::npos && call < span && span < real &&
               at(t, "frameTick(\"desktop_window\")", fn) != std::string::npos;
    };
    check(presentOrder(hook), "hookedPresent calls the feature, and ticks it, before the census span and the real Present");
    check(!presentOrder(replaced(hook, "desktopWindowPresent(self, dev, flags, runtimeFlatProfile())", "false")), "control: no call in the hook trips the pin");

    // The draw skip: the verdict (four thunks) and the three that do not ask it; each skip is after the foreign-context decline in the verdict.
    auto skips = [](const std::string& t) {
        const size_t verdict = at(t, "DrawVerdict beginPanelOverride(");
        const size_t station = at(t, "optimizedStationSkipsDraw(bindingShaderHash(BindSlot::Vs)", verdict);
        const size_t mine = at(t, "if (desktopWindowSkipsDraw(s, self, kind)) return DrawVerdict::kSkip;", verdict);
        return verdict != std::string::npos && station != std::string::npos && mine != std::string::npos && station < mine && count(t, "desktopWindowSkipsDraw(g_state, self, '") == 3 &&
               at(t, "void* back = desktopWindowBackIdentity();") != std::string::npos && at(t, "rt->resource != back") != std::string::npos &&
               at(t, "desktopWindowNoteSkippedDraw(self, kind, bindingShaderHash(BindSlot::Vs), bindingShaderHash(BindSlot::Ps), bindingGet(BindSlot::Dsv0) != nullptr);") != std::string::npos;
    };
    check(skips(vscreen), "the verdict and the DrawAuto and indirect thunks skip a draw whose render target 0 is the swap chain's buffer");
    check(!skips(replaced(vscreen, "if (desktopWindowSkipsDraw(s, self, kind)) return DrawVerdict::kSkip;", "")), "control: no skip in the verdict trips the pin");
    check(!skips(replaced(vscreen, "if (self == g_state->ownerCtx && desktopWindowSkipsDraw(g_state, self, 'A')) return;   // visual.desktop_window", "")), "control: a thunk without the skip trips the pin");
    check(!skips(replaced(vscreen, "desktopWindowNoteSkippedDraw(self, kind, bindingShaderHash(BindSlot::Vs), bindingShaderHash(BindSlot::Ps), bindingGet(BindSlot::Dsv0) != nullptr);",
                          "desktopWindowNoteSkippedDraw(self, kind, 0, 0, false);")),
          "control: a census that does not read the shaders and the depth view trips the pin");
    {   // the census line follows the 10 s line and starts the table again, in the Present hook's code
        const size_t fn = at(cpp, "bool desktopWindowPresent(");
        const size_t stats = at(cpp, "desktopWindowStatsLine(line", fn), format = at(cpp, "g_census.format(census, sizeof census);", fn), reset = at(cpp, "g_census.reset();", format);
        check(fn != std::string::npos && stats != std::string::npos && format != std::string::npos && reset != std::string::npos && stats < format && format < reset,
              "the census line is written after the 10 s line and the table is reset after it");
    }
    check(at(cpp, "VrWorldInternalScope internal;") != std::string::npos && at(cpp, "saved.restore(ctx);") != std::string::npos, "the draw runs under the internal scope and puts the context's state back");
    check(at(cpp, "back.Get(), std::memory_order_relaxed);") != std::string::npos && at(cpp, "ComPtr<ID3D11Texture2D> back;") != std::string::npos, "the buffer is a local reference, its identity a raw pointer");

    // The host hands the eye over after it is captured, and the provider is optional.
    auto handsOver = [](const std::string& t) {
        const size_t cap = at(t, "captured.capture(eye,selected,region,flags,true)");
        const size_t note = at(t, "desktopWindow.noteEye(unsigned(eye),treated.Get(),treatedBounds,(hiddenMeshServedEyes.load(std::memory_order_acquire)&3u)!=0);", cap);
        const size_t end = at(t, "return r;", note);
        return cap != std::string::npos && note != std::string::npos && end != std::string::npos && end - note < 300 &&
               at(t, "native_desktop_window,unavailable=") != std::string::npos && at(t, "desktopWindow.acquire(options.graphicsProvider,externalDevice,runtimeGeneration)") != std::string::npos &&
               at(t, "desktopWindow.close();") != std::string::npos;
    };
    check(handsOver(host), "the host hands each captured eye's finished texture over, acquires the provider as optional and closes it");
    check(!handsOver(replaced(host, "desktopWindow.noteEye(unsigned(eye),treated.Get(),treatedBounds,(hiddenMeshServedEyes.load(std::memory_order_acquire)&3u)!=0);", "")), "control: no handoff trips the pin");
    // Round 8: the export is version 3 (the runtime's hidden-area-mesh answer rides with each eye; version 2, round 6's field of view, is gone), the runtime asks for 3 then 1, the
    // host sends whether either eye was served a mesh, the OpenVR system says so for every answer that carried one, and the graphics half keeps it and crops with it.
    const std::string exportHeader = readFile(root + "\\src\\common\\native_desktop_window.h");
    const std::string client = readFile(root + "\\src\\openxr\\native_desktop_window_client.h");
    const std::string vrSystem = readFile(root + "\\src\\openxr\\openvr_system.cpp");
    const std::string systemSource = readFile(root + "\\src\\openxr\\system_source.h");
    check(!exportHeader.empty() && !client.empty() && !vrSystem.empty() && !systemSource.empty(), "the files the mesh pins read are where they look");
    auto meshWired = [&](const std::string& h, const std::string& c, const std::string& t, const std::string& w, const std::string& sys, const std::string& src) {
        return at(h, "#define EDVR_NATIVE_DESKTOP_WINDOW_VERSION_3 3u") != std::string::npos && at(h, "VERSION_2") == std::string::npos &&
               at(h, "typedef HRESULT(WINAPI *EdvrNativeDesktopWindowNoteEye3)(void*, uint32_t, ID3D11Texture2D*, const float*, uint32_t);") != std::string::npos &&
               at(c, "{EDVR_NATIVE_DESKTOP_WINDOW_VERSION_3, EDVR_NATIVE_DESKTOP_WINDOW_VERSION_1}") != std::string::npos &&
               at(c, "EdvrNativeDesktopWindowNoteEye3>(table_.noteEye)(table_.context, eye, texture, box, hiddenMeshServed ? 1u : 0u)") != std::string::npos &&
               at(c, "VERSION_2") == std::string::npos && at(c, "angles") == std::string::npos &&
               at(w, "hiddenMeshServedEyes.fetch_or(1u<<eye,std::memory_order_release);") != std::string::npos && at(w, "void noteHiddenMeshServed(unsigned eye) noexcept override {") != std::string::npos &&
               at(w, "contentAngles") == std::string::npos &&
               at(t, "table->version != EDVR_NATIVE_DESKTOP_WINDOW_VERSION_1 && table->version != EDVR_NATIVE_DESKTOP_WINDOW_VERSION_3") != std::string::npos &&
               at(t, "reinterpret_cast<EdvrNativeDesktopWindowNoteEye>(&edvr::noteEye3)") != std::string::npos &&
               at(t, "return noteEyeWith(context, eye, texture, bounds, hiddenMeshServed != 0);") != std::string::npos && at(t, "h.meshServed = meshServed;") != std::string::npos &&
               at(t, "s.meshServed[eye] = g_held[eye].meshServed;") != std::string::npos && at(t, "VERSION_2") == std::string::npos && at(t, "haveFov") == std::string::npos &&
               at(t, "draws[i].bounds = cropBounds[eyeIndex[i]];") != std::string::npos &&
               at(t, "desktopWindowShapes(S.aspectOpt, S.mode, eyeSize, shape);") != std::string::npos && at(t, "desktopWindowEyeRects(S.mode, shownPx, shape, rects, eyeIndex)") != std::string::npos &&
               at(sys, "if(result.pVertexData&&result.unTriangleCount)source_.noteHiddenMeshServed(e);") != std::string::npos &&
               at(src, "virtual void noteHiddenMeshServed(unsigned) noexcept {}") != std::string::npos;
    };
    check(meshWired(exportHeader, client, cpp, host, vrSystem, systemSource),
          "the export is version 3 with the mesh flag; the runtime asks for 3 then 1; the host sends whether either eye was served a mesh; the system reports every served answer; the graphics half keeps and uses it");
    check(!meshWired(replaced(exportHeader, "#define EDVR_NATIVE_DESKTOP_WINDOW_VERSION_3 3u", ""), client, cpp, host, vrSystem, systemSource), "control: no version 3 trips the pin");
    check(!meshWired(exportHeader, client, cpp, host, replaced(vrSystem, "if(result.pVertexData&&result.unTriangleCount)source_.noteHiddenMeshServed(e);", ""), systemSource),
          "control: a system that never says a mesh was served trips the pin");
    check(!meshWired(exportHeader, client, cpp, replaced(host, "hiddenMeshServedEyes.fetch_or(1u<<eye,std::memory_order_release);", ""), vrSystem, systemSource), "control: a host that never records it trips the pin");
    check(!meshWired(exportHeader, client, replaced(cpp, "draws[i].bounds = cropBounds[eyeIndex[i]];", "draws[i].bounds = snap.bounds[eyeIndex[i]];"), host, vrSystem, systemSource),
          "control: a draw that ignores the crop trips the pin");
    check(!meshWired(exportHeader, client, replaced(cpp, "s.meshServed[eye] = g_held[eye].meshServed;", ""), host, vrSystem, systemSource), "control: a snapshot that drops the flag trips the pin");
    check(!meshWired(exportHeader, replaced(client, "{EDVR_NATIVE_DESKTOP_WINDOW_VERSION_3, EDVR_NATIVE_DESKTOP_WINDOW_VERSION_1}", "{EDVR_NATIVE_DESKTOP_WINDOW_VERSION_1}"), cpp, host, vrSystem, systemSource),
          "control: a client that never asks for version 3 trips the pin");
    check(!meshWired(exportHeader + "\n#define EDVR_NATIVE_DESKTOP_WINDOW_VERSION_2 2u\n", client, cpp, host, vrSystem, systemSource), "control: a version 2 that is back trips the pin");
    // The crop rule: one word, native by default, read live with the others; the flat profile resets it; the plan decides the centre and the zoom for each eye; `full` (target 0)
    // is not cropped; the crop line names the rule, the shift and the bounds, and the change is one line.
    auto cropWired = [](const std::string& t) {
        const size_t flat = at(t, "if (flat) {");
        const size_t flatCrop = flat == std::string::npos ? flat : at(t, "S.cropOpt = DesktopWindowCropRule::Native;", flat);
        const size_t read = at(t, "cfg.getString(\"visual.desktop_window_crop\", \"native\")");
        return count(t, "cfg.getString(\"visual.desktop_window_crop\"") == 1 && read != std::string::npos && flatCrop != std::string::npos && flatCrop < read &&
               at(t, "desktopWindowParseCropRule(rawCrop.c_str(), &cropKnown)") != std::string::npos && at(t, "S.cropOpt = cropOpt;") != std::string::npos &&
               at(t, "desktop window: crop rule %s (was %s)") != std::string::npos && at(t, "desktop window: crop rule %s\"") != std::string::npos &&
               at(t, "unknown crop \\\"%.40s\\\" (it is native or center); treated as native") != std::string::npos &&
               at(t, "plans[eye] = desktopWindowCropPlan(S.cropOpt, snap.meshServed[eye], eye);") != std::string::npos &&
               at(t, "ratio > 0 ? (S.mode == DesktopWindowMode::BothEyes ? ratio * 0.5 : ratio) : 0.0, true, plans[eye].u, plans[eye].v,") != std::string::npos &&
               at(t, "cropBounds[eye], S.shiftOpt, plans[eye].zoom);") != std::string::npos &&
               at(t, "desktopWindowCropRuleText(ruleText, sizeof ruleText, S.cropOpt, snap.meshServed[eye], eye);") != std::string::npos &&
               at(t, "desktop window: crop %s, %s eye, rule %s, vertical shift %d; crop bounds") != std::string::npos &&
               at(t, "desktopWindowProjectionCentre") == std::string::npos && at(t, "straight ahead") == std::string::npos;
    };
    check(cropWired(cpp), "the crop rule is read as one word (native by default), reset by the flat profile, turned into a centre and zoom for each eye, never applied to full, and logged (change, crop line)");
    check(!cropWired(replaced(cpp, "\"visual.desktop_window_crop\", \"native\"", "\"visual.desktop_window_crop\", \"center\"")), "control: a rule that defaults to center trips the pin");
    check(!cropWired(replaced(cpp, "S.cropOpt = DesktopWindowCropRule::Native;", "")), "control: a flat profile that does not reset the rule trips the pin");
    check(!cropWired(replaced(cpp, "plans[eye] = desktopWindowCropPlan(S.cropOpt, snap.meshServed[eye], eye);", "plans[eye] = desktopWindowCropPlan(S.cropOpt, false, eye);")), "control: a plan that ignores the mesh trips the pin");
    check(!cropWired(replaced(cpp, "cropBounds[eye], S.shiftOpt, plans[eye].zoom);", "cropBounds[eye], S.shiftOpt);")), "control: a crop that is not given the zoom trips the pin");
    check(!cropWired(replaced(cpp, "ratio > 0 ? (S.mode == DesktopWindowMode::BothEyes ? ratio * 0.5 : ratio) : 0.0, true,", "(S.mode == DesktopWindowMode::BothEyes ? 0.5 : 1.0) * 1.7, true,")),
          "control: a crop that does not give full a target of 0 trips the pin");
    check(!cropWired(replaced(cpp, "desktop window: crop rule %s (was %s)", "desktop window: crop %s (was %s)")), "control: a change that is not logged trips the pin");
    check(!cropWired(replaced(cpp, "desktop window: crop %s, %s eye, rule %s, vertical shift %d; crop bounds", "desktop window: crop %s, %s eye, %s, vertical shift %d; crop bounds")), "control: a crop line that does not name the rule trips the pin");
    check(!cropWired(cpp + "\n// desktopWindowProjectionCentre(angles)\n"), "control: the projection-centre rule coming back trips the pin");
    // Round 6: a live change of the style or the shape is not a fight; the style goes to the new target and the fight state is forgotten.
    auto liveChange = [](const std::string& t) {
        const size_t a = at(t, "if (S.activeStyle != S.styleOpt) {");
        const size_t b = at(t, "S.ourStyle = static_cast<LONG_PTR>(desktopWindowStyleFor(S.activeStyle, static_cast<uint32_t>(S.gameStyle)));", a);
        const size_t c = at(t, "S.styleSeenOurs = false;", b);
        const size_t d = at(t, "if (S.activeAspect != S.aspectOpt) {", c);
        const size_t e = at(t, "S.keepHeightOnce = true;", d);
        const size_t f = at(t, "const bool borderless = S.activeStyle == DesktopWindowStyle::Borderless;");
        return a != std::string::npos && b != std::string::npos && c != std::string::npos && d != std::string::npos && e != std::string::npos && f != std::string::npos &&
               at(t, "desktopWindowBorderlessPlan(mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right, mi.rcMonitor.bottom)") != std::string::npos &&
               at(t, "desktop window: aspect %s (was %s)") != std::string::npos && at(t, "desktop window: style %s (was %s)") != std::string::npos &&
               at(t, "\"aspect %s, %s%s\"") != std::string::npos && at(t, "picture %s; window %s") != std::string::npos &&
               at(t, "S.activeStyle == DesktopWindowStyle::Window && !desktopWindowAspectOff") != std::string::npos &&
               at(t, "desktopWindowCorrect(aspect, S.settledRect[0], S.settledRect[1], S.settledClient[0], S.settledClient[1], 0, 0, settledDx, settledDy,") != std::string::npos;
    };
    check(liveChange(cpp), "a live change of the style or the shape resets the fight state and asks for the new target; borderless is the monitor's rectangle; the lines name the aspect and the style");
    check(!liveChange(replaced(cpp, "S.styleSeenOurs = false;\n            S.pending = false;\n            S.pendingStyle = false;\n            S.havePrev = false;", "S.pending = false;\n            S.pendingStyle = false;\n            S.havePrev = false;")),
          "control: a style change that does not forget the fight state trips the pin");
    check(!liveChange(replaced(cpp, "S.keepHeightOnce = true;", "")), "control: a shape change that does not keep the height trips the pin");

    // The cursor (rounds 3 to 5): three IAT hooks on the executable that pass everything through; only ShowCursor's count is changed, and only while the cursor is hidden.
    const std::string cursor = readFile(root + "\\src\\d3d11\\desktop_window_cursor.cpp");
    const std::string proxy = readFile(root + "\\src\\d3d11\\d3d11_proxy.cpp");
    const std::string gate = readFile(root + "\\src\\d3d11\\input_gate.cpp");
    check(!cursor.empty() && !proxy.empty() && !gate.empty(), "the cursor pins' files are where they look");
    auto between = [](const std::string& t, const char* from, const char* to) {
        const size_t a = t.find(from);
        const size_t b = a == std::string::npos ? a : t.find(to, a);
        return a == std::string::npos || b == std::string::npos ? std::string() : t.substr(a, b - a);
    };
    auto cursorHooks = [&](const std::string& t) {
        const std::string setPos = between(t, "BOOL WINAPI hookSetCursorPos(", "BOOL WINAPI hookGetCursorPos(");
        const std::string getPos = between(t, "BOOL WINAPI hookGetCursorPos(", "int WINAPI hookShowCursor(");
        const std::string show = between(t, "int WINAPI hookShowCursor(", "template <class F>");
        return at(t, "iatHookInstall(\"user32.dll\", \"SetCursorPos\", reinterpret_cast<void*>(&hookSetCursorPos)") != std::string::npos &&
               at(t, "iatHookInstall(\"user32.dll\", \"GetCursorPos\", reinterpret_cast<void*>(&hookGetCursorPos)") != std::string::npos &&
               at(t, "iatHookInstall(\"user32.dll\", \"ShowCursor\", reinterpret_cast<void*>(&hookShowCursor)") != std::string::npos &&
               at(t, "only ShowCursor's count is changed, and only while the cursor is hidden") != std::string::npos &&
               // SetCursorPos and GetCursorPos: counted only while active, and the real function's answer always; ShowCursor: the real count, or the game's count when hidden
               at(setPos, "if (g_active.load(std::memory_order_relaxed)) {") != std::string::npos && at(setPos, "return g_realSetPos(x, y);") != std::string::npos &&
               at(setPos, "return TRUE") == std::string::npos && at(setPos, "return FALSE") == std::string::npos &&
               at(getPos, "if (g_active.load(std::memory_order_relaxed))") != std::string::npos && at(getPos, "return g_realGetPos(point);") != std::string::npos &&
               at(getPos, "return TRUE") == std::string::npos && at(getPos, "return FALSE") == std::string::npos &&
               at(show, "const int real = g_realShow(show);") != std::string::npos && at(show, "return count;") != std::string::npos && count(show, "return ") == 1;
    };
    check(cursorHooks(cursor), "SetCursorPos, GetCursorPos and ShowCursor are patched in the executable and pass through; only ShowCursor's count is changed, while the cursor is hidden");
    check(!cursorHooks(replaced(cursor, "return g_realSetPos(x, y);", "return TRUE;")), "control: a SetCursorPos that does not reach the real one trips the pin");
    check(!cursorHooks(replaced(cursor, "return g_realGetPos(point);", "return TRUE;")), "control: a GetCursorPos that does not reach the real one trips the pin");
    // Round 5: the game's mouse lock (ClipCursor to the client rectangle, found in round 3) is needed for on-foot VR and is not touched in any state: nothing in the product
    // installs a hook on ClipCursor or calls it, and the old release is gone.
    auto clipUntouched = [](const std::string& t) {
        return at(t, "ClipCursor(") == std::string::npos && at(t, "\"ClipCursor\"") == std::string::npos && at(t, "hookClipCursor") == std::string::npos &&
               at(t, "g_patchClip") == std::string::npos && at(t, "g_realClip") == std::string::npos && at(t, "ClipVerdict") == std::string::npos && at(t, "g_clipReleased") == std::string::npos &&
               at(t, "clip released") == std::string::npos;
    };
    unsigned scanned = 0, touching = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root + "\\src")) {
        const std::string ext = entry.path().extension().string();
        if (!entry.is_regular_file() || (ext != ".cpp" && ext != ".h" && ext != ".inl" && ext != ".hpp")) continue;
        ++scanned;
        if (!clipUntouched(readFile(entry.path().string()))) ++touching;
    }
    check(scanned > 100 && touching == 0, "no source under src installs a ClipCursor hook or calls ClipCursor, and the release verdict, counters and line are gone");
    check(clipUntouched(cursor) && clipUntouched(readFile(root + "\\src\\common\\desktop_window_math.h")) && clipUntouched(cpp), "the desktop window's own sources are clean of it");
    check(!clipUntouched(cursor + "\nvoid f() { ClipCursor(nullptr); }\n"), "control: a call to ClipCursor trips the pin");
    check(!clipUntouched(replaced(cursor, "\"SetCursorPos\"", "\"ClipCursor\"")), "control: an import patch of ClipCursor trips the pin");
    check(!clipUntouched(cursor + "\nBOOL WINAPI hookClipCursor(const RECT* rect);\n"), "control: a ClipCursor hook trips the pin");
    check(!clipUntouched(readFile(root + "\\src\\common\\desktop_window_math.h") + "\nenum class DwClipVerdict { Pass };\n"), "control: the release verdict coming back trips the pin");
    // Installed once at startup, in a VR-scope install only, and put back at shutdown.
    auto installed = [](const std::string& t) {
        const size_t bypass = at(t, "LoadLibrary deadlock bypass installed in executable");
        const size_t install = at(t, "if (edvr::runtimeScopeIncludesVr()) edvr::desktopWindowCursorInstall();", bypass);
        const size_t shutdown = at(t, "void shutdown() {");
        const size_t uninstall = at(t, "edvr::desktopWindowCursorUninstall();", shutdown);
        return bypass != std::string::npos && install != std::string::npos && shutdown != std::string::npos && uninstall != std::string::npos && count(t, "desktopWindowCursorInstall()") == 1;
    };
    check(installed(proxy), "the cursor hooks are installed once at startup in the VR scope and uninstalled at shutdown");
    check(!installed(replaced(proxy, "if (edvr::runtimeScopeIncludesVr()) edvr::desktopWindowCursorInstall();", "edvr::desktopWindowCursorInstall();")), "control: an install for every scope trips the pin");
    // The active flag: set when an activation commits, cleared in leave (before its early return).
    auto flagged = [](const std::string& t, const std::string& c) {
        const size_t leaveAt = at(t, "void leave(const char* reason) {");
        const size_t clearAt = at(t, "desktopWindowCursorSetActive(false);", leaveAt);
        const size_t returnAt = at(t, "if (!S.active) return;", leaveAt);
        const size_t commit = at(t, "S.active = true;\n        desktopWindowCursorSetActive(true);");
        return leaveAt != std::string::npos && clearAt != std::string::npos && returnAt != std::string::npos && clearAt < returnAt && commit != std::string::npos &&
               at(c, "if (active && !was) g_centre.store(0, std::memory_order_relaxed);") != std::string::npos;
    };
    check(flagged(cpp, cursor), "the cursor flag is set at the activation's commit and cleared in leave before its early return");
    check(!flagged(replaced(cpp, "desktopWindowCursorSetActive(false);", ""), cursor), "control: a flag never cleared trips the pin");
    check(!flagged(replaced(cpp, "S.active = true;\n        desktopWindowCursorSetActive(true);", "S.active = true;"), cursor), "control: a flag never set trips the pin");
    // The hidden cursor (round 4): on the window's thread only, undone in SetActive(false), decided after the centre is published.
    auto hides = [&](const std::string& c, const std::string& w) {
        const std::string hide = between(c, "void desktopWindowCursorHide(HWND hwnd) {", "int desktopWindowCursorTakeLine(");
        const std::string hook = between(c, "int WINAPI hookShowCursor(BOOL show) {", "template <class F>");
        const std::string active = between(c, "void desktopWindowCursorSetActive(bool active) {", "void desktopWindowCursorHide(HWND hwnd) {");
        const size_t centre = at(w, "desktopWindowCursorPublishCentre(centre.x, centre.y);"), decide = at(w, "desktopWindowCursorHide(hwnd);", centre);
        return at(hide, "GetWindowThreadProcessId(hwnd, nullptr)") != std::string::npos && at(hide, "if (owner != here) {") != std::string::npos &&
               at(hide, "the cursor is not hidden -- the window belongs to thread %lu, Present runs on %lu") != std::string::npos &&
               at(hide, "desktopWindowHideCursor(true, GetForegroundWindow() == hwnd, inside)") != std::string::npos && at(hide, "GetCursorPos(&cursor)") != std::string::npos &&
               at(hide, "ClientToScreen(hwnd, &origin)") != std::string::npos && at(hide, "desktopWindowPointInRect(cursor.x, cursor.y, origin.x, origin.y, origin.x + client.right, origin.y + client.bottom)") != std::string::npos &&
               at(hide, "g_count.hide(realShow);") != std::string::npos && at(hide, "g_count.unhide(realShow);") != std::string::npos &&
               at(hook, "if (g_hideThread.load(std::memory_order_relaxed) == GetCurrentThreadId())") != std::string::npos &&
               at(hook, "g_count.afterGameCall(real, g_hideWanted.load(std::memory_order_relaxed), realShow);") != std::string::npos &&
               at(active, "if (g_realShow) g_count.unhide(realShow);") != std::string::npos && at(active, "g_hideWanted.store(false") != std::string::npos &&
               centre != std::string::npos && decide != std::string::npos;
    };
    check(hides(cursor, cpp), "the cursor is hidden by the rule, on the window's thread only (said once otherwise), with the hook acting on that thread only, and the hides given back when the activation ends");
    check(!hides(replaced(cursor, "if (owner != here) {", "if (false) {"), cpp), "control: no thread check trips the pin");
    check(!hides(replaced(cursor, "if (g_hideThread.load(std::memory_order_relaxed) == GetCurrentThreadId())", "if (true)"), cpp), "control: a ShowCursor hook that acts on every thread trips the pin");
    check(!hides(replaced(cursor, "if (g_realShow) g_count.unhide(realShow);", ""), cpp), "control: no undo when the activation ends trips the pin");
    check(!hides(cursor, replaced(cpp, "desktopWindowCursorHide(hwnd);", "")), "control: a rule never decided trips the pin");
    {   // the line follows the census line, every 10 s, and the centre is published every Present
        const size_t fn = at(cpp, "bool desktopWindowPresent(");
        const size_t census = at(cpp, "g_census.format(census, sizeof census);", fn), take = at(cpp, "desktopWindowCursorTakeLine(cursor, sizeof cursor);", census);
        check(fn != std::string::npos && census != std::string::npos && take != std::string::npos && at(cpp, "desktopWindowCursorPublishCentre(centre.x, centre.y);") != std::string::npos,
              "the cursor line is written after the census line, and the client centre is published");
    }
    // The DirectInput mouse: only its cooperative level is observed, the driver's answer is forwarded first and returned unchanged, in the VR scope.
    auto mouseObserved = [&](const std::string& t) {
        const std::string hook = between(t, "HRESULT STDMETHODCALLTYPE hookMouseSetCooperativeLevel(", "void captureMouse(");
        const size_t branch = at(t, "} else if (isMouseGuid(guid)) {");
        return at(t, "constexpr size_t kSlotSetCooperativeLevel = 13;") != std::string::npos &&
               at(hook, "const HRESULT hr = g_mouseDoor.original(self, hwnd, flags);") != std::string::npos && at(hook, "desktopWindowNoteMouseCooperative(flags, hr);") != std::string::npos &&
               at(hook, "return hr;") != std::string::npos && at(hook, "original(self, hwnd, flags)") < at(hook, "desktopWindowNoteMouseCooperative") &&
               branch != std::string::npos && at(t, "if (runtimeScopeIncludesVr()) guardedBudget(g_budgetDi, [&] { captureMouse(*device); });", branch) != std::string::npos &&
               at(t, "d.hook.replace(kSlotSetCooperativeLevel,") != std::string::npos && count(t, "kSlotSetCooperativeLevel,") == 1;
    };
    check(mouseObserved(gate), "the game's DirectInput mouse: SetCooperativeLevel (slot 13) is forwarded first and its flags are logged, in the VR scope");
    check(!mouseObserved(replaced(gate, "if (runtimeScopeIncludesVr()) guardedBudget(g_budgetDi, [&] { captureMouse(*device); });", "guardedBudget(g_budgetDi, [&] { captureMouse(*device); });")),
          "control: a mouse watched in every scope trips the pin");
    check(!mouseObserved(replaced(gate, "const HRESULT hr = g_mouseDoor.original(self, hwnd, flags);", "const HRESULT hr = S_OK;")), "control: a SetCooperativeLevel that does not reach the driver trips the pin");

    // The build: the export, the source, the shaders, and the rig itself.
    check(count(build, "desktop_window_cursor.cpp") == 3, "build.bat compiles the cursor module into the DLL and into the two rigs that compile input_gate.cpp");
    check(at(build, "--extra-export edvrAcquireDesktopWindow") != std::string::npos && at(build, "\"src\\d3d11\\desktop_window.cpp\"") != std::string::npos &&
              at(build, ":rig_desktop_window_test") != std::string::npos,
          "build.bat exports edvrAcquireDesktopWindow, compiles desktop_window.cpp and runs this rig");
    check(at(shaderTool, "fixed_desktop_window_variants.h") != std::string::npos && at(shaderTool, "kDesktopWindowPsBytecode") != std::string::npos, "the shader build registers the two desktop window shaders");

    // edvr.ini: [visual], after world_scale, the three keys in order, each with its ui: line on the Desktop window page, live, and the sentence the menu reads.
    const size_t visual = at(ini, "\n[visual]\n");
    const size_t world = at(ini, "\nworld_scale = 100\n", visual);
    const size_t key = at(ini, "\ndesktop_window = left_eye\n", world);
    const size_t aspectKey = at(ini, "\ndesktop_window_aspect = full\n", key), cropKey = at(ini, "\ndesktop_window_crop = native\n", aspectKey);
    const size_t shiftKey = at(ini, "\ndesktop_window_vertical_shift = 0\n", cropKey);
    const size_t styleKey = at(ini, "\ndesktop_window_style = window\n", shiftKey);
    const size_t ui = ini.rfind("# ui: Image | choices game=Game, left_eye=Left eye, right_eye=Right eye, both_eyes=Both eyes | live | menu desktop_window\n", key);
    const size_t uiAspect = ini.rfind("# ui: Shape | choices full=Full picture, 1:1=1:1, 4:3=4:3, 16:10=16:10, 16:9=16:9, 21:9=21:9 | live | menu desktop_window\n", aspectKey);
    const size_t uiCrop = ini.rfind("# ui: Crop | choices native=Same as the game's mirror, center=Centre | live | menu desktop_window\n", cropKey);
    const size_t uiShift = ini.rfind("# ui: Vertical shift | range -100..100 | step 5 | unit % | signed | zero default | recommended 0 | live | menu desktop_window\n", shiftKey);
    const size_t uiStyle = ini.rfind("# ui: Style | choices window=Window, borderless=Borderless full screen | live | menu desktop_window\n", styleKey);
    check(visual != std::string::npos && world != std::string::npos && key != std::string::npos && ui != std::string::npos && ui > world && key - ui < 160 &&
              aspectKey != std::string::npos && uiAspect != std::string::npos && uiAspect > key && aspectKey - uiAspect < 200 && cropKey != std::string::npos && uiCrop != std::string::npos &&
              uiCrop > aspectKey && cropKey - uiCrop < 200 && shiftKey != std::string::npos && uiShift != std::string::npos && uiShift > cropKey && shiftKey - uiShift < 200 && styleKey != std::string::npos && uiStyle != std::string::npos && uiStyle > shiftKey && styleKey - uiStyle < 160 &&
              count(ini, "\ndesktop_window = ") == 1 && count(ini, "\ndesktop_window_aspect = ") == 1 && count(ini, "\ndesktop_window_crop = ") == 1 && count(ini, "\ndesktop_window_vertical_shift = ") == 1 &&
              count(ini, "\ndesktop_window_style = ") == 1,
          "edvr.ini: [visual], after world_scale, the five keys in order ship left_eye, full, native, 0 and window, each with its ui: line on the Mirror window page, labelled Image, Shape, Crop, Vertical shift and Style (the page names the window) (the crop two choices, the shift a signed number row, step 5, unit %)");
    check(ini.rfind("VR only. Live.\n# ui: Image |", key) != std::string::npos && ini.rfind("VR only. Live.\n# ui: Shape |", aspectKey) != std::string::npos &&
              ini.rfind("VR only. Live.\n# ui: Crop |", cropKey) != std::string::npos && ini.rfind("VR only. Live.\n# ui: Vertical shift |", shiftKey) != std::string::npos && ini.rfind("VR only. Live.\n# ui: Style |", styleKey) != std::string::npos,
          "edvr.ini: each comment says VR only and live");
    check(at(ini, "menu visual", key) == std::string::npos || at(ini, "menu visual", key) > styleKey, "edvr.ini: none of the five is on the Visual page");
    {   // the words: the shift's 0 is the default centre; the crop's comment says what native and center do, with Elite's two numbers and the mirrored right eye; nothing says straight ahead
        auto unwrapped = [](std::string s) {
            size_t p;
            while ((p = s.find("\n# ")) != std::string::npos) s.replace(p, 3, " ");
            return s;
        };
        const size_t from = ini.rfind("\n# Moves the cut-out", shiftKey);
        const size_t cropFrom = ini.rfind("\n# Where the cut-out sits", cropKey);
        const std::string words = from == std::string::npos || shiftKey == std::string::npos ? std::string() : unwrapped(ini.substr(from, shiftKey - from));
        const std::string shapeWords = key != std::string::npos && aspectKey > key ? unwrapped(ini.substr(key, aspectKey - key)) : std::string();
        const std::string cropWords = cropFrom == std::string::npos || cropKey == std::string::npos ? std::string() : unwrapped(ini.substr(cropFrom, cropKey - cropFrom));
        auto wordsOk = [](const std::string& w, const std::string& sh, const std::string& cw) {
            return !w.empty() && at(w, "0 is the default centre") != std::string::npos && at(w, "does nothing with the shape \"full\"") != std::string::npos && at(w, "straight ahead") == std::string::npos &&
                   !sh.empty() && at(sh, "straight ahead") == std::string::npos && !cw.empty() && at(cw, "straight ahead") == std::string::npos && at(cw, "\"native\" (the default)") != std::string::npos &&
                   at(cw, "\"center\"") != std::string::npos && at(cw, "1.32 times") != std::string::npos && at(cw, "0.515") != std::string::npos && at(cw, "0.485") != std::string::npos &&
                   at(cw, "left eye") != std::string::npos && at(cw, "right eye") != std::string::npos && at(cw, "\"full\" is never cropped or zoomed") != std::string::npos &&
                   at(cw, "hidden area") != std::string::npos;
        };
        check(wordsOk(words, shapeWords, cropWords),
              "edvr.ini: the shift's comment says 0 is the default centre and what it does nothing for; the crop's comment names both rules, Elite's 1.32 and 0.515, the mirrored 0.485 and that full is never cropped; none says straight ahead");
        check(!wordsOk(replaced(words, "0 is the default centre", "0 is straight ahead"), shapeWords, cropWords), "control: a shift comment that says straight ahead trips the words pin");
        check(!wordsOk(words, replaced(shapeWords, "cut the picture to", "straight ahead cut the picture to"), cropWords), "control: a shape comment that says straight ahead trips the words pin");
        check(!wordsOk(words, shapeWords, replaced(cropWords, "0.485", "0.5")), "control: a crop comment that does not say the right eye's mirror trips the words pin");
        check(!wordsOk(words, shapeWords, replaced(cropWords, "1.32 times", "twice")), "control: a crop comment with another zoom trips the words pin");
    }
    const std::string schemaTool = readFile(root + "\\tools\\gen_settings_schema.py");
    check(at(schemaTool, "'visual', 'desktop_window', 'flat_visual', 'debug')") != std::string::npos, "the schema generator knows the desktop_window page");
    check(at(cpp, "cfg.getString(\"visual.desktop_window_aspect\", \"full\")") != std::string::npos && at(cpp, "cfg.getString(\"visual.desktop_window_style\", \"window\")") != std::string::npos &&
              at(flatKeys, "desktop_window") == std::string::npos,
          "the shape and style keys are read as one word each, defaults full and window, in the VR profile only (the flat profile's allow-list does not carry them or the crop)");

    // Round 7: the vertical shift is one whole number, 0 by default, held to -100..100, read live with the others; the flat profile resets it before any read; the crop of both eyes is
    // given it; its first read and each change are logged; the first-activation crop line carries it.
    auto shiftWired = [](const std::string& t) {
        const size_t flat = at(t, "if (flat) {");
        const size_t flatShift = flat == std::string::npos ? flat : at(t, "S.shiftOpt = 0;\n        return;", flat);
        const size_t read = at(t, "cfg.getIntInRange(\"visual.desktop_window_vertical_shift\", 0, -100, 100)");
        const size_t keep = at(t, "S.shiftOpt = shiftOpt;", read);
        return count(t, "cfg.getIntInRange(\"visual.desktop_window_vertical_shift\"") == 1 && read != std::string::npos && flatShift != std::string::npos && flatShift < read && keep != std::string::npos &&
               count(t, "cropBounds[eye], S.shiftOpt, plans[eye].zoom);") == 1 && at(t, "desktop window: vertical shift %d (was %d)") != std::string::npos &&
               at(t, "desktop window: vertical shift %d\"") != std::string::npos && at(t, "rule %s, vertical shift %d; crop bounds") != std::string::npos &&
               at(t, "ruleText, S.shiftOpt, cropBounds[eye][0]") != std::string::npos && at(t, "straight ahead") == std::string::npos;
    };
    check(shiftWired(cpp), "the vertical shift is read as one whole number from -100 to 100 (default 0), reset by the flat profile, passed to the crop of both eyes, and logged (first read, change, crop line)");
    check(!shiftWired(replaced(cpp, "\"visual.desktop_window_vertical_shift\", 0, -100, 100", "\"visual.desktop_window_vertical_shift\", 0, 0, 100")), "control: a shift that cannot go below 0 trips the pin");
    check(!shiftWired(replaced(cpp, "cropBounds[eye], S.shiftOpt, plans[eye].zoom);", "cropBounds[eye], 0, plans[eye].zoom);")), "control: a crop that is not given the shift trips the pin");
    check(!shiftWired(replaced(cpp, "S.shiftOpt = 0;\n        return;", "return;")), "control: a flat profile that does not reset the shift trips the pin");
    check(!shiftWired(replaced(cpp, "desktop window: vertical shift %d (was %d)", "desktop window: shift %d (was %d)")), "control: a change that is not logged trips the pin");
    check(!shiftWired(replaced(cpp, "S.shiftOpt = shiftOpt;", "")), "control: a read that is not kept trips the pin");
    check(!shiftWired(replaced(cpp, "rule %s, vertical shift %d; crop bounds", "straight ahead %s, vertical shift %d; crop bounds")), "control: a crop line that says straight ahead trips the pin");
    check(at(flatKeys, "vertical_shift") == std::string::npos, "the flat profile's allow-list does not carry visual.desktop_window_vertical_shift");
}

// The desktop window's vertical sync (desktop_vsync.h): VR presents with 0 whatever Elite asks, flat as Elite asks; one line per change, never per frame. The wiring: the
// Present hook calls the real Present with the decided value on Elite's own swapchain, and the trace keeps the value Elite asked for. Each pin has a control that trips.
static void vsyncRules() {
    check(desktopVsyncPresented(false, 1) == 0 && desktopVsyncPresented(false, 2) == 0 && desktopVsyncPresented(false, 0) == 0, "vsync: VR presents with 0 whatever Elite asks");
    check(desktopVsyncPresented(true, 1) == 1 && desktopVsyncPresented(true, 0) == 0 && desktopVsyncPresented(true, 4) == 4, "vsync: flat presents Elite's own interval");
    DesktopVsyncNote n;
    const bool first = n.changed(false, 1), again = n.changed(false, 1), asked = n.changed(false, 0), toFlat = n.changed(true, 0), same = n.changed(true, 0);
    check(first && !again && asked && toFlat && !same, "vsync: a line on the first Present and at each change of Elite's value or the profile, none per frame");
    char line[200];
    formatDesktopVsync(line, sizeof(line), false, 1);
    check(std::strstr(line, "desktop vsync (VR): Elite asked sync interval 1; presented with 0") == line, "vsync: the VR override's line names what Elite asked and the 0 presented");
    formatDesktopVsync(line, sizeof(line), false, 0);
    check(std::strcmp(line, "desktop vsync (VR): Elite asks sync interval 0; nothing to override") == 0, "vsync: a VR Present already at 0 says so, so a log tells the code ran");
    formatDesktopVsync(line, sizeof(line), true, 1);
    check(std::strstr(line, "desktop vsync: the flat profile") == line && std::strstr(line, "presented as asked") != nullptr, "vsync: the flat line says Elite's interval stands");
}
static void vsyncWiring(const std::string& root) {
    const std::string hook = readFile(root + "\\src\\d3d11\\device_hook.cpp");
    auto wired = [](const std::string& t) {
        const size_t mine = at(t, "if (self != g_state->swapChain) {\n        return g_state->realPresent(self, syncInterval, flags);");
        const size_t decide = at(t, "const UINT presentSync = desktopVsyncPresented(flatProfileNow, syncInterval);");
        const size_t call = at(t, "const HRESULT hr = g_state->realPresent(self, presentSync, flags);");
        return mine != std::string::npos && decide != std::string::npos && call != std::string::npos && mine < decide && decide < call &&
               count(t, "g_state->realPresent(self, syncInterval, flags)") == 1 && at(t, "GetCurrentThreadId(), syncInterval, flags, static_cast<int32_t>(hr)};") != std::string::npos;
    };
    check(!hook.empty() && wired(hook), "vsync wiring: another swapchain's Present is forwarded as asked; Elite's is presented with the decided interval; the trace keeps Elite's value");
    check(!wired(replaced(hook, "g_state->realPresent(self, presentSync, flags);", "g_state->realPresent(self, syncInterval, flags);")), "vsync control: a real Present given Elite's value trips the pin");
    check(!wired(replaced(hook, "GetCurrentThreadId(), syncInterval, flags,", "GetCurrentThreadId(), presentSync, flags,")), "vsync control: a trace that records the presented value trips the pin");
}

int main(int argc, char** argv) {
    vsyncRules();
    rules();
    shapeRules();
    windowRules();
    cursorRules();
    hideRules();
    lines();
    handoff();
    if (argc > 1) {
        wiring(argv[1]);
        vsyncWiring(argv[1]);
    } else {
        check(false, "argv[1] is the repository root");
    }
    std::printf("desktop_window_test: %u checks, %u failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
