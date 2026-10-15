// The blend probe's pure logic (src\d3d11\blend_probe_core.h): the coverage window, which draws are rows, the row and grid text and its
// line length against the log's buffer, the refusal reasons, and the grid's indexing. No D3D11: the device path is exercised in the DLL's
// own run. --self-test exits nonzero on the first failed check.
#include "../../src/d3d11/blend_probe_core.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>

using namespace edvr::blend;

namespace {
unsigned g_checks = 0;
int g_failures = 0;

void check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("FAIL: %s\n", what);
    }
}

bool near(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) <= eps; }

void testWindow() {
    Rect r;
    check(windowRect(0, 0, 2016, 2016, 2016, 2016, &r), "a full viewport has a window");
    check(r.x0 == 960 && r.x1 == 1056 && r.y0 == 960 && r.y1 == 1056, "the window is 96 square about the viewport's centre");
    check(r.width() == kWindow && r.height() == kWindow && r.area() == kWindow * kWindow, "width, height and area");
    // A viewport that is not the whole target: the right eye's half of a shared texture.
    check(windowRect(1000, 0, 1000, 1000, 2000, 1000, &r) && r.x0 == 1452 && r.x1 == 1548 && r.y0 == 452 && r.y1 == 548,
          "the centre is the viewport's, not the target's");
    // Clipping to the target.
    check(windowRect(0, 0, 40, 40, 2016, 2016, &r) && r.x0 == 0 && r.y0 == 0 && r.x1 == 68 && r.y1 == 68,
          "a window that starts off the target is clipped to it");
    check(windowRect(0, 0, 160, 160, 100, 100, &r) && r.x0 == 32 && r.x1 == 100 && r.y1 == 100, "a window that ends off the target is clipped");
    // Nothing left.
    check(!windowRect(0, 0, 2016, 2016, 0, 2016, &r), "a target with no width has no window");
    check(!windowRect(0, 0, 0, 2016, 2016, 2016, &r), "a viewport with no width has no window");
    check(!windowRect(0, 0, 2016, -1, 2016, 2016, &r), "a viewport with negative height has no window");
    check(!windowRect(5000, 5000, 100, 100, 2016, 2016, &r), "a viewport whose centre is far off the target has no window");
    check(!windowRect(0, 0, 100, 100, 2016, 2016, nullptr), "a null out refuses");
    check(windowRect(0, 0, 50, 50, 50, 50, &r) && r.width() == 50 && r.height() == 50, "a target smaller than the window clips to itself");
    // A float centre is floored, not rounded.
    check(windowRect(0, 0, 2001, 2001, 4000, 4000, &r) && r.x0 == 1000 - 48 + 0, "an odd viewport's centre is floored");
}

void testCandidate() {
    check(isCandidate(true, true, true), "blending writes colour: a candidate even if it writes depth");
    check(isCandidate(true, true, false), "blend and no depth write");
    check(isCandidate(true, false, false), "no blend, no depth write (an additive-looking opaque overlay)");
    check(!isCandidate(true, false, true), "an opaque depth-writing draw is not a candidate");
    check(!isCandidate(false, true, false), "a draw that writes no colour is not a candidate");
    check(!isCandidate(false, false, false), "nor a depth-only draw without a write");
    DepthDesc d;
    check(!depthWrites(d), "no depth view: no depth write");
    d.dsvBound = true;
    d.test = true;
    d.write = true;
    check(depthWrites(d), "a bound view with test and write on");
    d.test = false;
    check(!depthWrites(d), "depth test off: nothing is written");
    d.test = true;
    d.write = false;
    check(!depthWrites(d), "a zero write mask writes nothing");
    check(coversWindow(1) && !coversWindow(0) && !coversWindow(kCoverageUnknown), "unmeasured coverage does not cover");
}

void testStateText() {
    char b[160];
    BlendDesc bd;
    formatBlend(b, sizeof(b), bd);
    check(std::strcmp(b, "off wm=F") == 0, "a draw that does not blend");
    bd.enable = true;
    bd.src = 5;
    bd.dst = 2;
    bd.op = 1;
    bd.srcA = 2;
    bd.dstA = 2;
    bd.opA = 1;
    formatBlend(b, sizeof(b), bd);
    check(std::strcmp(b, "SRC_ALPHA,ONE,ADD/ONE,ONE,ADD wm=F") == 0, "SRC_ALPHA, ONE additive");
    bd.alphaToCoverage = true;
    bd.writeMask = 7;
    formatBlend(b, sizeof(b), bd);
    check(std::strstr(b, "wm=7 a2c") != nullptr, "write mask and alpha-to-coverage");
    bd.src = 99;
    formatBlend(b, sizeof(b), bd);
    check(std::strncmp(b, "?,", 2) == 0, "an unknown factor is a question mark, not a crash");
    DepthDesc d;
    formatDepth(b, sizeof(b), d);
    check(std::strcmp(b, "no depth view") == 0, "no depth view");
    d.dsvBound = true;
    d.test = true;
    d.func = 4;
    formatDepth(b, sizeof(b), d);
    check(std::strcmp(b, "test LESS_EQUAL write 0") == 0, "test LESS_EQUAL, no write");
    d.test = false;
    d.write = true;
    d.stencil = true;
    formatDepth(b, sizeof(b), d);
    check(std::strcmp(b, "test off write 1 stencil") == 0, "test off with a stencil");
    for (uint32_t v = 1; v <= 5; ++v) check(std::strcmp(blendOpName(v), "?") != 0, "every D3D11 blend op has a name");
    for (uint32_t v = 1; v <= 8; ++v) check(std::strcmp(compareName(v), "?") != 0, "every D3D11 comparison has a name");
}

void testMask() {
    int f = 0, a = 0;
    maskQuanta(0, &f, &a);
    check(f == 1 && a == 1, "the interface proper: 1 / 1");
    maskQuanta(1, &f, &a);
    check(f == 1 && a == 2, "the holo material and the sprite: floating 1, attached 2");
    maskQuanta(2, &f, &a);
    check(f == 1 && a == 2, "the flight HUD: floating 1, attached 2");
    maskQuanta(3, &f, &a);
    check(f == 1 && a == 1, "the smoke: one quantum, odd");
    maskQuanta(4, &f, &a);
    check(f == 1 && a == 1, "the scanner's chrome: 1 / 1");
    maskQuanta(0, nullptr, nullptr);
    check(std::strcmp(maskSlotName(1), "holo-material") == 0 && std::strcmp(maskSlotName(9), "?") == 0, "slot names");
}

Row sampleRow() {
    Row r;
    r.slot = 1;
    r.gameFrame = 123456;
    r.ordinal = 412;
    r.eye = 0;
    r.vs = 0x94D5C556DFD6D705ull;
    r.ps = 0x912477AEF6958379ull;
    r.drawKind = 'N';
    r.count = 6;
    r.instances = 233;
    std::strcpy(r.blend, "SRC_ALPHA,ONE,ADD/ONE,ONE,ADD wm=F");
    std::strcpy(r.depth, "test LESS_EQUAL write 0");
    std::strcpy(r.rtFormat, "R16G16B16A16_FLOAT");
    r.rtW = 2016;
    r.rtH = 2016;
    r.covered = 1234;
    std::strcpy(r.verdict, "glare-steady");
    std::strcpy(r.uiFamily, "hologram");
    std::strcpy(r.uiDecision, "left:verdict");
    std::strcpy(r.holo, "cockpit");
    std::strcpy(r.coverage, "holo-material:m1/2");
    std::strcpy(r.keyed, "rigid#7");
    std::strcpy(r.engine, "plain");
    return r;
}

void testRow() {
    char b[kRowBuf];
    const Row r = sampleRow();
    const int n = formatRow(b, sizeof(b), r);
    check(n > 0 && static_cast<size_t>(n) == std::strlen(b), "a row formats and reports its length");
    check(std::strncmp(b, "blend probe row:", 16) == 0, "a row starts with its prefix");
    check(std::strstr(b, "vs=94D5C556DFD6D705") != nullptr && std::strstr(b, "ps=912477AEF6958379") != nullptr, "hashes are 16 hex digits");
    check(std::strstr(b, "cov=1234") != nullptr, "covered pixels");
    check(std::strstr(b, "blend=[SRC_ALPHA,ONE,ADD/ONE,ONE,ADD wm=F]") != nullptr, "the blend string");
    check(std::strstr(b, "depth=[test LESS_EQUAL write 0]") != nullptr, "the depth string");
    check(std::strstr(b, "rt=R16G16B16A16_FLOAT 2016x2016") != nullptr, "the render target");
    check(std::strstr(b, "verdict=glare-steady ui=hologram/left:verdict holo=cockpit mask=holo-material:m1/2 kf=rigid#7 eng=plain") != nullptr,
          "the routes, in order");
    check(std::strstr(b, "n=6 inst=233") != nullptr && std::strstr(b, " N ") != nullptr, "kind, count and instances");
    // An unmeasured row says so.
    Row u = sampleRow();
    u.covered = kCoverageUnknown;
    formatRow(b, sizeof(b), u);
    check(std::strstr(b, "cov=?") != nullptr, "an unmeasured row shows cov=?");
    // A hash with leading zeros keeps its width.
    Row z = sampleRow();
    z.vs = 0x1Full;
    z.ps = 0;
    formatRow(b, sizeof(b), z);
    check(std::strstr(b, "vs=000000000000001F ps=0000000000000000") != nullptr, "hashes keep their sixteen digits");
    // The text must not fit a buffer that is too small, and says so.
    char tiny[40];
    check(formatRow(tiny, sizeof(tiny), r) == -1, "a row that does not fit a buffer returns -1");
    // The worst case fits the log: every string field at its full capacity and every number at its widest.
    Row w;
    w.slot = 2;
    w.gameFrame = 0xFFFFFFFFu;
    w.ordinal = 0xFFFFFFFFu;
    w.eye = -1;
    w.vs = ~0ull;
    w.ps = ~0ull;
    w.drawKind = 'X';
    w.count = 0xFFFFFFFFu;
    w.instances = 0xFFFFFFFFu;
    std::memset(w.blend, 'b', sizeof(w.blend) - 1);
    std::memset(w.depth, 'd', sizeof(w.depth) - 1);
    std::memset(w.rtFormat, 'r', sizeof(w.rtFormat) - 1);
    w.rtW = 0xFFFFFFFFu;
    w.rtH = 0xFFFFFFFFu;
    w.covered = 0x7FFFFFFFFFFFFFFFll;
    std::memset(w.verdict, 'v', sizeof(w.verdict) - 1);
    std::memset(w.uiFamily, 'f', sizeof(w.uiFamily) - 1);
    std::memset(w.uiDecision, 'u', sizeof(w.uiDecision) - 1);
    std::memset(w.holo, 'h', sizeof(w.holo) - 1);
    std::memset(w.coverage, 'c', sizeof(w.coverage) - 1);
    std::memset(w.keyed, 'k', sizeof(w.keyed) - 1);
    std::memset(w.engine, 'e', sizeof(w.engine) - 1);
    const int wn = formatRow(b, sizeof(b), w);
    check(wn > 0 && static_cast<size_t>(wn) < kLogMessageMax, "the widest possible row fits the log's line buffer");
    std::printf("  widest row: %d characters (limit %zu)\n", wn, kLogMessageMax);
}

void testRefusals() {
    check(std::strlen(refusalText(Refusal::kNone)) == 0, "no refusal has no text");
    std::string seen;
    for (uint8_t i = 1; i < static_cast<uint8_t>(Refusal::kCount); ++i) {
        const char* t = refusalText(static_cast<Refusal>(i));
        check(std::strlen(t) > 20, "every refusal has a reason of some length");
        check(seen.find(t) == std::string::npos, "no two refusals share a reason");
        seen += t;
        check(std::strlen(t) < kLogMessageMax - 80, "a refusal fits the log line with its prefix and detail");
    }
    check(std::strstr(refusalText(Refusal::kNotVr), "not VR") != nullptr, "the flat profile's refusal says not VR");
    check(std::strstr(refusalText(Refusal::kNoEyePass), "no eye pass") != nullptr, "the empty-scene refusal says no eye pass");
    check(std::strstr(refusalText(Refusal::kQueriesUnavailable), "occlusion queries unavailable") != nullptr, "the query refusal names the queries");
    check(std::strstr(refusalText(Refusal::kQueriesUnanswered), "cov=?") != nullptr, "unanswered queries say what the row shows");
}

void testGridIndex() {
    // The 96-pixel window: the cell centres are 6, 18, ..., 90.
    const uint32_t expect[8] = {6, 18, 30, 42, 54, 66, 78, 90};
    bool all = true;
    for (uint32_t i = 0; i < kGrid; ++i) all = all && gridCoord(i, 96) == expect[i];
    check(all, "the 8 cell centres over 96 pixels are 6, 18 ... 90");
    for (uint32_t extent = 1; extent <= 200; ++extent) {
        uint32_t prev = 0;
        bool inside = true, mono = true;
        for (uint32_t i = 0; i < kGrid; ++i) {
            const uint32_t c = gridCoord(i, extent);
            inside = inside && c < extent;
            mono = mono && (i == 0 || c >= prev);
            prev = c;
        }
        check(inside, "every grid coordinate is inside its extent");
        check(mono, "grid coordinates never go backwards");
    }
    check(gridCoord(0, 0) == 0, "an empty extent gives 0, not a division");
    check(gridCoord(99, 96) == gridCoord(7, 96), "an index past the grid clamps to the last cell");
    check(gridCell(0, 0) == 0 && gridCell(7, 0) == 7 && gridCell(0, 1) == 8 && gridCell(7, 7) == 63, "row-major cell indexing");
    bool unique = true;
    bool seen[64] = {};
    for (uint32_t j = 0; j < kGrid; ++j)
        for (uint32_t i = 0; i < kGrid; ++i) {
            const uint32_t k = gridCell(i, j);
            unique = unique && k < 64 && !seen[k];
            seen[k] = true;
        }
    check(unique, "the 64 cells index 64 distinct slots");
}

void testFlags() {
    DecisionFlags f = decodeFlags(1.0f);
    check(f.path == 1 && !f.hidden && !f.ui && f.engineKind == 0, "path 1, nothing else");
    // path 11 (engine), hidden, kind 1 in bits 12..14, UI.
    const float w = static_cast<float>(11u | 16u | 64u | (1u << 12));
    f = decodeFlags(w);
    check(f.path == 11 && f.hidden && f.ui && f.engineKind == 1, "engine path, hidden, UI, kind 1");
    f = decodeFlags(static_cast<float>(1u | (6u << 12)));
    check(f.engineKind == 6 && f.path == 1, "kind 6 is the stale pose");
    f = decodeFlags(static_cast<float>(1u | (7u << 12)));
    check(f.engineKind == 7, "all three kind bits");
    f = decodeFlags(-5.0f);
    check(f.path == 0 && f.engineKind == 0, "a negative word decodes to nothing");
    f = decodeFlags(std::numeric_limits<float>::quiet_NaN());
    check(f.path == 0, "a NaN word decodes to nothing");
    f = decodeFlags(1e30f);
    check(f.path == 0, "an out-of-range word decodes to nothing");
    check(isCameraPath(1) && isCameraPath(2) && !isCameraPath(11) && !isCameraPath(8), "the camera paths are 1 and 2");
    check(std::strcmp(engineKindName(1), "exact") == 0 && std::strcmp(engineKindName(4), "stale-slot") == 0 &&
          std::strcmp(engineKindName(0), "none") == 0 && std::strcmp(engineKindName(3), "other") == 0, "engine kind names");
}

GridSample cell(float mvx, float mvy, uint32_t path, uint32_t kind, float z = 0.5f) {
    GridSample s;
    s.valid = true;
    s.z = z;
    s.mvx = mvx;
    s.mvy = mvy;
    s.flags.path = path;
    s.flags.engineKind = kind;
    s.coverage = 0;
    s.bias = 0;
    return s;
}

void testMedianAndSummary() {
    float a[3] = {3, 1, 2};
    check(near(median(a, 3), 2.0f), "odd median");
    float b[4] = {4, 1, 2, 3};
    check(near(median(b, 4), 2.5f), "even median");
    check(near(median(nullptr, 0), 0.0f), "empty median");
    check(near(rotationDegrees(3.0f), 0.0f), "the identity rotates by zero");
    check(near(rotationDegrees(-1.0f), 180.0f, 1e-3f), "a half turn is 180 degrees");
    check(near(rotationDegrees(7.0f), 0.0f), "a trace above 3 clamps");
    // 64 camera cells at (1, -2); 8 of them replaced by an engine mover at (4, 0.5).
    GridSample g[64];
    for (auto& c : g) c = cell(1.0f, -2.0f, 1, 0);
    for (int i = 0; i < 8; ++i) g[gridCell(3, i)] = cell(4.0f, 0.5f, 11, 1);
    g[0].flags.hidden = true;
    g[1].flags.ui = true;
    g[2].coverage = 7;
    g[2].bias = 255;
    GridSummary s = summarize(g, 64);
    check(s.cells == 64 && s.cameraCells == 56 && s.engineCells == 8 && s.otherCells == 0, "cells by path");
    check(near(s.medCamera[0], 1.0f) && near(s.medCamera[1], -2.0f), "the camera-path median");
    check(near(s.medEngine[0], 4.0f) && near(s.medEngine[1], 0.5f), "the engine-path median");
    check(near(s.medAll[0], 1.0f), "the all-cells median is the majority's");
    check(s.haveDelta && near(s.medianDelta, std::sqrt(9.0f + 6.25f), 1e-3f), "engine minus camera");
    check(s.kindCells[0] == 56 && s.kindCells[1] == 8, "engine kind histogram");
    check(s.hidden == 1 && s.ui == 1 && s.coverageSet == 1 && s.biasSet == 1, "hidden, ui, coverage and bias counts");
    // Only camera cells: no delta.
    for (auto& c : g) c = cell(0.0f, 0.0f, 2, 0);
    s = summarize(g, 64);
    check(!s.haveDelta && s.engineCells == 0 && s.cameraCells == 64, "no engine cells: no delta, and it says so");
    // Cells that were not read are not counted.
    GridSample none[64];
    s = summarize(none, 64);
    check(s.cells == 0, "unread cells are not summarised");
    // Holo and screen paths are 'other'.
    for (auto& c : g) c = cell(0.0f, 0.0f, 8, 0);
    g[5] = cell(0.0f, 0.0f, 10, 0);
    s = summarize(g, 64);
    check(s.otherCells == 64, "holo and screen paths are other");
    // A short array never reads past it.
    s = summarize(g, 10);
    check(s.cells == 10, "a short array is summarised as far as it goes");
}

void testGridText() {
    char b[kRowBuf];
    GridSample s = cell(-1.2f, 0.31f, 11, 1, 0.84210f);
    s.flags.hidden = true;
    s.flags.ui = true;
    s.coverage = 3;
    s.bias = 255;
    formatCell(b, sizeof(b), s);
    check(std::strcmp(b, "z=0.84210 mv=-1.20,0.31 p=11HU e=1 c=3 b=255") == 0, "a cell's text");
    GridSample none;
    formatCell(b, sizeof(b), none);
    check(std::strcmp(b, "-") == 0, "an unread cell is a dash");
    GridSample noz = cell(0, 0, 1, 0);
    noz.z = std::numeric_limits<float>::quiet_NaN();
    noz.coverage = -1;
    noz.bias = -1;
    formatCell(b, sizeof(b), noz);
    check(std::strcmp(b, "z=- mv=0.00,0.00 p=1 e=0 c=- b=-") == 0, "a missing depth, coverage and bias are dashes");
    GridSample cells[64];
    for (auto& c : cells) c = s;
    cells[gridCell(2, 3)] = noz;
    const int n = formatGridRow(b, sizeof(b), 2, 3, cells);
    check(n > 0 && std::strncmp(b, "blend probe grid f=2 row 3:", 27) == 0, "a grid row's prefix");
    int brackets = 0;
    for (const char* c = b; *c; ++c)
        if (*c == '[') ++brackets;
    check(brackets == 8, "a grid row holds eight cells");
    // The widest cell, eight of them, fits the log line.
    GridSample wide = cell(-99999.99f, -99999.99f, 11, 7, 123456.78901f);
    wide.flags.hidden = true;
    wide.flags.ui = true;
    wide.coverage = 255;
    wide.bias = 255;
    for (auto& c : cells) c = wide;
    const int wn = formatGridRow(b, sizeof(b), 7, 7, cells);
    check(wn > 0 && static_cast<size_t>(wn) < kLogMessageMax, "the widest grid row fits the log's line buffer");
    std::printf("  widest grid row: %d characters (limit %zu)\n", wn, kLogMessageMax);
    check(formatGridRow(b, 64, 0, 0, cells) == -1, "a grid row that does not fit a buffer returns -1");

    GridHeader h;
    h.slot = 2;
    h.gameFrame = 0xFFFFFFFFu;
    h.sceneFrame = 0xFFFFFFFFu;
    h.winX = 960;
    h.winY = 960;
    h.winW = 96;
    h.winH = 96;
    h.inputW = 0xFFFFFFFFu;
    h.inputH = 0xFFFFFFFFu;
    h.rotDeg = 180.0f;
    h.transLen = 99999.0f;
    h.transValid = true;
    h.inputs = "-decision -depth -coverage -bias";
    const int hn = formatGridHead(b, sizeof(b), h);
    check(hn > 0 && static_cast<size_t>(hn) < kLogMessageMax, "the grid's head line fits the log's line buffer");
    std::printf("  grid head: %d characters (limit %zu)\n", hn, kLogMessageMax);
    check(std::strstr(b, "window x=960 y=960 96x96") != nullptr, "the head names the window");
    check(std::strlen(gridLegend()) + 20 < kLogMessageMax, "the grid legend fits the log's line buffer");
    check(std::strstr(gridLegend(), "p = the path") != nullptr && std::strstr(gridLegend(), "e = engine-record kind") != nullptr, "the legend explains the path and the engine kind");

    GridSummary sum;
    sum.cells = 64;
    sum.medAll[0] = -99999.99f;
    sum.medAll[1] = -99999.99f;
    sum.medCamera[0] = -99999.99f;
    sum.medCamera[1] = -99999.99f;
    sum.medEngine[0] = -99999.99f;
    sum.medEngine[1] = -99999.99f;
    for (auto& k : sum.kindCells) k = 0xFFFFFFFFu;
    sum.haveDelta = true;
    sum.medianDelta = 1234.5f;
    const int sn = formatGridSummary(b, sizeof(b), 2, sum);
    check(sn > 0 && static_cast<size_t>(sn) < kLogMessageMax, "the grid's summary line fits the log's line buffer");
    check(std::strstr(b, "engine minus camera median 1234.500 px") != nullptr, "the summary gives the delta");
    sum.haveDelta = false;
    formatGridSummary(b, sizeof(b), 0, sum);
    check(std::strstr(b, "n/a (needs both paths)") != nullptr, "the summary says when there is no delta");
}

void testInstallLine() {
    char b[kRowBuf];
    formatInstallLine(b, sizeof(b), false, true, "F3", "edvr.ini");
    check(std::strcmp(b, "blend probe: off; hotkey.blend_probe is bound to F3") == 0, "install line, bound: pinned text");
    formatInstallLine(b, sizeof(b), false, false, "", "edvr.ini");
    check(std::strcmp(b, "blend probe: off; hotkey.blend_probe is NOT bound -- add \"blend_probe = <a free key>\" under [hotkey] in edvr.ini to use it") == 0,
          "install line, unbound or empty: pinned text");
    formatInstallLine(b, sizeof(b), false, false, nullptr, nullptr);
    check(std::strstr(b, "is NOT bound") != nullptr, "install line, unbound with a null key");
    formatInstallLine(b, sizeof(b), false, true, nullptr, "edvr.ini");
    check(std::strcmp(b, "blend probe: off; hotkey.blend_probe is bound to ") == 0, "install line, a null key text does not crash");
    formatInstallLine(b, sizeof(b), true, false, "", "edvr.ini");
    check(std::strstr(b, "flat profile") != nullptr && std::strchr(b, '\n') == nullptr, "install line, flat");
    formatInstallLine(b, sizeof(b), false, false, "", "edvr.ini");
    check(std::strchr(b, '\n') == nullptr && std::strlen(b) < kLogMessageMax, "install line is one line and fits the log");
}

void testFrameLines() {
    char b[kRowBuf];
    FrameCounts c;
    c.gameFrame = 0xFFFFFFFFu;
    c.eyeDraws = c.eye0 = c.eye1 = c.eyeUnknown = 0xFFFFFFFFu;
    c.candidates = c.tested = c.covering = c.listed = c.truncated = 0xFFFFFFFFu;
    c.candidateOverflow = c.untestedState = c.indirect = c.unanswered = 0xFFFFFFFFu;
    const int n = formatFrameLine(b, sizeof(b), 2, c);
    check(n > 0 && static_cast<size_t>(n) < kLogMessageMax, "the frame line fits the log's line buffer");
    FrameCounts z;
    formatFrameLine(b, sizeof(b), 0, z);
    check(std::strstr(b, "0 eye draws seen") != nullptr && std::strstr(b, "0 rows listed") != nullptr,
          "a frame with nothing says zero draws seen and zero rows, in words");
    formatCapturedLine(b, sizeof(b), 3, 17, 2);
    check(std::strcmp(b, "blend probe: captured 3 frames, 17 rows (2 of 3 motion grids staged)") == 0, "the captured line");
    check(kMaxRowsPerFrame == 200 && kFrames == 3 && kWindow == 96 && kGrid == 8, "the contract's numbers: 3 frames, 200 rows, 96 pixels, 8x8");
}
}  // namespace

int main(int argc, char** argv) {
    bool selfTest = false;
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], "--self-test") == 0) selfTest = true;
    if (!selfTest) {
        std::printf("usage: blend_probe_test --self-test\n");
        return 2;
    }
    testWindow();
    testCandidate();
    testStateText();
    testMask();
    testRow();
    testRefusals();
    testGridIndex();
    testFlags();
    testMedianAndSummary();
    testGridText();
    testFrameLines();
    testInstallLine();
    std::printf("blend_probe_test: %u checks, %d failed\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
