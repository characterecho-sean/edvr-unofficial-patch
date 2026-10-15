#pragma once
// The blend probe's pure half (blend_probe.cpp; TEMPORARY, the instrument of docs/dlss-blur-elements-2026-10-14.md): the window
// the coverage test covers, which draws are rows, the row and grid text, the refusal reasons and the grid's indexing. No Windows,
// no D3D11, no engine memory. The rig tools\blend_probe_test compiles this header alone.
//
// What a press measures: for 3 consecutive frames, every eye draw that writes colour and is blend-enabled OR writes no depth is a
// candidate; a candidate whose rasterised samples reach the 96x96 window at the view centre (an occlusion query on a scissored
// duplicate of the draw that writes nothing) is a ROW. Per frame one 8x8 grid over the same window reads what the temporal pass
// decided for those pixels: depth, motion, the decision path, the engine-record kind, the coverage mask and the bias mask.
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace edvr::blend {

constexpr uint32_t kWindow = 96;             // the window's side, in render pixels
constexpr uint32_t kFrames = 3;              // consecutive frames one press captures
constexpr uint32_t kMaxRowsPerFrame = 200;   // rows listed per frame; the rest are counted as truncated
constexpr uint32_t kMaxCandidates = 2048;    // candidate draws tested per frame (one query each); the rest are counted
constexpr uint32_t kGrid = 8;                // the grid is kGrid x kGrid samples over the window
constexpr uint32_t kReadTicksMax = 120;      // frame boundaries the answers are waited for before the report is written anyway
// Log::note holds 1200 bytes a line: a 15-byte stamp, a 16-byte truncation marker and the line ending are taken from that. A row
// or a grid line stays under this, so no probe line ever reaches the marker.
constexpr size_t kLogMessageMax = 1100;
constexpr size_t kRowBuf = 1200;             // the buffer a line is formatted into (the rig checks the result against kLogMessageMax)

// ---- the window -------------------------------------------------------------------------------------------------------------

struct Rect {
    int32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;   // [x0, x1) x [y0, y1)
    uint32_t width() const noexcept { return x1 > x0 ? static_cast<uint32_t>(x1 - x0) : 0u; }
    uint32_t height() const noexcept { return y1 > y0 ? static_cast<uint32_t>(y1 - y0) : 0u; }
    uint32_t area() const noexcept { return width() * height(); }
};

// The kWindow-square window about the centre of a viewport, clipped to the render target. False when nothing is left (a target
// or viewport with no area, or a viewport whose centre lies outside the target).
inline bool windowRect(float vpX, float vpY, float vpW, float vpH, uint32_t rtW, uint32_t rtH, Rect* out) noexcept {
    if (!out || rtW == 0 || rtH == 0 || !(vpW > 0.0f) || !(vpH > 0.0f)) return false;
    const int32_t cx = static_cast<int32_t>(std::floor(vpX + vpW * 0.5f));
    const int32_t cy = static_cast<int32_t>(std::floor(vpY + vpH * 0.5f));
    constexpr int32_t half = static_cast<int32_t>(kWindow / 2);
    Rect r;
    r.x0 = std::max(cx - half, 0);
    r.y0 = std::max(cy - half, 0);
    r.x1 = std::min(cx + half, static_cast<int32_t>(rtW));
    r.y1 = std::min(cy + half, static_cast<int32_t>(rtH));
    if (r.area() == 0) return false;
    *out = r;
    return true;
}

// A draw is a candidate row when it writes colour and either blends or writes no depth. depthWrites is "a depth view is bound, the
// state enables depth and its write mask is not zero".
inline bool isCandidate(bool writesColour, bool blendEnabled, bool depthWrites) noexcept {
    return writesColour && (blendEnabled || !depthWrites);
}

// Coverage: -1 is "not measured" (the query never answered, or could not be made); anything else is the sample count.
constexpr int64_t kCoverageUnknown = -1;
inline bool coversWindow(int64_t samples) noexcept { return samples > 0; }

// ---- names ------------------------------------------------------------------------------------------------------------------

inline const char* blendFactorName(uint32_t v) noexcept {   // D3D11_BLEND
    switch (v) {
        case 1: return "ZERO";
        case 2: return "ONE";
        case 3: return "SRC_COLOR";
        case 4: return "INV_SRC_COLOR";
        case 5: return "SRC_ALPHA";
        case 6: return "INV_SRC_ALPHA";
        case 7: return "DEST_ALPHA";
        case 8: return "INV_DEST_ALPHA";
        case 9: return "DEST_COLOR";
        case 10: return "INV_DEST_COLOR";
        case 11: return "SRC_ALPHA_SAT";
        case 14: return "BLEND_FACTOR";
        case 15: return "INV_BLEND_FACTOR";
        case 16: return "SRC1_COLOR";
        case 17: return "INV_SRC1_COLOR";
        case 18: return "SRC1_ALPHA";
        case 19: return "INV_SRC1_ALPHA";
        default: return "?";
    }
}
inline const char* blendOpName(uint32_t v) noexcept {   // D3D11_BLEND_OP
    switch (v) {
        case 1: return "ADD";
        case 2: return "SUB";
        case 3: return "REVSUB";
        case 4: return "MIN";
        case 5: return "MAX";
        default: return "?";
    }
}
inline const char* compareName(uint32_t v) noexcept {   // D3D11_COMPARISON_FUNC
    switch (v) {
        case 1: return "NEVER";
        case 2: return "LESS";
        case 3: return "EQUAL";
        case 4: return "LESS_EQUAL";
        case 5: return "GREATER";
        case 6: return "NOT_EQUAL";
        case 7: return "GREATER_EQUAL";
        case 8: return "ALWAYS";
        default: return "?";
    }
}

struct BlendDesc {
    bool enable = false;
    uint32_t src = 2, dst = 1, op = 1, srcA = 2, dstA = 1, opA = 1;   // the D3D11 default: ONE, ZERO, ADD
    uint32_t writeMask = 0xF;
    bool alphaToCoverage = false;
};
struct DepthDesc {
    bool dsvBound = false;
    bool test = false;
    uint32_t func = 2;
    bool write = false;
    bool stencil = false;
};

// "off wm=F" for a draw that does not blend; "SRC_ALPHA,ONE,ADD/ONE,ONE,ADD wm=F" for one that does (colour, then alpha).
inline int formatBlend(char* out, size_t cap, const BlendDesc& b) noexcept {
    if (!b.enable)
        return std::snprintf(out, cap, "off wm=%X%s", b.writeMask & 0xFu, b.alphaToCoverage ? " a2c" : "");
    return std::snprintf(out, cap, "%s,%s,%s/%s,%s,%s wm=%X%s", blendFactorName(b.src), blendFactorName(b.dst),
                         blendOpName(b.op), blendFactorName(b.srcA), blendFactorName(b.dstA), blendOpName(b.opA),
                         b.writeMask & 0xFu, b.alphaToCoverage ? " a2c" : "");
}
// "test LESS_EQUAL write 0" / "test off write 0" / "no depth view".
inline int formatDepth(char* out, size_t cap, const DepthDesc& d) noexcept {
    if (!d.dsvBound) return std::snprintf(out, cap, "no depth view");
    if (!d.test) return std::snprintf(out, cap, "test off write %d%s", d.write ? 1 : 0, d.stencil ? " stencil" : "");
    return std::snprintf(out, cap, "test %s write %d%s", compareName(d.func), d.write ? 1 : 0, d.stencil ? " stencil" : "");
}
inline bool depthWrites(const DepthDesc& d) noexcept { return d.dsvBound && d.test && d.write; }

// ---- the reactive mask a coverage family writes (ui_depth.cpp floorBuffer) ------------------------------------------------------
// The mask's value is in quanta of 1/255. The constant buffer carries y (the value for a stroke drawn AT a surface, "attached") and
// w (for one that floats over the scene). Slot 0 (the interface proper) and 4 (the scanner's chrome): both 1. Slots 1 (the holo
// material and the sprite) and 2 (the flight HUD): floating 1, attached 2. Slot 3 (the smoke): one quantum, odd (its shader
// quantises; the buffer's w is 0). The parity is the temporal pass's word on the body's path: even rides a turning body, odd keeps
// the camera's.
inline void maskQuanta(int slot, int* floating, int* attached) noexcept {
    int f = 1, a = 1;
    if (slot == 1 || slot == 2) a = 2;
    if (floating) *floating = f;
    if (attached) *attached = a;
}
inline const char* maskSlotName(int slot) noexcept {
    switch (slot) {
        case 0: return "interface";
        case 1: return "holo-material";
        case 2: return "flight-hud";
        case 3: return "smoke";
        case 4: return "scanner-chrome";
        default: return "?";
    }
}

// ---- the row ----------------------------------------------------------------------------------------------------------------------

struct Row {
    uint32_t slot = 0;          // captured frame 0..kFrames-1
    uint32_t gameFrame = 0;     // vScreen's frame number
    uint32_t ordinal = 0;       // the draw's place among the frame's eye draws (both eyes counted)
    int eye = -1;               // 0, 1, or -1 when the pass's table cannot tell
    uint64_t vs = 0, ps = 0;
    char drawKind = '?';        // D, I, N, X
    uint32_t count = 0, instances = 1;
    char blend[96] = {};
    char depth[48] = {};
    char rtFormat[24] = {};
    uint32_t rtW = 0, rtH = 0;
    int64_t covered = kCoverageUnknown;
    char verdict[16] = {};      // EDVR's draw verdict
    char uiFamily[24] = {};     // the UI layer's family for the draw, "-" when it named none
    char uiDecision[16] = {};   // taken | left:<reason> | none (no decision ran)
    char holo[12] = {};         // the hologram pass's list: cockpit | world | -
    char coverage[40] = {};     // ui-depth coverage: "-" or "<slot name> m<floating>/<attached>"
    char keyed[28] = {};        // the engine-motion kFamilies pair: "-" or "rigid" | "skinned" | "self-marking" with the family index
    char engine[12] = {};       // sub | plain: the pool shaders were substituted for this draw
};

// One row as a log line. The caller prefixes nothing; the text starts "blend probe row:". Returns the length, or -1 if the text
// did not fit `cap` (the rig pins that a worst-case row fits kLogMessageMax).
inline int formatRow(char* out, size_t cap, const Row& r) noexcept {
    char cov[24];
    if (r.covered < 0) std::snprintf(cov, sizeof(cov), "?");
    else std::snprintf(cov, sizeof(cov), "%lld", static_cast<long long>(r.covered));
    const int n = std::snprintf(out, cap,
        "blend probe row: f=%u gf=%u #%u eye=%d vs=%016llX ps=%016llX %c n=%u inst=%u blend=[%s] depth=[%s] rt=%s %ux%u "
        "cov=%s verdict=%s ui=%s/%s holo=%s mask=%s kf=%s eng=%s",
        r.slot, r.gameFrame, r.ordinal, r.eye, static_cast<unsigned long long>(r.vs), static_cast<unsigned long long>(r.ps),
        r.drawKind, r.count, r.instances, r.blend, r.depth, r.rtFormat, r.rtW, r.rtH, cov, r.verdict, r.uiFamily, r.uiDecision,
        r.holo, r.coverage, r.keyed, r.engine);
    if (n < 0 || static_cast<size_t>(n) >= cap) return -1;
    return n;
}

// ---- refusals ---------------------------------------------------------------------------------------------------------------------

enum class Refusal : uint8_t {
    kNone = 0,
    kNotVr,              // the flat profile has no eye passes
    kBusy,               // a capture is already running
    kNoHooks,            // the draw hooks are not live
    kNoEyePass,          // no draw into an eye texture in the captured frames
    kQueriesUnavailable, // occlusion queries could not be made
    kQueriesUnanswered,  // some queries had not answered when the report was written
    kNoGrid,             // the temporal pass staged no grid
    kCount
};
inline const char* refusalText(Refusal r) noexcept {
    switch (r) {
        case Refusal::kNotVr:
            return "not VR: the probe reads the VR eye passes and the flat profile has none";
        case Refusal::kBusy:
            return "a capture is already running; press again once it has reported";
        case Refusal::kNoHooks:
            return "the draw hooks are not live (the graphics half has not installed its fixes), so no draw can be offered";
        case Refusal::kNoEyePass:
            return "no eye pass seen: not one draw into an eye texture reached the probe in the captured frames (a menu, a loading "
                   "screen, no VR session, or the draw path was off)";
        case Refusal::kQueriesUnavailable:
            return "occlusion queries unavailable: the device refused to make one, so no coverage was measured and every row shows "
                   "cov=?";
        case Refusal::kQueriesUnanswered:
            return "some coverage queries had not answered by the time the report was written; those rows show cov=?";
        case Refusal::kNoGrid:
            return "no motion grid: the temporal pass staged none in the captured frames (fix.temporal_aa is off, the upscaler "
                   "path is not the NVIDIA one, or the pass never reached eye 0)";
        default: return "";
    }
}

// ---- the grid ---------------------------------------------------------------------------------------------------------------------

// The pixel, in window coordinates, of grid column or row `index` of `n` over an `extent`-pixel side: the middle of the cell. Always
// inside the extent when extent > 0.
inline uint32_t gridCoord(uint32_t index, uint32_t extent, uint32_t n = kGrid) noexcept {
    if (extent == 0 || n == 0) return 0;
    if (index >= n) index = n - 1;
    const uint64_t v = (static_cast<uint64_t>(index) * extent + extent / 2) / n;
    return static_cast<uint32_t>(v < extent ? v : extent - 1);
}
// The linear index of a cell in a row-major array of kGrid x kGrid.
inline uint32_t gridCell(uint32_t col, uint32_t row) noexcept { return row * kGrid + col; }

// What the decision texture's w word holds (temporal_shader_source.h, EDVR_TEMPORAL_TRACE): the integer flags as a float.
//   bits 0..3   the path the pixel's motion took: 1 camera on the world, 2 camera on the ship, 8 cockpit hologram, 10 screen,
//               11 engine record, 12 planet patch
//   bit 4       hidden: NVIDIA is told to start this pixel's history afresh
//   bit 6       a UI pixel by the coverage mask
//   bits 12..14 the engine-record kind (0 none, 1 exact motion, 2 masked, 4 stale slot, 5 bad slot code, 6 stale pose)
struct DecisionFlags {
    uint32_t path = 0;
    bool hidden = false, ui = false;
    uint32_t engineKind = 0;
};
inline DecisionFlags decodeFlags(float w) noexcept {
    DecisionFlags f;
    const uint32_t bits = (w >= 0.0f && w < 16777216.0f) ? static_cast<uint32_t>(w + 0.5f) : 0u;
    f.path = bits & 15u;
    f.hidden = (bits & 16u) != 0;
    f.ui = (bits & 64u) != 0;
    f.engineKind = (bits >> 12) & 7u;
    return f;
}
inline const char* engineKindName(uint32_t k) noexcept {
    switch (k) {
        case 0: return "none";
        case 1: return "exact";
        case 2: return "masked";
        case 4: return "stale-slot";
        case 5: return "bad-code";
        case 6: return "stale-pose";
        default: return "other";
    }
}
// Paths that are the camera term alone (no per-object source): 1 and 2.
inline bool isCameraPath(uint32_t path) noexcept { return path == 1 || path == 2; }

struct GridSample {
    bool valid = false;         // the decision texture was read for this cell
    float z = 0.0f;             // the pass's depth copy (raw; 0 is the far plane); NAN when that input was not staged
    float mvx = 0.0f, mvy = 0.0f;   // the pass's physical motion in render pixels (before the hidden-history sentinel)
    DecisionFlags flags;
    int coverage = -1;          // the coverage mask byte, -1 when not staged
    int bias = -1;              // the bias mask byte, -1 when not staged
};

// One cell: "z=0.84210 mv=-1.20,0.31 p=1 e=0 c=0 b=0" plus H (hidden) and U (UI) after the path. "-" for a cell that was not read.
inline int formatCell(char* out, size_t cap, const GridSample& s) noexcept {
    if (!s.valid) return std::snprintf(out, cap, "-");
    char z[16], c[8], b[8];
    if (std::isnan(s.z)) std::snprintf(z, sizeof(z), "-");
    else std::snprintf(z, sizeof(z), "%.5f", static_cast<double>(s.z));
    if (s.coverage < 0) std::snprintf(c, sizeof(c), "-");
    else std::snprintf(c, sizeof(c), "%d", s.coverage);
    if (s.bias < 0) std::snprintf(b, sizeof(b), "-");
    else std::snprintf(b, sizeof(b), "%d", s.bias);
    return std::snprintf(out, cap, "z=%s mv=%.2f,%.2f p=%u%s%s e=%u c=%s b=%s", z, static_cast<double>(s.mvx),
                         static_cast<double>(s.mvy), s.flags.path, s.flags.hidden ? "H" : "", s.flags.ui ? "U" : "",
                         s.flags.engineKind, c, b);
}

// One grid row as a log line: "blend probe grid f=0 row 3: [..] | [..] ..." (eight cells). Returns -1 if it did not fit.
inline int formatGridRow(char* out, size_t cap, uint32_t slot, uint32_t row, const GridSample* cells) noexcept {
    int n = std::snprintf(out, cap, "blend probe grid f=%u row %u:", slot, row);
    if (n < 0 || static_cast<size_t>(n) >= cap) return -1;
    for (uint32_t i = 0; i < kGrid; ++i) {
        char cell[96];
        formatCell(cell, sizeof(cell), cells[gridCell(i, row)]);
        const int m = std::snprintf(out + n, cap - static_cast<size_t>(n), " [%s]", cell);
        if (m < 0 || static_cast<size_t>(n) + static_cast<size_t>(m) >= cap) return -1;
        n += m;
    }
    return n;
}

inline float median(float* v, size_t n) noexcept {
    if (n == 0) return 0.0f;
    std::sort(v, v + n);
    if (n % 2) return v[n / 2];
    return 0.5f * (v[n / 2 - 1] + v[n / 2]);
}

struct GridSummary {
    uint32_t cells = 0;                 // cells read
    uint32_t cameraCells = 0, engineCells = 0, otherCells = 0;   // by the path the motion took (otherCells: holo, screen, planet)
    float medAll[2] = {}, medCamera[2] = {}, medEngine[2] = {};   // medians of the motion, per axis
    uint32_t kindCells[8] = {};         // engine-record kind per cell
    uint32_t hidden = 0, ui = 0, coverageSet = 0, biasSet = 0;
    float medianDelta = 0.0f;           // |engine median - camera median| in pixels when both exist, else 0
    bool haveDelta = false;
};

inline GridSummary summarize(const GridSample* cells, size_t n) noexcept {
    GridSummary s;
    float allX[kGrid * kGrid], allY[kGrid * kGrid], camX[kGrid * kGrid], camY[kGrid * kGrid], engX[kGrid * kGrid],
        engY[kGrid * kGrid];
    size_t na = 0, nc = 0, ne = 0;
    for (size_t i = 0; i < n && i < kGrid * kGrid; ++i) {
        const GridSample& g = cells[i];
        if (!g.valid) continue;
        ++s.cells;
        allX[na] = g.mvx;
        allY[na] = g.mvy;
        ++na;
        if (isCameraPath(g.flags.path)) {
            ++s.cameraCells;
            camX[nc] = g.mvx;
            camY[nc] = g.mvy;
            ++nc;
        } else if (g.flags.path == 11) {
            ++s.engineCells;
            engX[ne] = g.mvx;
            engY[ne] = g.mvy;
            ++ne;
        } else {
            ++s.otherCells;
        }
        ++s.kindCells[g.flags.engineKind & 7u];
        if (g.flags.hidden) ++s.hidden;
        if (g.flags.ui) ++s.ui;
        if (g.coverage > 0) ++s.coverageSet;
        if (g.bias > 0) ++s.biasSet;
    }
    s.medAll[0] = median(allX, na);
    s.medAll[1] = median(allY, na);
    s.medCamera[0] = median(camX, nc);
    s.medCamera[1] = median(camY, nc);
    s.medEngine[0] = median(engX, ne);
    s.medEngine[1] = median(engY, ne);
    if (nc > 0 && ne > 0) {
        const float dx = s.medEngine[0] - s.medCamera[0], dy = s.medEngine[1] - s.medCamera[1];
        s.medianDelta = std::sqrt(dx * dx + dy * dy);
        s.haveDelta = true;
    }
    return s;
}

// The rotation angle, in degrees, of a rotation matrix from its trace (the pass's delta rotation dR0..2, one row each).
inline float rotationDegrees(float trace) noexcept {
    float c = (trace - 1.0f) * 0.5f;
    if (c > 1.0f) c = 1.0f;
    if (c < -1.0f) c = -1.0f;
    return std::acos(c) * 57.29578f;
}

struct GridHeader {
    uint32_t slot = 0, gameFrame = 0, sceneFrame = 0;
    uint32_t winX = 0, winY = 0, winW = 0, winH = 0, inputW = 0, inputH = 0;
    float rotDeg = 0.0f, transLen = 0.0f;
    bool transValid = false;
    const char* inputs = "";    // which inputs were staged: "decision depth coverage bias" with a - before a missing one
};

// The grid's first line and its summary line. Two lines, so each stays well inside the log's width.
inline int formatGridHead(char* out, size_t cap, const GridHeader& h) noexcept {
    return std::snprintf(out, cap,
        "blend probe grid f=%u: game frame %u, temporal pass frame %u; window x=%u y=%u %ux%u of the %ux%u input (8x8 cell centres); "
        "the pass's camera term this frame: rotation %.4f deg, translation %.4f (%s); inputs %s",
        h.slot, h.gameFrame, h.sceneFrame, h.winX, h.winY, h.winW, h.winH, h.inputW, h.inputH,
        static_cast<double>(h.rotDeg), static_cast<double>(h.transLen), h.transValid ? "world path on" : "world path off",
        h.inputs);
}
// Said once per press, before the first grid: what the cell tokens are.
inline const char* gridLegend() noexcept {
    return "blend probe grid legend: z = the pass's raw depth copy (0 = far); mv = physical motion in input pixels (x,y); p = the path "
           "the motion took (1 camera/world, 2 camera/ship, 8 cockpit hologram, 10 screen, 11 engine record, 12 planet patch) with "
           "H = NVIDIA's history hidden here and U = a UI pixel; e = engine-record kind (0 none, 1 exact, 2 masked, 4 stale slot, "
           "5 bad code, 6 stale pose); c = coverage mask byte (low 2 bits the kind, the rest the fixed bias); b = bias mask byte; "
           "'-' = that input was not staged";
}
inline int formatGridSummary(char* out, size_t cap, uint32_t slot, const GridSummary& s) noexcept {
    char delta[48];
    if (s.haveDelta) std::snprintf(delta, sizeof(delta), "%.3f px", static_cast<double>(s.medianDelta));
    else std::snprintf(delta, sizeof(delta), "n/a (needs both paths)");
    return std::snprintf(out, cap,
        "blend probe grid f=%u summary: %u of 64 cells read; median motion all=(%.2f,%.2f); camera-path cells %u median=(%.2f,%.2f); "
        "engine-path cells %u median=(%.2f,%.2f); other-path cells %u; engine minus camera median %s; engine kinds none=%u exact=%u "
        "masked=%u stale-slot=%u bad-code=%u stale-pose=%u other=%u; hidden=%u ui=%u coverage-set=%u bias-set=%u",
        slot, s.cells, static_cast<double>(s.medAll[0]), static_cast<double>(s.medAll[1]), s.cameraCells,
        static_cast<double>(s.medCamera[0]), static_cast<double>(s.medCamera[1]), s.engineCells,
        static_cast<double>(s.medEngine[0]), static_cast<double>(s.medEngine[1]), s.otherCells, delta, s.kindCells[0],
        s.kindCells[1], s.kindCells[2], s.kindCells[4], s.kindCells[5], s.kindCells[6],
        s.kindCells[3] + s.kindCells[7], s.hidden, s.ui, s.coverageSet, s.biasSet);
}

// ---- the install line ---------------------------------------------------------------------------------------------------------------
// One line at launch, saying whether the key is bound, so an unbound probe shows then and not after a flight (the 2026-10-14 flight had
// no key in the live ini, which install_edvr.py leaves alone). The wording is pinned by the rig.
// iniName is Config::iniName() (a message spells the settings file through it, never as a literal; config_test pins that).
inline int formatInstallLine(char* out, size_t cap, bool flatProfile, bool bound, const char* key, const char* iniName) noexcept {
    if (flatProfile) return std::snprintf(out, cap, "blend probe: off; VR only, hotkey.blend_probe does nothing in the flat profile");
    if (bound) return std::snprintf(out, cap, "blend probe: off; hotkey.blend_probe is bound to %s", key ? key : "");
    return std::snprintf(out, cap,
                         "blend probe: off; hotkey.blend_probe is NOT bound -- add \"blend_probe = <a free key>\" under [hotkey] in %s to use it",
                         iniName ? iniName : "the settings file");
}

// ---- the report lines -------------------------------------------------------------------------------------------------------------

struct FrameCounts {
    uint32_t gameFrame = 0;
    uint32_t eyeDraws = 0, eye0 = 0, eye1 = 0, eyeUnknown = 0;
    uint32_t candidates = 0, tested = 0, covering = 0, listed = 0, truncated = 0;
    uint32_t candidateOverflow = 0;     // candidates beyond kMaxCandidates, never tested
    uint32_t untestedState = 0;         // candidates whose state could not be read or whose replay was declined (UAV, stream output, bad target)
    uint32_t indirect = 0;              // indirect draws seen: not replayable, not tested
    uint32_t unanswered = 0;            // tested draws whose query had not answered
};
inline int formatFrameLine(char* out, size_t cap, uint32_t slot, const FrameCounts& c) noexcept {
    return std::snprintf(out, cap,
        "blend probe: frame f=%u (game frame %u): %u eye draws seen (eye0 %u, eye1 %u, eye unknown %u); %u candidates (colour-writing "
        "and blending or not writing depth), %u tested, %u cover the window, %u rows listed, truncated %u; not tested: %u over the "
        "%u-per-frame cap, %u whose state could not be replayed, %u indirect draws; %u answers missing",
        slot, c.gameFrame, c.eyeDraws, c.eye0, c.eye1, c.eyeUnknown, c.candidates, c.tested, c.covering, c.listed, c.truncated,
        c.candidateOverflow, kMaxCandidates, c.untestedState, c.indirect, c.unanswered);
}
inline int formatCapturedLine(char* out, size_t cap, uint32_t frames, uint32_t rows, uint32_t grids) noexcept {
    return std::snprintf(out, cap, "blend probe: captured %u frames, %u rows (%u of %u motion grids staged)", frames, rows, grids,
                         frames);
}

}  // namespace edvr::blend
