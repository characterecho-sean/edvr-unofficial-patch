// Output-identity rig for the UI resolve (src/d3d11/ui_resolve.h), on WARP.
//
// The UI resolve is the post-DLSS pass that bounds NVIDIA's output against the
// frame's own raster wherever the interface is (and, at fix.temporal_aa's
// corona level, holds faint flat glow anywhere). A change to it that is only
// about WHERE its data comes from -- today, not fetching an input the caller
// left unbound -- must leave every output byte and every history byte exactly
// where they were. This rig is that proof, four ways:
//
//   goldens   FNV-1a 64 hashes of the output texture and the history the
//             unmodified shader wrote for fixed fixtures, recorded before the
//             change existed (--print-goldens, from the frozen reference).
//             They are the "before". To re-record after a legitimate change,
//             check out the commit before it and run --print-goldens; never
//             paste this build's output over a failure. Both the frozen
//             reference and the production bytecode must reproduce them.
//   pairs     the same inputs through the frozen reference (ui_resolve_
//             reference.h, the shader as it was) and the production bytecode,
//             compared byte for byte, on every fixture and on random fuzz
//             fixtures (sizes, ratios, jitter -- wild included -- marks,
//             history, motion, the screen map). The bits that say which inputs
//             are unbound (b1.z) are derived from the bindings, as
//             temporal_pass.cpp derives them. Then each input in turn is left
//             unbound with the rest bound: that must equal the reference over
//             a texture of zeros in its place.
//   controls  bits of zero over inputs that really are unbound must still be
//             the reference (a caller that says nothing is slow, not wrong);
//             and claiming a bound input unbound must FAIL -- for all three at
//             once on every fixture that has content, and for each bit on the
//             named fixtures built to need it -- so a green run means the
//             fixtures are sensitive to the very thing the bits hide.
//   trace     the D3D11 debug layer reports nothing, and a stale bytecode
//             header (build\gen older than ui_resolve.h) fails the run.
//
// Runs on WARP by default (the build gate). --adapter nvidia runs the pairs and
// controls on the RTX (a desk check, never part of the build); --bench times
// the reference against the production shader at the eye's real size there,
// with the bindings of the carrier flight (2026-09-29).
//
//   ui_holo_pass_test --self-test | --dry-run | --print-goldens | --stats | --bench
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "../../src/d3d11/fixed_shader_source.h"   // kUiLayerCompositeHlsl, for --bench-composite
#include "../../src/d3d11/ui_resolve.h"
#include "temporal_shader_bytecode.h"  // edvr::kUiResolveBytecode, from build\gen
#include "ui_resolve_reference.h"      // edvr_reference::kUiResolveReference, the shader as it was

using Microsoft::WRL::ComPtr;

namespace {

int g_checks = 0, g_failures = 0;
void check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("FAIL: %s\n", what);
    }
}
void hr(HRESULT r, const char* what) {
    if (FAILED(r)) {
        std::printf("FAIL: %s (0x%08lX)\n", what, static_cast<unsigned long>(r));
        std::exit(1);
    }
}

// ------------------------------------------------------------------ inputs

struct Rng {
    uint32_t s;
    explicit Rng(uint32_t seed) : s(seed ? seed : 0x9E3779B9u) {}
    uint32_t next() {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return s;
    }
    int range(int n) { return static_cast<int>(next() % static_cast<uint32_t>(n)); }
};

uint16_t toHalf(float f) {
    uint32_t x;
    std::memcpy(&x, &f, 4);
    const uint32_t sign = (x >> 16) & 0x8000u;
    const uint32_t mant = x & 0x7FFFFFu;
    const int32_t e8 = static_cast<int32_t>((x >> 23) & 0xFFu);
    if (e8 == 0xFF) return static_cast<uint16_t>(sign | 0x7C00u | (mant ? 0x200u : 0u));
    int32_t exp = e8 - 127 + 15;
    if (exp >= 31) return static_cast<uint16_t>(sign | 0x7C00u);
    if (exp <= 0) {
        if (exp < -10) return static_cast<uint16_t>(sign);
        const uint32_t m = mant | 0x800000u;
        const uint32_t shift = static_cast<uint32_t>(14 - exp);
        uint32_t half = m >> shift;
        const uint32_t rem = m & ((1u << shift) - 1u), mid = 1u << (shift - 1);
        if (rem > mid || (rem == mid && (half & 1u))) ++half;
        return static_cast<uint16_t>(sign | half);
    }
    uint32_t half = (static_cast<uint32_t>(exp) << 10) | (mant >> 13);
    const uint32_t rem = mant & 0x1FFFu;
    if (rem > 0x1000u || (rem == 0x1000u && (half & 1u))) ++half;
    return static_cast<uint16_t>(sign | half);
}

// One fixture: the sizes, the jitter, b1, and which inputs exist and how they
// are filled. Every fill is a pure function of these fields (and the seed).
struct Fix {
    std::string name;
    uint32_t seed;
    int w, h, ow, oh;   // the frame's raster (input) size and the output size
    float jx, jy;       // the jitter, in input pixels
    float tol, hold;    // b1: the clamp's tolerance and the hold's limit; tol < 0 leaves b1 unbound
    int content;        // 0 faint sky with stars, 1 bands, 2 bright interior, 3 mixed quadrants
    int marks;          // 0 none (coverage and edits unbound), 1 corner, 2 tile edges, 3 corona edge, 4 whole frame,
                        // 5 screen, 6 mixed, 7 sparse scatter, 8 random rectangles, 9 a HUD's worth of panels (bench),
                        // 10 coverage and edits bound but empty (bench)
    int hist;           // 0 no history bound, 1 sparse influence, 2 dense influence, 3 bound and all zero (bench)
    int motion;         // 0 zero, 1 small, 2 large, 3 hostile (NaN, Inf, huge)
    int rx, ry;         // the screen map's offset of the frame's region
    int sx, sy;         // the screen map's extra size beyond the frame; sx < 0: no screen map
};

struct Inputs {
    std::vector<uint8_t> raw, trained, cover, edits, hist;
    std::vector<uint16_t> screen, motion;
    int screenW = 0, screenH = 0;
};

const Fix kFix[] = {
    // name, seed, w,h, ow,oh, jx,jy, tol,hold, content,marks,hist,motion, rx,ry, sx,sy
    {"sky_no_ui_2x", 11, 61, 47, 122, 94, 0.25f, -0.40f, 12 / 255.f, 64 / 255.f, 0, 0, 0, 0, 0, 0, -1, 0},
    {"sky_hold_off_b1_unbound", 12, 61, 47, 122, 94, 0.25f, -0.40f, -1.f, 0.f, 0, 0, 0, 0, 0, 0, -1, 0},
    {"sky_tol_zero_hold_on", 13, 61, 47, 122, 94, -0.10f, 0.30f, 0.f, 64 / 255.f, 0, 0, 0, 0, 0, 0, -1, 0},
    {"corner_ui", 21, 64, 64, 128, 128, 0.f, 0.f, 12 / 255.f, 64 / 255.f, 3, 1, 0, 0, 0, 0, -1, 0},
    {"corner_ui_jit_edge", 22, 64, 64, 128, 128, 0.5f, -0.5f, 12 / 255.f, 64 / 255.f, 3, 1, 0, 0, 0, 0, -1, 0},
    {"tile_edges_ui", 31, 96, 80, 192, 160, -0.3f, 0.2f, 12 / 255.f, 64 / 255.f, 3, 2, 0, 0, 0, 0, -1, 0},
    {"tile_edges_ui_odd", 32, 61, 47, 122, 94, 0.5f, 0.5f, 12 / 255.f, 64 / 255.f, 1, 2, 0, 0, 0, 0, -1, 0},
    {"corona_edge", 41, 64, 64, 128, 128, 0.1f, 0.1f, 12 / 255.f, 64 / 255.f, 0, 3, 0, 0, 0, 0, -1, 0},
    {"corona_edge_15x", 42, 64, 64, 96, 96, -0.2f, 0.4f, 12 / 255.f, 64 / 255.f, 0, 3, 0, 0, 0, 0, -1, 0},
    {"menu_full_frame", 51, 61, 47, 122, 94, 0.f, 0.f, 12 / 255.f, 64 / 255.f, 2, 4, 0, 0, 0, 0, -1, 0},
    {"screen_ui", 52, 61, 47, 122, 94, 0.2f, -0.2f, 12 / 255.f, 64 / 255.f, 3, 5, 0, 0, 3, 2, 6, 4},
    {"screen_mixed_region_max", 53, 64, 48, 128, 96, 0.f, 0.3f, 12 / 255.f, 64 / 255.f, 3, 6, 0, 0, 8, 8, 8, 8},
    {"history_sparse_small_motion", 61, 61, 47, 122, 94, 0.25f, -0.4f, 12 / 255.f, 64 / 255.f, 3, 0, 1, 1, 0, 0, -1, 0},
    {"history_dense_large_motion", 62, 61, 47, 122, 94, 0.f, 0.f, 12 / 255.f, 64 / 255.f, 0, 0, 2, 2, 0, 0, -1, 0},
    {"history_hostile_motion", 63, 61, 47, 122, 94, 0.1f, 0.1f, 12 / 255.f, 64 / 255.f, 3, 0, 2, 3, 0, 0, -1, 0},
    {"history_with_ui", 64, 64, 64, 128, 128, 0.25f, 0.25f, 12 / 255.f, 64 / 255.f, 3, 6, 1, 2, 0, 0, -1, 0},
    {"ratio_1x", 71, 33, 17, 33, 17, 0.f, 0.f, 12 / 255.f, 64 / 255.f, 3, 1, 1, 1, 0, 0, -1, 0},
    {"ratio_133", 72, 33, 17, 44, 23, 0.f, 0.f, 12 / 255.f, 64 / 255.f, 3, 1, 1, 1, 0, 0, -1, 0},
    {"ratio_25", 73, 33, 17, 83, 43, 0.25f, 0.25f, 12 / 255.f, 64 / 255.f, 3, 6, 1, 1, 0, 0, -1, 0},
    {"ratio_075_down", 74, 33, 17, 25, 13, 0.f, 0.f, 12 / 255.f, 64 / 255.f, 3, 1, 1, 1, 0, 0, -1, 0},
    {"jitter_1_3", 81, 64, 64, 128, 128, 1.3f, -1.3f, 12 / 255.f, 64 / 255.f, 3, 6, 1, 1, 0, 0, -1, 0},
    {"jitter_0_9", 82, 64, 64, 128, 128, 0.9f, 0.9f, 12 / 255.f, 64 / 255.f, 3, 6, 1, 1, 0, 0, -1, 0},
    {"jitter_neg_1_7", 83, 64, 64, 128, 128, -1.7f, 0.2f, 12 / 255.f, 64 / 255.f, 0, 3, 1, 1, 0, 0, -1, 0},
    {"tiny_7x5", 91, 7, 5, 14, 10, 0.f, 0.f, 12 / 255.f, 64 / 255.f, 3, 1, 1, 1, 0, 0, -1, 0},
    {"wide_130x9", 92, 130, 9, 260, 18, 0.25f, 0.f, 12 / 255.f, 64 / 255.f, 3, 2, 1, 1, 0, 0, -1, 0},
    {"tall_9x130", 93, 9, 130, 18, 260, 0.f, -0.25f, 12 / 255.f, 64 / 255.f, 3, 2, 1, 1, 0, 0, -1, 0},
    {"eye_left", 101, 80, 72, 160, 144, 0.25f, -0.4f, 12 / 255.f, 64 / 255.f, 3, 6, 1, 1, 0, 0, -1, 0},
    {"eye_right", 102, 80, 72, 160, 144, -0.25f, 0.4f, 12 / 255.f, 64 / 255.f, 3, 6, 1, 1, 0, 0, -1, 0},
    {"production_shape_eighth", 111, 252, 244, 504, 488, 0.1f, -0.1f, 12 / 255.f, 64 / 255.f, 3, 6, 1, 1, 0, 0, -1, 0},
};
constexpr size_t kFixtures = sizeof(kFix) / sizeof(kFix[0]);

// The faint-sky, star and band content, all integer arithmetic.
void fillRaw(const Fix& f, Rng& rng, std::vector<uint8_t>& raw) {
    raw.assign(static_cast<size_t>(f.w) * f.h * 4, 255);
    for (int y = 0; y < f.h; ++y) {
        for (int x = 0; x < f.w; ++x) {
            int style = f.content;
            if (style == 3) style = ((x >= f.w / 2) ? 1 : 0) + ((y >= f.h / 2) ? 2 : 0);   // quadrants 0,1,2,3
            uint8_t* p = &raw[(static_cast<size_t>(y) * f.w + x) * 4];
            for (int c = 0; c < 3; ++c) {
                int v;
                switch (style) {
                    case 0: v = 1 + rng.range(4); break;                                   // faint sky
                    case 1: v = ((x * 3 + y * 2 + c * 5) / 4) % 80; break;               // slow bands, faint and not
                    case 2: v = 60 + rng.range(196); break;                                // bright interior
                    default: v = (x < f.w / 4) ? 1 + rng.range(3) : 20 + rng.range(60); break;
                }
                p[c] = static_cast<uint8_t>(v);
            }
        }
    }
    // Stars: a hot core with a halo that falls off through the faint range,
    // so the hold's brightness limit and flatness edge both cross real pixels.
    const int stars = std::max(1, f.w * f.h / 900);
    for (int s = 0; s <= stars; ++s) {
        const int cx = s == 0 ? f.w / 2 : rng.range(f.w), cy = s == 0 ? f.h / 2 : rng.range(f.h);
        for (int dy = -4; dy <= 4; ++dy) {
            for (int dx = -4; dx <= 4; ++dx) {
                const int x = cx + dx, y = cy + dy;
                if (x < 0 || y < 0 || x >= f.w || y >= f.h) continue;
                const int v = 255 - 40 * (dx * dx + dy * dy);
                if (v <= 0) continue;
                uint8_t* p = &raw[(static_cast<size_t>(y) * f.w + x) * 4];
                for (int c = 0; c < 3; ++c) p[c] = static_cast<uint8_t>(std::max<int>(p[c], v - c));
            }
        }
    }
}

void fillTrained(const Fix& f, Rng& rng, const std::vector<uint8_t>& raw, std::vector<uint8_t>& out) {
    out.assign(static_cast<size_t>(f.ow) * f.oh * 4, 0);
    for (int y = 0; y < f.oh; ++y) {
        for (int x = 0; x < f.ow; ++x) {
            const int qx = std::min(f.w - 1, x * f.w / f.ow), qy = std::min(f.h - 1, y * f.h / f.oh);
            const uint8_t* s = &raw[(static_cast<size_t>(qy) * f.w + qx) * 4];
            uint8_t* d = &out[(static_cast<size_t>(y) * f.ow + x) * 4];
            const bool big = rng.range(16) == 0;
            for (int c = 0; c < 3; ++c) {
                int v = s[c] + (rng.range(9) - 3) + (big ? (rng.range(2) ? 20 + rng.range(40) : -(20 + rng.range(40))) : 0);
                d[c] = static_cast<uint8_t>(std::min(255, std::max(0, v)));
            }
            d[3] = (rng.next() & 1) ? 255 : 127;
        }
    }
}

void fillMarks(const Fix& f, Rng& rng, Inputs& in) {
    const bool wantCover = f.marks != 0, wantEdits = f.marks != 0;
    if (wantCover) in.cover.assign(static_cast<size_t>(f.w) * f.h, 0);
    if (wantEdits) in.edits.assign(static_cast<size_t>(f.w) * f.h, 0);
    auto cov = [&](int x, int y, uint8_t v) {
        if (x >= 0 && y >= 0 && x < f.w && y < f.h && !in.cover.empty()) in.cover[static_cast<size_t>(y) * f.w + x] = v;
    };
    auto edi = [&](int x, int y, uint8_t v) {
        if (x >= 0 && y >= 0 && x < f.w && y < f.h && !in.edits.empty()) in.edits[static_cast<size_t>(y) * f.w + x] = v;
    };
    auto scr = [&](int x, int y, int w) {
        if (in.screen.empty()) return;
        const int sx = f.rx + x, sy = f.ry + y;
        if (sx < 0 || sy < 0 || sx >= in.screenW || sy >= in.screenH) return;
        in.screen[(static_cast<size_t>(sy) * in.screenW + sx) * 4 + 3] = toHalf(static_cast<float>(w));
    };
    const int m = f.marks;
    if (m == 1 || m == 6) {
        for (int y = 0; y <= 4; ++y) for (int x = 0; x <= 5; ++x) cov(x, y, (x + y) & 1 ? 1 : 2);
        for (int y = 1; y <= 3; ++y) for (int x = 1; x <= 3; ++x) edi(x, y, 255);
        cov(10, 10, 3);                 // smoke: not marked
        cov(11, 10, 5);                 // floating (k=1) with high bits set
        cov(12, 10, 10);                // attached (k=2) with high bits set
        cov(13, 10, 4);                 // k=0
        edi(9, 9, 1);                   // the smallest edit
        cov(f.w - 1, f.h - 1, 1);       // the far corner
        edi(f.w - 2, f.h - 1, 128);
    }
    if (m == 2 || m == 6) {
        for (int y = 3; y <= 20; ++y) { cov(7, y, 1); cov(8, y, 2); }
        for (int x = 20; x <= 40; ++x) { cov(x, 15, 1); cov(x, 16, 1); }
        for (int y = 0; y <= 30; ++y) edi(23, y, 255);
        for (int x = 0; x < f.w; ++x) edi(x, 31, 64);
        for (int x = 15; x <= 17; ++x) cov(x, f.h / 2, 2);
    }
    if (m == 3) {
        cov(f.w / 2 + 5, f.h / 2, 1);
        cov(f.w / 2 + 6, f.h / 2 + 1, 2);
        edi(f.w / 2 - 5, f.h / 2 + 1, 255);
        cov(f.w / 2, f.h / 2 - 6, 1);
    }
    if (m == 4) {
        for (int y = 0; y < f.h; ++y) for (int x = 0; x < f.w; ++x) {
            cov(x, y, static_cast<uint8_t>((rng.range(5) == 0) ? 3 : (1 + (rng.range(2)) + 4 * rng.range(4))));
            if (rng.range(3) == 0) edi(x, y, static_cast<uint8_t>(1 + rng.range(255)));
        }
    }
    if (m == 5 || m == 6) {
        for (int y = 2; y <= 6; ++y) for (int x = 4; x <= 9; ++x) scr(x, y, 3);
        for (int x = 0; x < f.w; x += 9) scr(x, f.h - 1, 3);
        scr(0, 0, 3);
    }
    if (m == 7) {   // sparse scatter: singles anywhere, including the last row and column
        const int n = std::max(4, f.w * f.h / 60);
        for (int i = 0; i < n; ++i) {
            const int x = rng.range(f.w), y = rng.range(f.h), kind = rng.range(4);
            if (kind == 0) cov(x, y, static_cast<uint8_t>(1 + rng.range(2) + 4 * rng.range(4)));
            else if (kind == 1) edi(x, y, static_cast<uint8_t>(1 + rng.range(255)));
            else if (kind == 2) { cov(x, y, static_cast<uint8_t>(rng.range(256))); scr(x, y, 3); }
            else scr(x, y, 3);
        }
    }
    if (m == 8) {   // a few random rectangles of coverage, some with an edited core
        const int n = 1 + rng.range(5);
        for (int i = 0; i < n; ++i) {
            const int x0 = rng.range(f.w), y0 = rng.range(f.h);
            const int rw = 1 + rng.range(std::max(1, f.w / 3)), rh = 1 + rng.range(std::max(1, f.h / 3));
            for (int y = y0; y < y0 + rh; ++y) {
                for (int x = x0; x < x0 + rw; ++x) {
                    cov(x, y, static_cast<uint8_t>(1 + rng.range(2) + 4 * rng.range(4)));
                    if (rng.range(3) == 0) edi(x, y, static_cast<uint8_t>(1 + rng.range(255)));
                }
            }
        }
    }
    if (m == 9) {   // a HUD's worth: six panels, about 15% of the eye, half with live text
        static const float box[6][4] = {{0.06f, 0.62f, 0.22f, 0.90f}, {0.78f, 0.62f, 0.94f, 0.90f}, {0.42f, 0.78f, 0.58f, 0.96f},
                                        {0.03f, 0.25f, 0.09f, 0.45f}, {0.91f, 0.25f, 0.97f, 0.45f}, {0.44f, 0.06f, 0.56f, 0.12f}};
        for (int b = 0; b < 6; ++b) {
            const int x0 = static_cast<int>(box[b][0] * f.w), y0 = static_cast<int>(box[b][1] * f.h);
            const int x1 = static_cast<int>(box[b][2] * f.w), y1 = static_cast<int>(box[b][3] * f.h);
            for (int y = y0; y < y1; ++y) {
                for (int x = x0; x < x1; ++x) {
                    cov(x, y, ((x ^ y) & 3) == 0 ? 2 : 1);
                    if (b % 2 == 0 && ((x / 6) ^ (y / 9)) % 5 == 0) edi(x, y, 255);
                }
            }
        }
    }
}

Inputs makeInputs(const Fix& f) {
    Inputs in;
    Rng rng(f.seed * 2654435761u + 12345u);
    fillRaw(f, rng, in.raw);
    fillTrained(f, rng, in.raw, in.trained);
    if (f.sx >= 0) {
        in.screenW = f.w + f.sx;
        in.screenH = f.h + f.sy;
        in.screen.assign(static_cast<size_t>(in.screenW) * in.screenH * 4, 0);
        for (size_t i = 0; i < in.screen.size(); i += 4) {   // colour and motion noise; validity 0..2, never 3 here
            in.screen[i] = toHalf(static_cast<float>(rng.range(100)) / 10.f);
            in.screen[i + 1] = toHalf(static_cast<float>(rng.range(100)) / 10.f);
            in.screen[i + 2] = toHalf(static_cast<float>(rng.range(100)) / 10.f);
            in.screen[i + 3] = toHalf(static_cast<float>(rng.range(3)));
        }
    }
    fillMarks(f, rng, in);
    if (f.hist) {
        in.hist.assign(static_cast<size_t>(f.w) * f.h, 0);
        if (f.hist == 4) {   // last frame's footprint: full influence where the marks are (bench)
            for (size_t i = 0; i < in.hist.size() && i < in.cover.size(); ++i) {
                const uint32_t k = in.cover[i] & 3u;
                if (k == 1u || k == 2u) in.hist[i] = 255;
            }
        } else if (f.hist != 3) {   // 3: bound and all zero (bench)
            const int every = f.hist == 1 ? 50 : 3;
            for (auto& v : in.hist) {
                if (rng.range(every) == 0) {
                    const int pick = rng.range(6);
                    v = pick == 0 ? 255 : pick == 1 ? 1 : static_cast<uint8_t>(1 + rng.range(255));
                }
            }
            // A few exact spikes the transported taps land on.
            if (f.w > 8 && f.h > 8) {
                in.hist[static_cast<size_t>(2) * f.w + 2] = 255;
                in.hist[static_cast<size_t>(f.h - 2) * f.w + (f.w - 2)] = 200;
            }
        }
    }
    in.motion.assign(static_cast<size_t>(f.w) * f.h * 2, 0);
    if (f.motion) {
        for (size_t i = 0; i < in.motion.size(); ++i) {
            float v = 0.f;
            switch (f.motion) {
                case 1: v = (static_cast<float>(rng.range(301)) - 150.f) / 100.f; break;
                case 2: v = (static_cast<float>(rng.range(2801)) - 1400.f) / 100.f; break;
                default: {
                    const int k = rng.range(12);
                    if (k == 0) { in.motion[i] = 0x7E00; continue; }            // NaN
                    if (k == 1) { in.motion[i] = 0x7C00; continue; }            // +Inf
                    if (k == 2) { in.motion[i] = 0xFC00; continue; }            // -Inf
                    if (k == 3) { in.motion[i] = 0x7BFF; continue; }            // 65504
                    v = (static_cast<float>(rng.range(2001)) - 1000.f) / 100.f;
                }
            }
            in.motion[i] = toHalf(v);
        }
    }
    return in;
}

// ------------------------------------------------------------------ the device

struct Device {
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<ID3D11InfoQueue> info;
    bool warp = true;
};

Device makeDevice(bool nvidia) {
    Device d;
    ComPtr<IDXGIAdapter> adapter;
    D3D_DRIVER_TYPE type = D3D_DRIVER_TYPE_WARP;
    if (nvidia) {
        ComPtr<IDXGIFactory1> factory;
        hr(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "DXGI factory");
        for (UINT i = 0;; ++i) {
            ComPtr<IDXGIAdapter> a;
            if (factory->EnumAdapters(i, &a) == DXGI_ERROR_NOT_FOUND) break;
            DXGI_ADAPTER_DESC desc{};
            a->GetDesc(&desc);
            if (desc.VendorId == 0x10DE) { adapter = a; break; }
        }
        if (!adapter) { std::puts("FAIL: no NVIDIA adapter for --adapter nvidia"); std::exit(1); }
        type = D3D_DRIVER_TYPE_UNKNOWN;
        d.warp = false;
    }
    D3D_FEATURE_LEVEL level{};
    HRESULT made = D3D11CreateDevice(adapter.Get(), type, nullptr, D3D11_CREATE_DEVICE_DEBUG, nullptr, 0,
                                     D3D11_SDK_VERSION, &d.dev, &level, &d.ctx);
    if (made == DXGI_ERROR_SDK_COMPONENT_MISSING)
        made = D3D11CreateDevice(adapter.Get(), type, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &d.dev, &level, &d.ctx);
    hr(made, "D3D11CreateDevice");
    d.dev.As(&d.info);
    return d;
}

ComPtr<ID3D11ComputeShader> shaderFromBytecode(ID3D11Device* dev, const unsigned char* bytes, size_t n) {
    ComPtr<ID3D11ComputeShader> cs;
    hr(dev->CreateComputeShader(bytes, n, nullptr, &cs), "CreateComputeShader");
    return cs;
}

// The build's own compile of a fixed shader: entry main, cs_5_0, flags zero.
ComPtr<ID3DBlob> compileBlob(const char* source, const D3D_SHADER_MACRO* defines, const char* what, const char* profile = "cs_5_0") {
    ComPtr<ID3DBlob> code, errors;
    const HRESULT r = D3DCompile(source, std::strlen(source), what, defines, nullptr, "main", profile, 0, 0, &code, &errors);
    if (FAILED(r)) {
        std::printf("FAIL: compiling %s (0x%08lX): %s\n", what, static_cast<unsigned long>(r),
                    errors ? static_cast<const char*>(errors->GetBufferPointer()) : "no message");
        std::exit(1);
    }
    return code;
}
ComPtr<ID3D11ComputeShader> shaderFromSource(ID3D11Device* dev, const char* source, const D3D_SHADER_MACRO* defines, const char* what) {
    ComPtr<ID3DBlob> code = compileBlob(source, defines, what);
    return shaderFromBytecode(dev, static_cast<const unsigned char*>(code->GetBufferPointer()), code->GetBufferSize());
}
uint64_t fnvText(const char* s) {
    uint64_t h = 14695981039346656037ull;
    for (; *s; ++s) { h ^= static_cast<uint8_t>(*s); h *= 1099511628211ull; }
    return h;
}

ComPtr<ID3D11Texture2D> tex(ID3D11Device* dev, int w, int h, DXGI_FORMAT fmt, UINT bind, const void* data, UINT rowBytes) {
    D3D11_TEXTURE2D_DESC d{};
    d.Width = static_cast<UINT>(w);
    d.Height = static_cast<UINT>(h);
    d.MipLevels = d.ArraySize = d.SampleDesc.Count = 1;
    d.Format = fmt;
    d.BindFlags = bind;
    D3D11_SUBRESOURCE_DATA init{data, rowBytes, 0};
    ComPtr<ID3D11Texture2D> t;
    hr(dev->CreateTexture2D(&d, data ? &init : nullptr, &t), "CreateTexture2D");
    return t;
}
ComPtr<ID3D11ShaderResourceView> srvOf(ID3D11Device* dev, ID3D11Texture2D* t) {
    ComPtr<ID3D11ShaderResourceView> v;
    hr(dev->CreateShaderResourceView(t, nullptr, &v), "CreateShaderResourceView");
    return v;
}
ComPtr<ID3D11UnorderedAccessView> uavOf(ID3D11Device* dev, ID3D11Texture2D* t) {
    ComPtr<ID3D11UnorderedAccessView> v;
    hr(dev->CreateUnorderedAccessView(t, nullptr, &v), "CreateUnorderedAccessView");
    return v;
}
std::vector<uint8_t> readBytes(Device& d, ID3D11Texture2D* t, int bytesPerPixel) {
    D3D11_TEXTURE2D_DESC td{};
    t->GetDesc(&td);
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> stage;
    hr(d.dev->CreateTexture2D(&td, nullptr, &stage), "staging texture");
    d.ctx->CopyResource(stage.Get(), t);
    D3D11_MAPPED_SUBRESOURCE map{};
    hr(d.ctx->Map(stage.Get(), 0, D3D11_MAP_READ, 0, &map), "Map");
    std::vector<uint8_t> out(static_cast<size_t>(td.Width) * td.Height * bytesPerPixel);
    for (UINT y = 0; y < td.Height; ++y)
        std::memcpy(&out[static_cast<size_t>(y) * td.Width * bytesPerPixel],
                    static_cast<const uint8_t*>(map.pData) + static_cast<size_t>(y) * map.RowPitch,
                    static_cast<size_t>(td.Width) * bytesPerPixel);
    d.ctx->Unmap(stage.Get(), 0);
    return out;
}

// ------------------------------------------------------------------ one dispatch

struct Params {   // the shader's cbuffer P, as temporal_pass.cpp lays it out
    int region[4];
    int size[2];
    int texSize[2];
    float tanNow[4], tanPrev[4], jit[4];
};

struct Result {
    std::vector<uint8_t> out;        // ow*oh*4, the output texture
    std::vector<uint8_t> influence;  // w*h, the history the pass wrote (its only meaningful channel)
};

// The layout of the history the shader reads and writes.
//   RgbaA  RGBA8 with the influence in alpha and junk in the colour channels:
//          the pass as it has always had it, reference and production alike.
//   R8     R8_UNORM, for the bench's what-would-R8-buy experiment only.
enum class Hist { RgbaA, R8 };

// One eye's worth of textures, bound the way temporal_pass.cpp binds them, so a
// fixture can be dispatched once (run) or timed many times (--bench).
struct Setup {
    const Fix* f = nullptr;
    Hist hist = Hist::R8;
    ComPtr<ID3D11Texture2D> output, next;
    ComPtr<ID3D11ShaderResourceView> srv[7];
    ComPtr<ID3D11UnorderedAccessView> outU, nextU;
    ComPtr<ID3D11Buffer> cbP, cbR;
};

// unbind: bit k leaves SRV slot k null even though the fixture has the texture
// (the bench's way of asking what a resource that is not bound costs).
// forceBits: the "inputs not bound" bits written to b1.z, in place of the ones
// the bindings imply (temporal_pass.cpp derives them the same way: coverage is
// slot 2, history 3, source edits 5); -1 leaves the derived bits. Only a
// fixture that binds b1 has any.
void makeSetup(Device& d, const Fix& f, const Inputs& in, Hist hist, Setup& s, uint32_t unbind = 0, int forceBits = -1) {
    ID3D11Device* dev = d.dev.Get();
    s.f = &f;
    s.hist = hist;
    auto raw = tex(dev, f.w, f.h, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE, in.raw.data(), f.w * 4);
    auto trained = tex(dev, f.ow, f.oh, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE, in.trained.data(), f.ow * 4);
    s.output = tex(dev, f.ow, f.oh, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, nullptr, 0);
    auto motion = tex(dev, f.w, f.h, DXGI_FORMAT_R16G16_FLOAT, D3D11_BIND_SHADER_RESOURCE, in.motion.data(), f.w * 4);
    const DXGI_FORMAT histFmt = hist == Hist::R8 ? DXGI_FORMAT_R8_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM;
    s.next = tex(dev, f.w, f.h, histFmt, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, nullptr, 0);

    ComPtr<ID3D11Texture2D> cover, edits, screen, prev;
    if (!in.cover.empty()) cover = tex(dev, f.w, f.h, DXGI_FORMAT_R8_UNORM, D3D11_BIND_SHADER_RESOURCE, in.cover.data(), f.w);
    if (!in.edits.empty()) edits = tex(dev, f.w, f.h, DXGI_FORMAT_R8_UNORM, D3D11_BIND_SHADER_RESOURCE, in.edits.data(), f.w);
    if (!in.screen.empty()) screen = tex(dev, in.screenW, in.screenH, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_SHADER_RESOURCE, in.screen.data(), in.screenW * 8);
    if (!in.hist.empty()) {
        if (hist == Hist::R8) {
            prev = tex(dev, f.w, f.h, histFmt, D3D11_BIND_SHADER_RESOURCE, in.hist.data(), f.w);
        } else {
            // The channels the shader does not read carry something that would
            // show if it did.
            std::vector<uint8_t> rgba(static_cast<size_t>(f.w) * f.h * 4);
            for (size_t i = 0; i < in.hist.size(); ++i) {
                for (int c = 0; c < 4; ++c) rgba[i * 4 + c] = static_cast<uint8_t>(37 + 54 * c + (3 + 4 * c) * i);
                rgba[i * 4 + 3] = in.hist[i];
            }
            prev = tex(dev, f.w, f.h, histFmt, D3D11_BIND_SHADER_RESOURCE, rgba.data(), f.w * 4);
        }
    }

    Params p{};
    p.region[0] = f.rx; p.region[1] = f.ry; p.region[2] = f.rx + f.w; p.region[3] = f.ry + f.h;
    p.size[0] = p.texSize[0] = f.w;
    p.size[1] = p.texSize[1] = f.h;
    p.jit[0] = f.jx; p.jit[1] = f.jy;
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = sizeof(Params);
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA pd{&p, 0, 0};
    hr(dev->CreateBuffer(&bd, &pd, &s.cbP), "cbuffer P");

    s.srv[0] = srvOf(dev, raw.Get());
    s.srv[1] = srvOf(dev, trained.Get());
    if (cover) s.srv[2] = srvOf(dev, cover.Get());
    if (prev) s.srv[3] = srvOf(dev, prev.Get());
    s.srv[4] = srvOf(dev, motion.Get());
    if (edits) s.srv[5] = srvOf(dev, edits.Get());
    if (screen) s.srv[6] = srvOf(dev, screen.Get());
    for (int i = 0; i < 7; ++i) if (unbind & (1u << i)) s.srv[i].Reset();
    if (f.tol >= 0.f) {   // b1 = {tolerance, hold limit, inputs not bound, 0}
        int bits = (s.srv[2] ? 0 : 1) | (s.srv[5] ? 0 : 2) | (s.srv[3] ? 0 : 4);
        if (forceBits >= 0) bits = forceBits;
        const float limits[4] = {f.tol, f.hold, static_cast<float>(bits), 0.f};
        D3D11_BUFFER_DESC bd1{};
        bd1.ByteWidth = 16;
        bd1.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        D3D11_SUBRESOURCE_DATA pd1{limits, 0, 0};
        hr(dev->CreateBuffer(&bd1, &pd1, &s.cbR), "cbuffer R");
    }
    s.outU = uavOf(dev, s.output.Get());
    s.nextU = uavOf(dev, s.next.Get());
}

void bindAndClear(Device& d, ID3D11ComputeShader* cs, Setup& s) {
    ID3D11DeviceContext* ctx = d.ctx.Get();
    const FLOAT poison[4] = {1.f, 0.f, 1.f, 0.03f};   // a hole in the dispatch's coverage shows as this
    ctx->ClearUnorderedAccessViewFloat(s.outU.Get(), poison);
    const FLOAT zero[4] = {0.f, 0.f, 0.f, 0.f};
    ctx->ClearUnorderedAccessViewFloat(s.nextU.Get(), zero);
    ID3D11ShaderResourceView* srvs[7];
    for (int i = 0; i < 7; ++i) srvs[i] = s.srv[i].Get();
    ID3D11UnorderedAccessView* uavs[2] = {s.outU.Get(), s.nextU.Get()};
    ctx->CSSetShader(cs, nullptr, 0);
    ctx->CSSetShaderResources(0, 7, srvs);
    ctx->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);
    ID3D11Buffer* cbs[2] = {s.cbP.Get(), s.cbR.Get()};
    ctx->CSSetConstantBuffers(0, 2, cbs);
}
void dispatch(Device& d, const Fix& f) {
    d.ctx->Dispatch(static_cast<UINT>((f.w + 7) / 8), static_cast<UINT>((f.h + 7) / 8), 1);
}

Result readback(Device& d, Setup& s) {
    const Fix& f = *s.f;
    Result r;
    r.out = readBytes(d, s.output.Get(), 4);
    const int bytes = s.hist == Hist::R8 ? 1 : 4;
    const std::vector<uint8_t> h = readBytes(d, s.next.Get(), bytes);
    const int keep = s.hist == Hist::RgbaA ? 3 : 0;   // alpha in an RGBA8 history, the only channel in R8
    r.influence.resize(static_cast<size_t>(f.w) * f.h);
    for (size_t i = 0; i < r.influence.size(); ++i) r.influence[i] = h[i * bytes + keep];
    return r;
}

Result run(Device& d, ID3D11ComputeShader* cs, const Fix& f, const Inputs& in, Hist hist, uint32_t unbind = 0, int forceBits = -1) {
    Setup s;
    makeSetup(d, f, in, hist, s, unbind, forceBits);
    bindAndClear(d, cs, s);
    dispatch(d, f);
    d.ctx->ClearState();
    return readback(d, s);
}

uint64_t fnv(const std::vector<uint8_t>& a, uint64_t h) {
    for (uint8_t b : a) { h ^= b; h *= 1099511628211ull; }
    return h;
}
uint64_t hashOf(const Result& r) {
    return fnv(r.influence, fnv(r.out, 14695981039346656037ull));
}

// What a fixture actually exercised, so a green run cannot be vacuous: output
// pixels the pass changed from NVIDIA's own (in RGB), history bytes it wrote
// nonzero, and history bytes that are not simply the marked footprint.
struct Stats {
    size_t changed = 0, influence = 0, unmarkedInfluence = 0, holes = 0;
};
Stats statsOf(const Fix& f, const Inputs& in, const Result& r) {
    Stats s;
    for (int y = 0; y < f.oh; ++y) {
        for (int x = 0; x < f.ow; ++x) {
            const size_t o = (static_cast<size_t>(y) * f.ow + x) * 4;
            if (r.out[o] == 0xFF && r.out[o + 1] == 0x00 && r.out[o + 2] == 0xFF && r.out[o + 3] == 0x08) ++s.holes;
            else if (std::memcmp(&r.out[o], &in.trained[o], 3) != 0) ++s.changed;
        }
    }
    for (size_t i = 0; i < r.influence.size(); ++i) {
        if (!r.influence[i]) continue;
        ++s.influence;
        if (r.influence[i] != 255) ++s.unmarkedInfluence;   // a decayed or transported value, not a fresh mark
    }
    return s;
}

// Recorded from the UNMODIFIED shader (RGBA8 history, point taps; commit
// 446d7e7a) on WARP under Windows 11 build 26200; see the header comment before
// re-recording. One per fixture, in kFix's order.
const uint64_t kGolden[] = {
    0xc9b3da52cacbc8ceull,  // sky_no_ui_2x
    0x62f9a5286ee11c77ull,  // sky_hold_off_b1_unbound
    0x7e25b389d2427364ull,  // sky_tol_zero_hold_on
    0x6c314b3d74bc30acull,  // corner_ui
    0xf6dbfc12c58fad28ull,  // corner_ui_jit_edge
    0x40e50ad3058a150aull,  // tile_edges_ui
    0xafff68269fe4b9ceull,  // tile_edges_ui_odd
    0xe6663939bb0743fcull,  // corona_edge
    0x1b9baf607c1501efull,  // corona_edge_15x
    0xe3763485b7f5acbcull,  // menu_full_frame
    0xdb0b6865c3c6a9afull,  // screen_ui
    0x8eb29c5a350291fcull,  // screen_mixed_region_max
    0x54655b3a250a4ea8ull,  // history_sparse_small_motion
    0xb436368871587d18ull,  // history_dense_large_motion
    0x01884880e78903a8ull,  // history_hostile_motion
    0x6ee86010d1db5c07ull,  // history_with_ui
    0xe8ced885b7ff336cull,  // ratio_1x
    0x8bbb2b4ea6c87dddull,  // ratio_133
    0x3a26effaf4455afeull,  // ratio_25
    0xba9cc4585510b3e7ull,  // ratio_075_down
    0xffacb9cc988504e1ull,  // jitter_1_3
    0xd5f69822bab7b63eull,  // jitter_0_9
    0xc172c0b90c3a74b1ull,  // jitter_neg_1_7
    0x7d3a5917875f3b7full,  // tiny_7x5
    0x79af1c447b2d5e05ull,  // wide_130x9
    0xa06e3d190bc881a7ull,  // tall_9x130
    0x05b55b2f3044e2c0ull,  // eye_left
    0xfc7b75ba4cf4a372ull,  // eye_right
    0xd15f5d8cc5a6b07aull,  // production_shape_eighth
};

// ------------------------------------------------------------------ comparing

struct Diff {
    size_t outBytes = 0, historyBytes = 0;
    size_t firstOut = static_cast<size_t>(-1), firstHistory = static_cast<size_t>(-1);
    bool same() const { return outBytes == 0 && historyBytes == 0; }
};
Diff compare(const Result& a, const Result& b) {
    Diff d;
    if (a.out.size() != b.out.size() || a.influence.size() != b.influence.size()) {
        d.outBytes = d.historyBytes = static_cast<size_t>(-1);
        return d;
    }
    for (size_t i = 0; i < a.out.size(); ++i)
        if (a.out[i] != b.out[i] && d.outBytes++ == 0) d.firstOut = i;
    for (size_t i = 0; i < a.influence.size(); ++i)
        if (a.influence[i] != b.influence[i] && d.historyBytes++ == 0) d.firstHistory = i;
    return d;
}

// Random fixtures for the pair comparison: sizes on and off the 8-pixel tile
// edge, the ratios DLSS uses and a few it never does, the jitter DLSS uses and
// worse (whole pixels, a hair either side of a half pixel, ten pixels, a
// hundred thousand, NaN), every mark pattern, history and motion.
Fix randomFix(uint32_t n) {
    Rng r(n * 2246822519u + 3266489917u);
    Fix f{};
    f.name = "fuzz_" + std::to_string(n);
    f.seed = 5000u + n;
    static const int edge[] = {7, 8, 9, 15, 16, 17, 31, 32, 33, 63, 64, 65};
    auto size = [&]() { return r.range(3) == 0 ? edge[r.range(12)] : 1 + r.range(140); };
    f.w = size();
    f.h = size();
    static const float scale[] = {1.0f, 1.0f, 1.25f, 1.5f, 1.75f, 2.0f, 2.0f, 2.0f, 3.0f, 0.75f, 0.5f};
    const float s = scale[r.range(11)];
    f.ow = std::max(1, static_cast<int>(static_cast<float>(f.w) * s + 0.5f));
    f.oh = std::max(1, static_cast<int>(static_cast<float>(f.h) * s + 0.5f) + (r.range(4) == 0 ? r.range(3) - 1 : 0));
    auto jitter = [&]() -> float {
        switch (r.range(14)) {
            case 0: return 0.f;
            case 1: return static_cast<float>(r.range(2001) - 1000) / 1000.f * 1.7f;
            case 2: return static_cast<float>(r.range(19) - 9) + 0.5f;
            case 3: return static_cast<float>(r.range(7) - 3) + 0.5f - 1e-6f;
            case 4: return static_cast<float>(r.range(7) - 3) - 0.5f + 1e-6f;
            case 5: return static_cast<float>(r.range(2001) - 1000) / 100.f;
            case 6: return (r.range(2) ? 1.0e5f : -1.0e5f) + static_cast<float>(r.range(1000)) / 7.f;
            case 7: return r.range(3) == 0 ? std::nanf("") : 0.5f;
            default: return static_cast<float>(r.range(1001) - 500) / 1000.f;
        }
    };
    f.jx = jitter();
    f.jy = jitter();
    static const float tols[] = {-1.f, 0.f, 2 / 255.f, 12 / 255.f, 12 / 255.f, 40 / 255.f};
    static const float holds[] = {0.f, 8 / 255.f, 64 / 255.f, 64 / 255.f, 200 / 255.f};
    f.tol = tols[r.range(6)];
    f.hold = f.tol < 0.f ? 0.f : holds[r.range(5)];
    f.content = r.range(4);
    f.marks = r.range(10) == 0 ? 0 : 1 + r.range(8);
    f.hist = r.range(3);
    f.motion = r.range(4);
    f.rx = r.range(6);
    f.ry = r.range(6);
    const bool screen = f.marks == 5 || f.marks == 6 || f.marks == 7 || r.range(4) == 0;
    f.sx = screen ? f.rx + r.range(4) : -1;
    f.sy = screen ? f.ry + r.range(4) : 0;
    return f;
}

bool upscale(const Fix& f) { return f.ow >= f.w && f.oh >= f.h; }
bool finiteJitter(const Fix& f) { return std::isfinite(f.jx) && std::isfinite(f.jy); }

// ------------------------------------------------------------------ timing

double median(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    return v.empty() ? -1.0 : v[v.size() / 2];
}

// One batch of back-to-back dispatches between two GPU timestamps, in
// milliseconds per dispatch (-1 when the clock was disjoint).
double timeBatch(Device& d, ID3D11ComputeShader* cs, Setup& s, int batch) {
    ID3D11DeviceContext* ctx = d.ctx.Get();
    bindAndClear(d, cs, s);
    D3D11_QUERY_DESC qd{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
    ComPtr<ID3D11Query> disjoint, t0, t1;
    hr(d.dev->CreateQuery(&qd, &disjoint), "disjoint query");
    qd.Query = D3D11_QUERY_TIMESTAMP;
    hr(d.dev->CreateQuery(&qd, &t0), "timestamp query");
    hr(d.dev->CreateQuery(&qd, &t1), "timestamp query");
    ctx->Begin(disjoint.Get());
    ctx->End(t0.Get());
    for (int i = 0; i < batch; ++i) dispatch(d, *s.f);
    ctx->End(t1.Get());
    ctx->End(disjoint.Get());
    ctx->ClearState();
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dd{};
    while (ctx->GetData(disjoint.Get(), &dd, sizeof(dd), 0) == S_FALSE) {}
    UINT64 a = 0, b = 0;
    while (ctx->GetData(t0.Get(), &a, sizeof(a), 0) == S_FALSE) {}
    while (ctx->GetData(t1.Get(), &b, sizeof(b), 0) == S_FALSE) {}
    return dd.Disjoint ? -1.0 : static_cast<double>(b - a) / static_cast<double>(dd.Frequency) * 1000.0 / batch;
}

// Milliseconds per dispatch for several shader/setup pairs: the median over
// `rounds` rounds, each round one batch of every pair in turn. Interleaved so a
// clock ramp or a thermal step costs every pair the same, and preceded by a
// warm-up long enough for the GPU to reach its working clocks.
std::vector<double> timeInterleaved(Device& d, const std::vector<std::pair<ID3D11ComputeShader*, Setup*>>& runs, int batch, int rounds) {
    for (int i = 0; i < 40; ++i)
        for (const auto& r : runs) timeBatch(d, r.first, *r.second, 4);
    std::vector<std::vector<double>> ms(runs.size());
    for (int round = 0; round < rounds; ++round) {
        for (size_t k = 0; k < runs.size(); ++k) {
            const double t = timeBatch(d, runs[k].first, *runs[k].second, batch);
            if (t > 0) ms[k].push_back(t);
        }
    }
    std::vector<double> out;
    for (auto& v : ms) out.push_back(median(v));
    return out;
}

// The production shader with a scalar history, for the bench's what-would-R8-buy
// column only (it was measured and not taken; see the performance review).
std::string scalarHistory(std::string src) {
    const struct { const char* from; const char* to; } swaps[] = {
        {"Texture2D<float4> Previous", "Texture2D<float> Previous"},
        {"RWTexture2D<float4> Next", "RWTexture2D<float> Next"},
        {"Previous.Load(int3(q,0)).a", "Previous.Load(int3(q,0))"},
        {"Previous.Load(int3(corner,0)).a", "Previous.Load(int3(corner,0))"},
        {"Previous.Load(int3(int2(upper.x,corner.y),0)).a", "Previous.Load(int3(int2(upper.x,corner.y),0))"},
        {"Previous.Load(int3(int2(corner.x,upper.y),0)).a", "Previous.Load(int3(int2(corner.x,upper.y),0))"},
        {"Previous.Load(int3(upper,0)).a", "Previous.Load(int3(upper,0))"},
        {"Next[id.xy]=float4(0,0,0,here?1:stale?max(remaining-1.0/32.0,0):0);", "Next[id.xy]=here?1:stale?max(remaining-1.0/32.0,0):0;"},
    };
    for (const auto& s : swaps) {
        const size_t at = src.find(s.from);
        if (at == std::string::npos) {
            std::printf("FAIL: the R8 experiment could not find \"%s\" in the shader\n", s.from);
            std::exit(1);
        }
        src.replace(at, std::strlen(s.from), s.to);
    }
    return src;
}

// The reference against the production shader at the eye's real size (the
// carrier flight of 2026-09-29: 2016x1948 in, 4032x3896 out, per eye), with the
// bindings the flight had. Each row: the frozen reference, the production
// shader, the same with the reference again (drift check), and the production
// shader over an R8 history (the experiment that was not taken).
void runBench(Device& d, ID3D11ComputeShader* refCs, ID3D11ComputeShader* curCs) {
    if (d.warp) std::puts("note: --bench on WARP times a CPU; use --adapter nvidia.");
    auto r8Cs = shaderFromSource(d.dev.Get(), scalarHistory(edvr::kUiResolve).c_str(), nullptr, "UI resolve, R8 history experiment");
    const float tol = 12 / 255.f, hold = 64 / 255.f;
    constexpr uint32_t kCover = 1u << 2, kHistory = 1u << 3, kEdits = 1u << 5;
    struct Scene { Fix f; uint32_t unbind; };
    const std::vector<Scene> scenes = {
        {{"idle, everything bound (empty masks)", 201, 2016, 1948, 4032, 3896, 0.25f, -0.4f, tol, hold, 3, 10, 3, 1, 0, 0, -1, 0}, 0},
        {{"idle, source-edit mask unbound (a still HUD)", 201, 2016, 1948, 4032, 3896, 0.25f, -0.4f, tol, hold, 3, 10, 3, 1, 0, 0, -1, 0}, kEdits},
        {{"idle, coverage mask unbound", 201, 2016, 1948, 4032, 3896, 0.25f, -0.4f, tol, hold, 3, 10, 3, 1, 0, 0, -1, 0}, kCover},
        {{"idle, history unbound (first frame, after a reset)", 201, 2016, 1948, 4032, 3896, 0.25f, -0.4f, tol, hold, 3, 10, 3, 1, 0, 0, -1, 0}, kHistory},
        {{"idle, all three unbound (no UI this frame)", 206, 2016, 1948, 4032, 3896, 0.25f, -0.4f, tol, hold, 3, 0, 0, 1, 0, 0, -1, 0}, 0},
        {{"HUD (15% marked), everything bound", 202, 2016, 1948, 4032, 3896, 0.25f, -0.4f, tol, hold, 3, 9, 4, 1, 0, 0, -1, 0}, 0},
        {{"HUD, source-edit mask unbound (a still HUD)", 202, 2016, 1948, 4032, 3896, 0.25f, -0.4f, tol, hold, 3, 9, 4, 1, 0, 0, -1, 0}, kEdits},
        {{"HUD + virtual-screen map, edit mask unbound", 203, 2016, 1948, 4032, 3896, 0.25f, -0.4f, tol, hold, 3, 9, 4, 1, 0, 0, 0, 0}, kEdits},
        {{"menu (whole eye marked), everything bound", 204, 2016, 1948, 4032, 3896, 0.25f, -0.4f, tol, hold, 3, 4, 4, 1, 0, 0, -1, 0}, 0},
        {{"idle, corona hold off, everything bound", 205, 2016, 1948, 4032, 3896, 0.25f, -0.4f, tol, 0.f, 3, 10, 3, 1, 0, 0, -1, 0}, 0},
    };
    std::printf("%-52s %10s %10s %8s %10s\n", "ms per dispatch, one eye", "reference", "production", "gain", "R8 (not shipped)");
    for (const Scene& scene : scenes) {
        const Fix& f = scene.f;
        const Inputs in = makeInputs(f);
        Setup sRef, sCur, sR8;
        makeSetup(d, f, in, Hist::RgbaA, sRef, scene.unbind);
        makeSetup(d, f, in, Hist::RgbaA, sCur, scene.unbind);
        makeSetup(d, f, in, Hist::R8, sR8, scene.unbind);
        const std::vector<double> t = timeInterleaved(d, {{refCs, &sRef}, {curCs, &sCur}, {refCs, &sRef}, {r8Cs.Get(), &sR8}}, 8, 25);
        bindAndClear(d, refCs, sRef);
        dispatch(d, f);
        d.ctx->ClearState();
        const Result ref = readback(d, sRef);
        bindAndClear(d, curCs, sCur);
        dispatch(d, f);
        d.ctx->ClearState();
        const Result cur = readback(d, sCur);
        std::printf("%-52s %10.4f %10.4f %7.1f%% %10.4f   (reference again %.4f)\n", f.name.c_str(), t[0], t[1],
                    t[0] > 0 ? 100.0 * (t[0] - t[1]) / t[0] : 0.0, t[3], t[2]);
        check(compare(ref, cur).same(), "the bench's eye-sized output and history are byte for byte the reference's");
    }
}

// ------------------------------------------------------------------ the UI layer composite, at the eye's size

// One timestamped batch of whatever `fn` issues, in milliseconds per call.
double timeFn(Device& d, int batch, const std::function<void()>& fn) {
    ID3D11DeviceContext* ctx = d.ctx.Get();
    D3D11_QUERY_DESC qd{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
    ComPtr<ID3D11Query> disjoint, t0, t1;
    hr(d.dev->CreateQuery(&qd, &disjoint), "disjoint query");
    qd.Query = D3D11_QUERY_TIMESTAMP;
    hr(d.dev->CreateQuery(&qd, &t0), "timestamp query");
    hr(d.dev->CreateQuery(&qd, &t1), "timestamp query");
    ctx->Begin(disjoint.Get());
    ctx->End(t0.Get());
    for (int i = 0; i < batch; ++i) fn();
    ctx->End(t1.Get());
    ctx->End(disjoint.Get());
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dd{};
    while (ctx->GetData(disjoint.Get(), &dd, sizeof(dd), 0) == S_FALSE) {}
    UINT64 a = 0, b = 0;
    while (ctx->GetData(t0.Get(), &a, sizeof(a), 0) == S_FALSE) {}
    while (ctx->GetData(t1.Get(), &b, sizeof(b), 0) == S_FALSE) {}
    return dd.Disjoint ? -1.0 : static_cast<double>(b - a) / static_cast<double>(dd.Frequency) * 1000.0 / batch;
}

// The layer composite (kUiLayerCompositeHlsl, as ui_layer.cpp runs it) over an
// eye-sized frame with a UI-quality-125% RGBA16F layer that is empty (cleared to
// the "nothing here" value), holds a HUD's worth of panels, or is covered; and,
// beside it, what a bare full-eye read and write costs. Desk numbers for the
// performance review: what would a composite that skipped the layer's empty
// tiles save? Nothing here is production code.
void runCompositeBench(Device& d) {
    if (d.warp) std::puts("note: --bench-composite on WARP times a CPU; use --adapter nvidia.");
    ID3D11Device* dev = d.dev.Get();
    ID3D11DeviceContext* ctx = d.ctx.Get();
    const int fw = 4032, fh = 3896, lw = 5040, lh = 4870;

    std::vector<uint8_t> frameData(static_cast<size_t>(fw) * fh * 4);
    Rng rng(4242);
    for (auto& b : frameData) b = static_cast<uint8_t>(rng.next() >> 24);
    auto frame = tex(dev, fw, fh, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE, frameData.data(), fw * 4);
    auto out = tex(dev, fw, fh, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, nullptr, 0);
    auto frameV = srvOf(dev, frame.Get());
    auto outU = uavOf(dev, out.Get());

    struct Layer { ComPtr<ID3D11Texture2D> t; ComPtr<ID3D11ShaderResourceView> srv; ComPtr<ID3D11RenderTargetView> rtv; };
    auto makeLayer = [&]() {
        Layer l;
        l.t = tex(dev, lw, lh, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, nullptr, 0);
        l.srv = srvOf(dev, l.t.Get());
        hr(dev->CreateRenderTargetView(l.t.Get(), nullptr, &l.rtv), "layer RTV");
        return l;
    };
    Layer empty = makeLayer(), hud = makeLayer(), full = makeLayer();

    static const char kVs[] =
        "float4 main(uint id : SV_VertexID) : SV_POSITION { float2 p = id == 0 ? float2(-1,-1) : (id == 1 ? float2(-1,3) : float2(3,-1)); return float4(p,0,1); }";
    static const char kPs[] =
        "float4 main(float4 p : SV_POSITION) : SV_Target { return float4(0.20, 0.35, 0.55, 0.35); }";
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    {
        ComPtr<ID3DBlob> v = compileBlob(kVs, nullptr, "layer bench vs", "vs_5_0");
        hr(dev->CreateVertexShader(v->GetBufferPointer(), v->GetBufferSize(), nullptr, &vs), "VS");
        ComPtr<ID3DBlob> p = compileBlob(kPs, nullptr, "layer bench ps", "ps_5_0");
        hr(dev->CreatePixelShader(p->GetBufferPointer(), p->GetBufferSize(), nullptr, &ps), "PS");
    }
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    rd.ScissorEnable = TRUE;
    ComPtr<ID3D11RasterizerState> rs;
    hr(dev->CreateRasterizerState(&rd, &rs), "rasterizer state");

    auto paint = [&](Layer& l, const std::vector<D3D11_RECT>& panels) {
        const FLOAT clear[4] = {0.f, 0.f, 0.f, 1.f};   // "nothing here": no colour, fully transparent
        ctx->ClearRenderTargetView(l.rtv.Get(), clear);
        ID3D11RenderTargetView* rtv = l.rtv.Get();
        ctx->OMSetRenderTargets(1, &rtv, nullptr);
        D3D11_VIEWPORT vp{0.f, 0.f, static_cast<float>(lw), static_cast<float>(lh), 0.f, 1.f};
        ctx->RSSetViewports(1, &vp);
        ctx->RSSetState(rs.Get());
        ctx->VSSetShader(vs.Get(), nullptr, 0);
        ctx->PSSetShader(ps.Get(), nullptr, 0);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        for (const auto& r : panels) { ctx->RSSetScissorRects(1, &r); ctx->Draw(3, 0); }
        ctx->ClearState();
    };
    static const float box[6][4] = {{0.06f, 0.62f, 0.22f, 0.90f}, {0.78f, 0.62f, 0.94f, 0.90f}, {0.42f, 0.78f, 0.58f, 0.96f},
                                    {0.03f, 0.25f, 0.09f, 0.45f}, {0.91f, 0.25f, 0.97f, 0.45f}, {0.44f, 0.06f, 0.56f, 0.12f}};
    std::vector<D3D11_RECT> panels;
    for (const auto& b : box)
        panels.push_back({static_cast<LONG>(b[0] * lw), static_cast<LONG>(b[1] * lh), static_cast<LONG>(b[2] * lw), static_cast<LONG>(b[3] * lh)});
    paint(empty, {});
    paint(hud, panels);
    paint(full, {{0, 0, lw, lh}});

    ComPtr<ID3D11ComputeShader> cs = shaderFromSource(dev, edvr::kUiLayerCompositeHlsl, nullptr, "UI layer composite");
    static const char kCopyCs[] =
        "Texture2D<float4> F : register(t0); RWTexture2D<float4> O : register(u0);"
        "[numthreads(8,8,1)] void main(uint3 id : SV_DispatchThreadID) { O[id.xy] = F.Load(int3(id.xy, 0)); }";
    ComPtr<ID3D11ComputeShader> copyCs = shaderFromSource(dev, kCopyCs, nullptr, "bare copy");

    struct Params {
        int32_t region[4];
        float uv[4];
        float layerSize[2];
        uint32_t outSize[2];
        uint32_t mode, useMult, pad[2];
    } p{{0, 0, fw, fh}, {0.f, 0.f, 1.f, 1.f}, {static_cast<float>(lw), static_cast<float>(lh)}, {static_cast<uint32_t>(fw), static_cast<uint32_t>(fh)}, 0, 0, {0, 0}};
    static_assert(sizeof(Params) == 64, "the composite's cbuffer is 64 bytes");
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = sizeof(Params);
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA pd{&p, 0, 0};
    ComPtr<ID3D11Buffer> cb;
    hr(dev->CreateBuffer(&bd, &pd, &cb), "composite cbuffer");

    auto composite = [&](Layer& l) {
        ID3D11ShaderResourceView* srvs[3] = {frameV.Get(), l.srv.Get(), nullptr};
        ID3D11UnorderedAccessView* uav = outU.Get();
        ID3D11Buffer* cbp = cb.Get();
        ctx->CSSetShader(cs.Get(), nullptr, 0);
        ctx->CSSetShaderResources(0, 3, srvs);
        ctx->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
        ctx->CSSetConstantBuffers(0, 1, &cbp);
        ctx->Dispatch(static_cast<UINT>((fw + 7) / 8), static_cast<UINT>((fh + 7) / 8), 1);
        ctx->ClearState();
    };
    auto bareCopy = [&]() {
        ID3D11ShaderResourceView* srv = frameV.Get();
        ID3D11UnorderedAccessView* uav = outU.Get();
        ctx->CSSetShader(copyCs.Get(), nullptr, 0);
        ctx->CSSetShaderResources(0, 1, &srv);
        ctx->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
        ctx->Dispatch(static_cast<UINT>((fw + 7) / 8), static_cast<UINT>((fh + 7) / 8), 1);
        ctx->ClearState();
    };
    const std::vector<std::pair<const char*, std::function<void()>>> runs = {
        {"CopyResource frame -> out (the floor's floor)", [&]() { ctx->CopyResource(out.Get(), frame.Get()); }},
        {"bare compute copy frame -> out", bareCopy},
        {"composite, layer empty (cleared, never drawn)", [&]() { composite(empty); }},
        {"composite, layer with a HUD's panels (15%)", [&]() { composite(hud); }},
        {"composite, layer covered", [&]() { composite(full); }},
    };
    for (int i = 0; i < 40; ++i)
        for (const auto& r : runs) timeFn(d, 4, r.second);
    std::vector<std::vector<double>> ms(runs.size());
    for (int round = 0; round < 25; ++round)
        for (size_t k = 0; k < runs.size(); ++k) {
            const double t = timeFn(d, 8, runs[k].second);
            if (t > 0) ms[k].push_back(t);
        }
    std::printf("ms per call, one eye: frame %dx%d, layer %dx%d RGBA16F\n", fw, fh, lw, lh);
    for (size_t k = 0; k < runs.size(); ++k) std::printf("  %-52s %8.4f\n", runs[k].first, median(ms[k]));

    // What an empty layer does to the frame: nothing. (The composite of a layer
    // that is all "nothing here" is the frame, bit for bit: l = (0,0,0,1) exactly,
    // c = 0 + f * 1 * 1.) The bench is only worth reading if that holds.
    composite(empty);
    const std::vector<uint8_t> got = readBytes(d, out.Get(), 4);
    check(got == frameData, "a composite over an empty layer returns the frame byte for byte");
}

}  // namespace

int main(int argc, char** argv) {
    bool selfTest = false, dry = false, print = false, nvidia = false, stats = false, bench = false, benchComposite = false;
    int fuzz = 240;
    const char* usage =
        "usage: ui_holo_pass_test --self-test|--dry-run|--print-goldens|--stats|--bench|--bench-composite [--fuzz N] [--adapter nvidia]";
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--self-test")) selfTest = true;
        else if (!std::strcmp(argv[i], "--dry-run")) dry = true;
        else if (!std::strcmp(argv[i], "--print-goldens")) print = true;
        else if (!std::strcmp(argv[i], "--stats")) stats = true;
        else if (!std::strcmp(argv[i], "--bench")) bench = true;
        else if (!std::strcmp(argv[i], "--bench-composite")) benchComposite = true;
        else if (!std::strcmp(argv[i], "--fuzz") && i + 1 < argc) fuzz = std::max(0, std::atoi(argv[++i]));
        else if (!std::strcmp(argv[i], "--adapter") && i + 1 < argc && !std::strcmp(argv[i + 1], "nvidia")) { nvidia = true; ++i; }
        else { std::puts(usage); return 2; }
    }
    if (dry) {
        std::puts("Would compare the production UI resolve against its frozen reference on WARP, byte for byte, over the fixtures and fuzz cases; writes no files.");
        return 0;
    }
    if (!selfTest && !print && !stats && !bench && !benchComposite) { std::puts(usage); return 2; }

    Device d = makeDevice(nvidia);
    ID3D11Device* dev = d.dev.Get();
    if (benchComposite) {
        runCompositeBench(d);
        if (g_failures) { std::printf("FAILED: %d of %d checks\n", g_failures, g_checks); return 1; }
        return 0;
    }

    // The reference must be the shader it claims to be, and the bytecode header
    // must be the production source compiled; otherwise a green run says nothing.
    check(fnvText(edvr_reference::kUiResolveReference) == 0x34EA8555DBBE7C05ull,
          "the frozen reference is the shader temporal_shader_build pinned before the change");
    {
        ComPtr<ID3DBlob> code = compileBlob(edvr::kUiResolve, nullptr, "UI resolve");
        check(code->GetBufferSize() == sizeof(edvr::kUiResolveBytecode) &&
                  !std::memcmp(code->GetBufferPointer(), edvr::kUiResolveBytecode, sizeof(edvr::kUiResolveBytecode)),
              "the bytecode header is current: it is src/d3d11/ui_resolve.h compiled, byte for byte");
    }
    auto refCs = shaderFromSource(dev, edvr_reference::kUiResolveReference, nullptr, "UI resolve");
    auto curCs = shaderFromBytecode(dev, edvr::kUiResolveBytecode, sizeof(edvr::kUiResolveBytecode));

    if (bench) {
        runBench(d, refCs.Get(), curCs.Get());
        if (g_failures) { std::printf("FAILED: %d of %d checks\n", g_failures, g_checks); return 1; }
        return 0;
    }

    std::vector<Fix> fixes(kFix, kFix + kFixtures);
    for (int n = 0; n < fuzz; ++n) fixes.push_back(randomFix(static_cast<uint32_t>(n)));

    // What is counted, so the summary can say what the controls saw.
    size_t pairs = 0, skipRuns = 0, singleUnbindRuns = 0, failSafeRuns = 0, wrongFlagRuns = 0, wrongFlagDiverged = 0, fuzzChanged = 0, fuzzRuns = 0;
    std::vector<uint64_t> got;
    for (size_t i = 0; i < fixes.size(); ++i) {
        const Fix& f = fixes[i];
        const bool named = i < kFixtures;
        const Inputs in = makeInputs(f);
        const Result ref = run(d, refCs.Get(), f, in, Hist::RgbaA);
        const Result cur = run(d, curCs.Get(), f, in, Hist::RgbaA);
        const Stats st = statsOf(f, in, ref);
        char label[220];

        // The pair: the production shader, its unbound inputs skipped as the
        // pass skips them, is the reference byte for byte.
        const Diff pair = compare(ref, cur);
        std::snprintf(label, sizeof(label), "%s: the production shader is the reference, byte for byte", f.name.c_str());
        if (!pair.same())
            std::printf("  %s differs: %zu output bytes (first at %zu), %zu history bytes (first at %zu)\n", f.name.c_str(),
                        pair.outBytes, pair.firstOut, pair.historyBytes, pair.firstHistory);
        check(pair.same(), label);
        ++pairs;
        const int unboundBits = (in.cover.empty() ? 1 : 0) | (in.edits.empty() ? 2 : 0) | (in.hist.empty() ? 4 : 0);
        if (f.tol >= 0.f && unboundBits != 0) ++skipRuns;   // b1 carries the bits: this pair really skipped a fetch
        if (named) got.push_back(hashOf(ref));
        if (named && d.warp) {
            std::snprintf(label, sizeof(label), "%s: the frozen reference reproduces its recorded golden", f.name.c_str());
            check(hashOf(ref) == kGolden[i], label);
            std::snprintf(label, sizeof(label), "%s: the production shader reproduces the recorded golden", f.name.c_str());
            check(hashOf(cur) == kGolden[i], label);
        }
        if (!named) { ++fuzzRuns; if (st.changed > 0) ++fuzzChanged; }

        // A green run must not be a vacuous one: what each named fixture was
        // built to reach, it reached.
        if (named) {
            std::snprintf(label, sizeof(label), "%s: the dispatch covers every output pixel", f.name.c_str());
            check(st.holes == 0, label);
            const bool idle = f.tol < 0.f && f.hold == 0.f && f.marks == 0 && f.hist == 0;
            std::snprintf(label, sizeof(label), "%s: %s", f.name.c_str(), idle ? "an idle pass is the identity" : "the pass changed pixels");
            check(idle ? (st.changed == 0 && st.influence == 0) : st.changed > 0, label);
            if (f.marks != 0) {
                std::snprintf(label, sizeof(label), "%s: marks leave influence in the history", f.name.c_str());
                check(st.influence > 0, label);
            }
            if (f.hist != 0 && f.motion != 0 && f.w > 8 && f.h > 8) {   // the exact spikes need room
                std::snprintf(label, sizeof(label), "%s: transport carries influence past the marks", f.name.c_str());
                check(st.unmarkedInfluence > 0, label);
            }
        }

        // One input unbound at a time, the rest bound with their content -- the
        // state of a still HUD (source edits null, coverage and history bound):
        // the production shader with that view null must be the reference over a
        // texture of zeros where the view was.
        if (f.tol >= 0.f && (named || i % 3 == 0)) {
            struct Slot { int srv; std::vector<uint8_t> Inputs::* array; const char* name; };
            static const Slot slots[3] = {{2, &Inputs::cover, "coverage"}, {5, &Inputs::edits, "source edits"}, {3, &Inputs::hist, "history"}};
            for (const Slot& sl : slots) {
                if ((in.*(sl.array)).empty()) continue;
                Inputs zeroed = in;
                std::fill((zeroed.*(sl.array)).begin(), (zeroed.*(sl.array)).end(), static_cast<uint8_t>(0));
                const Result refZero = run(d, refCs.Get(), f, zeroed, Hist::RgbaA);
                const Result curNull = run(d, curCs.Get(), f, in, Hist::RgbaA, 1u << sl.srv);
                std::snprintf(label, sizeof(label), "%s: %s unbound, the rest bound: the production shader is the reference over zeros", f.name.c_str(), sl.name);
                check(compare(refZero, curNull).same(), label);
                ++singleUnbindRuns;
            }
        }

        // The controls, on fixtures that bind b1 (the bits travel there).
        if (f.tol >= 0.f) {
            // Fail-safe: bits of zero -- what a caller that says nothing sends --
            // over inputs that really are unbound is the pass as it was (a null
            // view reads zero), just slower.
            if (unboundBits != 0) {
                const Result failSafe = run(d, curCs.Get(), f, in, Hist::RgbaA, 0, 0);
                std::snprintf(label, sizeof(label), "%s: bits of zero over unbound inputs is the reference too (fail-safe)", f.name.c_str());
                check(compare(ref, failSafe).same(), label);
                ++failSafeRuns;
            }
            // Negative control: claim every input unbound while they are bound
            // with content. Fixtures with content must NOT survive it.
            const bool hasContent = !in.cover.empty() || !in.edits.empty() || !in.hist.empty();
            if (hasContent) {
                const Result wrong = run(d, curCs.Get(), f, in, Hist::RgbaA, 0, 7);
                ++wrongFlagRuns;
                if (!compare(ref, wrong).same()) ++wrongFlagDiverged;
            }
        }
        if (stats && named)
            std::printf("%-30s out %4dx%-4d changed %6zu  influence %5zu (decayed/transported %5zu)  unbound bits %d\n", f.name.c_str(),
                        f.ow, f.oh, st.changed, st.influence, st.unmarkedInfluence, unboundBits);
    }
    if (print) {
        std::printf("goldens: %zu fixtures, from the frozen reference on this adapter\n", got.size());
        for (size_t i = 0; i < got.size(); ++i)
            std::printf("    0x%016llxull,  // %s\n", static_cast<unsigned long long>(got[i]), kFix[i].name.c_str());
    }
    if (selfTest) {
        check(sizeof(kGolden) / sizeof(kGolden[0]) == kFixtures, "the goldens cover every fixture");
        if (!d.warp)
            std::puts("note: the goldens are WARP's; skipped on this adapter (the pairs above are the proof here).");
        // Each bit, by name, on a fixture whose content it would hide.
        struct BitCase { const char* fixture; int bit; const char* what; };
        for (const BitCase& c : {BitCase{"corner_ui", 1, "coverage"}, BitCase{"corner_ui", 2, "source edits"},
                                 BitCase{"history_dense_large_motion", 4, "history"}, BitCase{"menu_full_frame", 1, "coverage"},
                                 BitCase{"history_with_ui", 4, "history"}}) {
            for (size_t i = 0; i < kFixtures; ++i) {
                if (c.fixture != kFix[i].name) continue;
                const Fix& f = fixes[i];
                const Inputs in = makeInputs(f);
                // Bind everything the fixture has, then claim just this bit.
                const Result ref = run(d, refCs.Get(), f, in, Hist::RgbaA);
                const Result wrong = run(d, curCs.Get(), f, in, Hist::RgbaA, 0, c.bit);
                char label[200];
                std::snprintf(label, sizeof(label), "CONTROL: %s: claiming the %s unbound over a bound one fails the pair", c.fixture, c.what);
                check(!compare(ref, wrong).same(), label);
            }
        }
        check(skipRuns >= 20, "the pairs include 20 or more runs whose b1 bits really skipped a fetch");
        check(singleUnbindRuns >= 60, "the one-input-unbound pairs ran 60 or more times");
        check(failSafeRuns >= 20, "the fail-safe control ran 20 or more times");
        check(wrongFlagRuns > 0 && wrongFlagDiverged * 2 >= wrongFlagRuns, "CONTROL: claiming bound inputs unbound fails at least half the fixtures that have content");
        check(fuzz == 0 || fuzzChanged * 5 >= fuzzRuns * 3, "the fuzz cases change pixels (not vacuous)");
        if (d.info) {
            const UINT64 n = d.info->GetNumStoredMessagesAllowedByRetrievalFilter();
            UINT64 errors = 0;
            for (UINT64 m = 0; m < n; ++m) {
                SIZE_T len = 0;
                d.info->GetMessage(m, nullptr, &len);
                std::vector<char> buf(len);
                auto* msg = reinterpret_cast<D3D11_MESSAGE*>(buf.data());
                if (SUCCEEDED(d.info->GetMessage(m, msg, &len)) && msg->Severity <= D3D11_MESSAGE_SEVERITY_ERROR) {
                    ++errors;
                    std::printf("D3D11 %s\n", msg->pDescription);
                }
            }
            check(errors == 0, "the D3D11 debug layer reported no errors");
        }
        std::printf("pairs: %zu fixtures byte for byte against the reference (%zu named, %d fuzz); %zu of them skipped a fetch on b1's bits\n",
                    pairs, kFixtures, fuzz, skipRuns);
        std::printf("one input unbound at a time: %zu runs equal the reference over zeros\n", singleUnbindRuns);
        std::printf("controls: fail-safe (bits of zero over unbound inputs) held on %zu; claiming bound inputs unbound failed %zu of %zu\n",
                    failSafeRuns, wrongFlagDiverged, wrongFlagRuns);
        if (g_failures) { std::printf("FAILED: %d of %d checks\n", g_failures, g_checks); return 1; }
        std::printf("PASS: %d checks.\n", g_checks);
    }
    return 0;
}

