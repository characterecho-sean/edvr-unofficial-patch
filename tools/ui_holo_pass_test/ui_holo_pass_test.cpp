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

#include "../../src/d3d11/fixed_shader_source.h"   // kUiLayerCompositeHlsl and its in-place twin: the equality test and --bench-composite
#include "../../src/d3d11/ui_layer_shaders.h"      // kUiLayerCompositeGroupX/Y
#include "../../src/d3d11/ui_resolve.h"
#include "temporal_shader_bytecode.h"  // edvr::kUiResolveBytecode, from build\gen
#include "ui_resolve_reference.h"      // edvr_reference::kUiResolveReference, the shader as it was
#include "near_light_reference.h"      // edvr_reference::kNearLightReference, the near-light shader as it was

// RCAS's host side (FsrRcasCon) and its shader text, for --bench-groups; sharpen_pass.cpp's arrangement.
#define A_CPU 1
#pragma warning(push)
#pragma warning(disable : 4505)
#include "../../src/d3d11/fsr/ffx_a.h"
#include "../../src/d3d11/fsr/ffx_fsr1.h"
#pragma warning(pop)
#include "../../src/d3d11/fixed_extra_shader_source.h"

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

Device makeDevice(bool nvidia, bool debugLayer = true) {
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
    HRESULT made = D3D11CreateDevice(adapter.Get(), type, nullptr, debugLayer ? D3D11_CREATE_DEVICE_DEBUG : 0, nullptr, 0,
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

// The layer composite (kUiLayerCompositeHlsl, as ui_layer.cpp runs it out of place) against
// the same composite in place (kUiLayerCompositeInPlaceHlsl), over an eye-sized frame and the
// production layer (RGBA8 UNORM, at ui_quality 100 and 125), empty (cleared to the "nothing
// here" value), holding a HUD's worth of panels, or covered; and, beside them, what a bare
// full-eye read and write costs. Nothing here is production code except the shaders it times.
struct CompositeParams {
    int32_t region[4];
    float uv[4];
    float layerSize[2];
    uint32_t outSize[2];
    uint32_t mode, useMult, pad[2];
};
static_assert(sizeof(CompositeParams) == 64, "the composite's cbuffer is 64 bytes");

// The shader as production runs it (the build's bytecode) against the source it was compiled from.
void checkCompositeBytecodeCurrent() {
    ComPtr<ID3DBlob> a = compileBlob(edvr::kUiLayerCompositeHlsl, nullptr, "ui_layer_composite_cs");
    check(a->GetBufferSize() == sizeof(edvr::kUiLayerCompositeBytecode) &&
              !std::memcmp(a->GetBufferPointer(), edvr::kUiLayerCompositeBytecode, sizeof(edvr::kUiLayerCompositeBytecode)),
          "the out-of-place composite's bytecode header is current: kUiLayerCompositeHlsl compiled, byte for byte");
    ComPtr<ID3DBlob> b = compileBlob(edvr::kUiLayerCompositeInPlaceHlsl, nullptr, "ui_layer_composite_inplace_cs");
    check(b->GetBufferSize() == sizeof(edvr::kUiLayerCompositeInPlaceBytecode) &&
              !std::memcmp(b->GetBufferPointer(), edvr::kUiLayerCompositeInPlaceBytecode, sizeof(edvr::kUiLayerCompositeInPlaceBytecode)),
          "the in-place composite's bytecode header is current: kUiLayerCompositeInPlaceHlsl compiled, byte for byte");
}

bool typedLoadOk(Device& d, DXGI_FORMAT f) {
    D3D11_FEATURE_DATA_FORMAT_SUPPORT2 fs{f, 0};
    return SUCCEEDED(d.dev->CheckFeatureSupport(D3D11_FEATURE_FORMAT_SUPPORT2, &fs, sizeof(fs))) &&
           (fs.OutFormatSupport2 & D3D11_FORMAT_SUPPORT2_UAV_TYPED_LOAD) != 0;
}

// One composite, both ways, over the same inputs.
struct CompCase {
    const char* name;
    int texW, texH;          // the frame texture
    int region[4];           // x0, y0, x1, y1 in it
    int lw, lh;              // the layer
    float uv[4];
    bool mult;
    uint32_t mode;
    int emptyPercent;        // how much of the layer is "nothing here"
};

// A layer pixel: mostly "nothing here" exactly (0,0,0,255), else opaque ink (a = 255, rgb > 0), a translucent
// tint, a clear hole (a = 0) or a dark shade (rgb = 0, a < 255), so every branch of the arithmetic and of the skip runs.
void fillLayer(Rng& rng, int emptyPercent, std::vector<uint8_t>& px, bool isMult) {
    for (size_t i = 0; i + 3 < px.size(); i += 4) {
        const uint32_t r = rng.next() >> 8;
        const int pick = static_cast<int>(r % 100);
        uint8_t* p = &px[i];
        if (isMult) {   // the transmittance: mostly 1, sometimes a per-channel dim
            const bool one = pick < 70;
            for (int c = 0; c < 3; ++c) p[c] = one ? 255 : static_cast<uint8_t>(rng.next() >> 24);
            p[3] = 255;
            continue;
        }
        if (pick < emptyPercent) { p[0] = p[1] = p[2] = 0; p[3] = 255; continue; }
        const int kind = static_cast<int>((r >> 7) % 5);
        const uint8_t a = static_cast<uint8_t>(rng.next() >> 24), b = static_cast<uint8_t>(rng.next() >> 24),
                      c = static_cast<uint8_t>(rng.next() >> 24), e = static_cast<uint8_t>(rng.next() >> 24);
        switch (kind) {
            case 0: p[0] = static_cast<uint8_t>(a | 1); p[1] = b; p[2] = c; p[3] = 255; break;   // ink
            case 1: p[0] = a; p[1] = b; p[2] = c; p[3] = e; break;                              // tint
            case 2: p[0] = a; p[1] = b; p[2] = c; p[3] = 0; break;                              // hole
            case 3: p[0] = p[1] = p[2] = 0; p[3] = e; break;                                    // shade
            default: p[0] = 0; p[1] = 0; p[2] = 1; p[3] = 255; break;                           // the faintest ink
        }
    }
}

struct CompResult { std::vector<uint8_t> outOfPlace, inPlace; };

// shaderOut/shaderIn: the two compute shaders; the in-place one may be a mutant for a control.
CompResult runComposite(Device& d, const CompCase& c, ID3D11ComputeShader* shaderOut, ID3D11ComputeShader* shaderIn, bool production,
                        int outGx = static_cast<int>(edvr::kUiLayerCompositeGroupX), int outGy = static_cast<int>(edvr::kUiLayerCompositeGroupY)) {
    ID3D11Device* dev = d.dev.Get();
    ID3D11DeviceContext* ctx = d.ctx.Get();
    Rng rng(0xC0FFEEu + static_cast<uint32_t>(c.texW * 131 + c.texH * 7 + c.lw));
    std::vector<uint8_t> frameData(static_cast<size_t>(c.texW) * c.texH * 4), layerData(static_cast<size_t>(c.lw) * c.lh * 4),
        multData(static_cast<size_t>(c.lw) * c.lh * 4);
    for (auto& b : frameData) b = static_cast<uint8_t>(rng.next() >> 24);
    fillLayer(rng, c.emptyPercent, layerData, false);
    fillLayer(rng, 0, multData, true);
    auto frame = tex(dev, c.texW, c.texH, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE, frameData.data(), c.texW * 4);
    auto frameV = srvOf(dev, frame.Get());
    const int rw = c.region[2] - c.region[0], rh = c.region[3] - c.region[1];
    auto out = tex(dev, rw, rh, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, nullptr, 0);
    auto outU = uavOf(dev, out.Get());
    auto work = tex(dev, c.texW, c.texH, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, frameData.data(), c.texW * 4);
    auto workU = uavOf(dev, work.Get());
    auto layer = tex(dev, c.lw, c.lh, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE, layerData.data(), c.lw * 4);
    auto layerV = srvOf(dev, layer.Get());
    auto mult = tex(dev, c.lw, c.lh, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE, multData.data(), c.lw * 4);
    auto multV = srvOf(dev, mult.Get());

    CompositeParams p{{c.region[0], c.region[1], c.region[2], c.region[3]}, {c.uv[0], c.uv[1], c.uv[2], c.uv[3]},
                      {static_cast<float>(c.lw), static_cast<float>(c.lh)}, {static_cast<uint32_t>(rw), static_cast<uint32_t>(rh)},
                      c.mode, c.mult ? 1u : 0u, {0, 0}};
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = sizeof(p);
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA pd{&p, 0, 0};
    ComPtr<ID3D11Buffer> cb;
    hr(dev->CreateBuffer(&bd, &pd, &cb), "composite cbuffer");
    ID3D11Buffer* cbp = cb.Get();

    // Out of place, as ui_layer.cpp does: SRV frame, SRV layer (and multiply), UAV out; 32x8 groups (outGx/outGy).
    {
        ID3D11ShaderResourceView* srvs[3] = {frameV.Get(), layerV.Get(), c.mult ? multV.Get() : nullptr};
        ID3D11UnorderedAccessView* uav = outU.Get();
        ctx->CSSetShader(shaderOut, nullptr, 0);
        ctx->CSSetShaderResources(0, 3, srvs);
        ctx->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
        ctx->CSSetConstantBuffers(0, 1, &cbp);
        ctx->Dispatch(static_cast<UINT>((rw + outGx - 1) / outGx), static_cast<UINT>((rh + outGy - 1) / outGy), 1);
        ctx->ClearState();
    }
    // In place: no SRV of the frame at all, the frame as the UAV; 32x8 groups.
    {
        ID3D11ShaderResourceView* srvs[3] = {nullptr, layerV.Get(), c.mult ? multV.Get() : nullptr};
        ID3D11UnorderedAccessView* uav = workU.Get();
        ctx->CSSetShader(shaderIn, nullptr, 0);
        ctx->CSSetShaderResources(0, 3, srvs);
        ctx->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
        ctx->CSSetConstantBuffers(0, 1, &cbp);
        ctx->Dispatch(static_cast<UINT>((rw + static_cast<int>(edvr::kUiLayerCompositeGroupX) - 1) / static_cast<int>(edvr::kUiLayerCompositeGroupX)),
                      static_cast<UINT>((rh + static_cast<int>(edvr::kUiLayerCompositeGroupY) - 1) / static_cast<int>(edvr::kUiLayerCompositeGroupY)), 1);
        ctx->ClearState();
    }
    CompResult r;
    r.outOfPlace = readBytes(d, out.Get(), 4);
    const std::vector<uint8_t> whole = readBytes(d, work.Get(), 4);
    // The in-place texture, cut to the region; and everything outside the region must still be the frame.
    r.inPlace.resize(static_cast<size_t>(rw) * rh * 4);
    bool outsideUntouched = true;
    for (int y = 0; y < c.texH; ++y)
        for (int x = 0; x < c.texW; ++x) {
            const size_t at = (static_cast<size_t>(y) * c.texW + x) * 4;
            const bool inside = x >= c.region[0] && x < c.region[2] && y >= c.region[1] && y < c.region[3];
            if (inside) std::memcpy(&r.inPlace[(static_cast<size_t>(y - c.region[1]) * rw + (x - c.region[0])) * 4], &whole[at], 4);
            else if (std::memcmp(&whole[at], &frameData[at], 4)) outsideUntouched = false;
        }
    if (production) {   // a control's mutant is allowed to drift; the build's shader is not
        char what[200];
        std::snprintf(what, sizeof(what), "%s: in place leaves every texel outside the region as it was", c.name);
        check(outsideUntouched, what);
    }
    return r;
}

// The equality test: the in-place composite is the out-of-place composite, byte for byte, on test images.
void testCompositeInPlace(Device& d) {
    checkCompositeBytecodeCurrent();
    if (!typedLoadOk(d, DXGI_FORMAT_R8G8B8A8_UNORM)) {
        std::puts("note: this adapter has no typed UAV load of R8G8B8A8_UNORM; the in-place composite is not taken here (ui_layer.cpp falls back), so its equality is not tested.");
        return;
    }
    ID3D11Device* dev = d.dev.Get();
    ComPtr<ID3D11ComputeShader> outCs = shaderFromBytecode(dev, edvr::kUiLayerCompositeBytecode, sizeof(edvr::kUiLayerCompositeBytecode));
    ComPtr<ID3D11ComputeShader> inCs = shaderFromBytecode(dev, edvr::kUiLayerCompositeInPlaceBytecode, sizeof(edvr::kUiLayerCompositeInPlaceBytecode));
    ComPtr<ID3D11ComputeShader> oldOutCs;
    {
        std::string old = edvr::kUiLayerCompositeHlsl;
        const size_t at = old.find("[numthreads(32, 8, 1)]");
        if (at == std::string::npos) { std::puts("FAIL: the out-of-place composite's group anchor is missing"); std::exit(1); }
        old.replace(at, std::strlen("[numthreads(32, 8, 1)]"), "[numthreads(8, 8, 1)]");
        oldOutCs = shaderFromSource(dev, old.c_str(), nullptr, "UI layer composite at 8x8");
    }
    static const CompCase cases[] = {
        // texW, texH, region, layer, uv, mult, mode, empty%
        {"1:1 layer", 61, 47, {0, 0, 61, 47}, 61, 47, {0, 0, 1, 1}, false, 0, 60},
        {"1:1, a layer nearly all ink", 61, 47, {0, 0, 61, 47}, 61, 47, {0, 0, 1, 1}, false, 0, 2},
        {"1:1, a layer that is empty", 61, 47, {0, 0, 61, 47}, 61, 47, {0, 0, 1, 1}, false, 0, 100},
        {"1.25x layer", 61, 47, {0, 0, 61, 47}, 76, 59, {0, 0, 1, 1}, false, 0, 50},
        {"1.56x layer", 64, 48, {0, 0, 64, 48}, 100, 75, {0, 0, 1, 1}, false, 0, 50},
        {"0.54x layer (the footprint under one texel)", 61, 47, {0, 0, 61, 47}, 33, 25, {0, 0, 1, 1}, false, 0, 40},
        {"cropped uv", 61, 47, {0, 0, 61, 47}, 80, 60, {0.1f, 0.2f, 0.9f, 0.95f}, false, 0, 50},
        {"a region inside a larger texture", 90, 70, {9, 7, 70, 54}, 76, 59, {0, 0, 1, 1}, false, 0, 50},
        {"width and height off the group sizes", 131, 67, {0, 0, 131, 67}, 131, 67, {0, 0, 1, 1}, false, 0, 55},
        {"multiply, 1:1", 61, 47, {0, 0, 61, 47}, 61, 47, {0, 0, 1, 1}, true, 0, 60},
        {"multiply, 1.25x", 61, 47, {0, 0, 61, 47}, 76, 59, {0, 0, 1, 1}, true, 0, 50},
        {"multiply over an empty layer", 61, 47, {0, 0, 61, 47}, 61, 47, {0, 0, 1, 1}, true, 0, 100},
        {"debug view (mode 1)", 61, 47, {0, 0, 61, 47}, 76, 59, {0, 0, 1, 1}, false, 1, 50},
        {"debug view with multiply", 61, 47, {0, 0, 61, 47}, 61, 47, {0, 0, 1, 1}, true, 1, 50},
    };
    // The controls: a shader that drifts from the composite must be caught by these same cases.
    struct Mutant { const char* name; const char* from; const char* to; };
    static const Mutant mutants[] = {
        {"drops the multiply", "l.rgb + f.rgb * l.a * m;", "l.rgb + f.rgb * l.a;"},
        {"skips when the layer is only mostly transparent", "l.a == 1.0 &&", "l.a > 0.5 &&"},
        {"ignores the region's origin", "int2 p = region.xy + int2(id.xy);", "int2 p = int2(id.xy);"},
        {"skips every texel", "if (mode == 0 && all(l.rgb == float3(0, 0, 0)) && l.a == 1.0 && all(m == float3(1, 1, 1))) return;", "return;"},
        {"stores the frame's alpha as 1", "Frame[p] = float4(saturate(c), f.a);", "Frame[p] = float4(saturate(c), 1.0);"},
    };
    ComPtr<ID3D11ComputeShader> mutantCs[std::size(mutants)];
    for (size_t i = 0; i < std::size(mutants); ++i) {
        std::string src = edvr::kUiLayerCompositeInPlaceHlsl;
        const size_t at = src.find(mutants[i].from);
        if (at == std::string::npos) { std::printf("FAIL: mutant anchor missing: %s\n", mutants[i].name); std::exit(1); }
        src.replace(at, std::strlen(mutants[i].from), mutants[i].to);
        mutantCs[i] = shaderFromSource(dev, src.c_str(), nullptr, mutants[i].name);
    }
    size_t caught[std::size(mutants)] = {};
    size_t texels = 0, differing = 0;
    for (const CompCase& c : cases) {
        const CompResult r = runComposite(d, c, outCs.Get(), inCs.Get(), true);
        char what[200];
        std::snprintf(what, sizeof(what), "%s: the in-place composite equals the out-of-place composite, byte for byte (%zu texels)", c.name, r.outOfPlace.size() / 4);
        check(r.inPlace == r.outOfPlace, what);
        {   // ...and the out-of-place shader at 32x8 is the one it was at 8x8 (before 2026-10-13)
            const CompResult o = runComposite(d, c, oldOutCs.Get(), inCs.Get(), false, 8, 8);
            std::snprintf(what, sizeof(what), "%s: the 32x8 out-of-place composite is the 8x8 one, byte for byte", c.name);
            check(o.outOfPlace == r.outOfPlace, what);
        }
        texels += r.outOfPlace.size() / 4;
        for (size_t i = 0; i < r.inPlace.size() && i < r.outOfPlace.size(); ++i) differing += r.inPlace[i] != r.outOfPlace[i];
        for (size_t i = 0; i < std::size(mutants); ++i) {
            const CompResult m = runComposite(d, c, outCs.Get(), mutantCs[i].Get(), false);
            if (m.inPlace != m.outOfPlace) ++caught[i];
        }
    }
    for (size_t i = 0; i < std::size(mutants); ++i) {
        char what[200];
        std::snprintf(what, sizeof(what), "CONTROL: an in-place shader that %s is caught (%zu of %zu cases differ)", mutants[i].name, caught[i], std::size(cases));
        check(caught[i] >= 1, what);
    }
    // An empty layer returns the frame untouched: the claim the skip rests on.
    {
        const CompCase& c = cases[2];
        Rng rng(0xC0FFEEu + static_cast<uint32_t>(c.texW * 131 + c.texH * 7 + c.lw));
        std::vector<uint8_t> frameData(static_cast<size_t>(c.texW) * c.texH * 4);
        for (auto& b : frameData) b = static_cast<uint8_t>(rng.next() >> 24);
        const CompResult r = runComposite(d, c, outCs.Get(), inCs.Get(), true);
        check(r.outOfPlace == frameData && r.inPlace == frameData, "a composite over an empty layer returns the frame byte for byte, in place and out of place");
    }
    std::printf("in place against out of place: %zu cases, %zu texels, %zu differing bytes\n", std::size(cases), texels, differing);
}

// A dual-source blend of the same composite onto the frame as a render target:
// layer.rgb + frame.rgb * (layer.a * mult), the multiply done by the blend unit.
// Only for the bench, as the one other way to avoid the copy: not exact (see its line).
static const char kBlendVs[] =
    "float4 main(uint id : SV_VertexID) : SV_POSITION { float2 p = id == 0 ? float2(-1,-1) : (id == 1 ? float2(-1,3) : float2(3,-1)); return float4(p,0,1); }";
static const char kBlendPs[] = R"HLSL(
Texture2D<float4> Layer : register(t1);
cbuffer P : register(b0) {
    int4   region; float4 uv; float2 layerSize; uint2 outSize; uint mode; uint useMult; uint2 pad;
};
struct O { float4 c0 : SV_Target0; float4 c1 : SV_Target1; };
O main(float4 pos : SV_POSITION) {
    uint2 id = uint2(pos.xy);
    float2 span = (uv.zw - uv.xy) * layerSize / float2(outSize);
    float2 x0 = uv.xy * layerSize + float2(id) * span;
    float2 x1 = x0 + span;
    int2 k0 = max(int2(floor(x0)), int2(0, 0));
    int2 k1 = min(int2(ceil(x1)) - 1, int2(layerSize) - 1);
    float4 acc = float4(0, 0, 0, 0);
    float wsum = 0;
    [loop] for (int j = k0.y; j <= min(k1.y, k0.y + 3); ++j) {
        float wy = min(x1.y, (float)j + 1.0) - max(x0.y, (float)j);
        if (wy <= 0) continue;
        [loop] for (int i = k0.x; i <= min(k1.x, k0.x + 3); ++i) {
            float wx = min(x1.x, (float)i + 1.0) - max(x0.x, (float)i);
            if (wx <= 0) continue;
            acc += Layer.Load(int3(i, j, 0)) * (wx * wy);
            wsum += wx * wy;
        }
    }
    float4 l = wsum > 0 ? acc / wsum : float4(0, 0, 0, 1);
    if (all(l.rgb == float3(0, 0, 0)) && l.a == 1.0) discard;
    O o; o.c0 = float4(l.rgb, 0); o.c1 = float4(l.a, l.a, l.a, 1); return o;
}
)HLSL";

// The panels of a HUD, as fractions of the eye, in the layer's own pixels.
static const float kHudBox[6][4] = {{0.06f, 0.62f, 0.22f, 0.90f}, {0.78f, 0.62f, 0.94f, 0.90f}, {0.42f, 0.78f, 0.58f, 0.96f},
                                    {0.03f, 0.25f, 0.09f, 0.45f}, {0.91f, 0.25f, 0.97f, 0.45f}, {0.44f, 0.06f, 0.56f, 0.12f}};

struct BenchSize {
    int fw, fh, lw, lh;
    const char* label;
};

void runCompositeBenchCase(Device& d, const BenchSize& cc) {
    ID3D11Device* dev = d.dev.Get();
    ID3D11DeviceContext* ctx = d.ctx.Get();
    const int fw = cc.fw, fh = cc.fh, lw = cc.lw, lh = cc.lh;

    std::vector<uint8_t> frameData(static_cast<size_t>(fw) * fh * 4);
    Rng rng(4242);
    for (auto& b : frameData) b = static_cast<uint8_t>(rng.next() >> 24);
    auto frame = tex(dev, fw, fh, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE, frameData.data(), fw * 4);
    auto out = tex(dev, fw, fh, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, nullptr, 0);
    // The in-place frame: what RCAS's output is (a UAV-capable texture EDVR owns), plus a render target for the blend variant.
    auto work = tex(dev, fw, fh, DXGI_FORMAT_R8G8B8A8_UNORM,
                    D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_RENDER_TARGET, frameData.data(), fw * 4);
    auto frameV = srvOf(dev, frame.Get());
    auto outU = uavOf(dev, out.Get());
    auto workU = uavOf(dev, work.Get());
    ComPtr<ID3D11RenderTargetView> workRtv;
    hr(dev->CreateRenderTargetView(work.Get(), nullptr, &workRtv), "frame RTV");

    struct Layer { ComPtr<ID3D11Texture2D> t; ComPtr<ID3D11ShaderResourceView> srv; ComPtr<ID3D11RenderTargetView> rtv; };
    auto makeLayer = [&]() {
        Layer l;
        l.t = tex(dev, lw, lh, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, nullptr, 0);
        l.srv = srvOf(dev, l.t.Get());
        hr(dev->CreateRenderTargetView(l.t.Get(), nullptr, &l.rtv), "layer RTV");
        return l;
    };
    Layer empty = makeLayer(), hud = makeLayer(), full = makeLayer();

    static const char kVs[] =
        "float4 main(uint id : SV_VertexID) : SV_POSITION { float2 p = id == 0 ? float2(-1,-1) : (id == 1 ? float2(-1,3) : float2(3,-1)); return float4(p,0,1); }";
    // Text-like content: a flat tinted panel with a sparse bright grid on it, alpha (transmittance) 0.35 on the panel.
    static const char kPs[] =
        "float4 main(float4 p : SV_POSITION) : SV_Target { uint2 q = uint2(p.xy); bool ink = ((q.x >> 2) ^ (q.y >> 3)) % 5 == 0; return ink ? float4(0.9, 0.95, 1.0, 0.0) : float4(0.20, 0.35, 0.55, 0.35); }";
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
    std::vector<D3D11_RECT> panels;
    for (const auto& b : kHudBox)
        panels.push_back({static_cast<LONG>(b[0] * lw), static_cast<LONG>(b[1] * lh), static_cast<LONG>(b[2] * lw), static_cast<LONG>(b[3] * lh)});
    paint(empty, {});
    paint(hud, panels);
    paint(full, {{0, 0, lw, lh}});

    ComPtr<ID3D11ComputeShader> cs = shaderFromBytecode(dev, edvr::kUiLayerCompositeBytecode, sizeof(edvr::kUiLayerCompositeBytecode));
    ComPtr<ID3D11ComputeShader> inplace = shaderFromBytecode(dev, edvr::kUiLayerCompositeInPlaceBytecode, sizeof(edvr::kUiLayerCompositeInPlaceBytecode));
    // The same two shaders at the other group shape (the bench's evidence that the launch, not the bytes, was the cost).
    auto reshaped = [&](const char* source, const char* from, const char* to) {
        std::string src = source;
        const size_t at = src.find(from);
        if (at == std::string::npos) { std::puts("FAIL: bench group anchor missing"); std::exit(1); }
        src.replace(at, std::strlen(from), to);
        return shaderFromSource(dev, src.c_str(), nullptr, "reshaped composite");
    };
    ComPtr<ID3D11ComputeShader> outNarrow = reshaped(edvr::kUiLayerCompositeHlsl, "[numthreads(32, 8, 1)]", "[numthreads(8, 8, 1)]");
    ComPtr<ID3D11ComputeShader> inNarrow = reshaped(edvr::kUiLayerCompositeInPlaceHlsl, "[numthreads(32, 8, 1)]", "[numthreads(8, 8, 1)]");
    // The upper bound for a tile-bounded dispatch: the same in-place shader over exactly the panels' rectangles
    // (the code does not know them; this is what perfect knowledge of the layer's content bounds could buy).
    ComPtr<ID3D11ComputeShader> oracle = reshaped(edvr::kUiLayerCompositeInPlaceHlsl, "void main(uint3 id : SV_DispatchThreadID) {",
                                                  "void main(uint3 gid : SV_DispatchThreadID) { uint3 id = uint3(gid.xy + pad, 0);");
    static const char kCopyCs[] =
        "Texture2D<float4> F : register(t0); RWTexture2D<float4> O : register(u0);"
        "[numthreads(8,8,1)] void main(uint3 id : SV_DispatchThreadID) { O[id.xy] = F.Load(int3(id.xy, 0)); }";
    ComPtr<ID3D11ComputeShader> copyCs = shaderFromSource(dev, kCopyCs, nullptr, "bare copy");

    ComPtr<ID3D11VertexShader> bvs;
    ComPtr<ID3D11PixelShader> bps;
    {
        ComPtr<ID3DBlob> v = compileBlob(kBlendVs, nullptr, "blend vs", "vs_5_0");
        hr(dev->CreateVertexShader(v->GetBufferPointer(), v->GetBufferSize(), nullptr, &bvs), "blend VS");
        ComPtr<ID3DBlob> p = compileBlob(kBlendPs, nullptr, "blend ps", "ps_5_0");
        hr(dev->CreatePixelShader(p->GetBufferPointer(), p->GetBufferSize(), nullptr, &bps), "blend PS");
    }
    D3D11_BLEND_DESC bld{};
    bld.RenderTarget[0].BlendEnable = TRUE;
    bld.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
    bld.RenderTarget[0].DestBlend = D3D11_BLEND_SRC1_COLOR;
    bld.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bld.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
    bld.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
    bld.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bld.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE;
    ComPtr<ID3D11BlendState> blend;
    hr(dev->CreateBlendState(&bld, &blend), "blend state");
    D3D11_RASTERIZER_DESC rd2{};
    rd2.FillMode = D3D11_FILL_SOLID;
    rd2.CullMode = D3D11_CULL_NONE;
    rd2.DepthClipEnable = TRUE;
    ComPtr<ID3D11RasterizerState> rs2;
    hr(dev->CreateRasterizerState(&rd2, &rs2), "rasterizer state 2");

    CompositeParams p{{0, 0, fw, fh}, {0.f, 0.f, 1.f, 1.f}, {static_cast<float>(lw), static_cast<float>(lh)}, {static_cast<uint32_t>(fw), static_cast<uint32_t>(fh)}, 0, 0, {0, 0}};
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = sizeof(CompositeParams);
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA pd{&p, 0, 0};
    ComPtr<ID3D11Buffer> cb;
    hr(dev->CreateBuffer(&bd, &pd, &cb), "composite cbuffer");
    // One constant buffer per panel for the oracle: the pad carries the panel's origin in the frame; the full-eye mapping is kept.
    std::vector<ComPtr<ID3D11Buffer>> panelCb;
    for (const auto& r : panels) {
        CompositeParams q = p;
        q.pad[0] = static_cast<uint32_t>(r.left * fw / lw);
        q.pad[1] = static_cast<uint32_t>(r.top * fh / lh);
        ComPtr<ID3D11Buffer> b;
        D3D11_SUBRESOURCE_DATA qd{&q, 0, 0};
        hr(dev->CreateBuffer(&bd, &qd, &b), "panel cbuffer");
        panelCb.push_back(b);
    }

    auto dispatchFull = [&](int gx, int gy) {
        ctx->Dispatch(static_cast<UINT>((fw + gx - 1) / gx), static_cast<UINT>((fh + gy - 1) / gy), 1);
    };
    auto compositeOut = [&](Layer& l, ID3D11ComputeShader* shader, int gx, int gy) {
        ID3D11ShaderResourceView* srvs[3] = {frameV.Get(), l.srv.Get(), nullptr};
        ID3D11UnorderedAccessView* uav = outU.Get();
        ID3D11Buffer* cbp = cb.Get();
        ctx->CSSetShader(shader, nullptr, 0);
        ctx->CSSetShaderResources(0, 3, srvs);
        ctx->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
        ctx->CSSetConstantBuffers(0, 1, &cbp);
        dispatchFull(gx, gy);
        ctx->ClearState();
    };
    auto compositeIn = [&](Layer& l, ID3D11ComputeShader* shader, int gx, int gy) {
        ID3D11ShaderResourceView* srvs[3] = {nullptr, l.srv.Get(), nullptr};
        ID3D11UnorderedAccessView* uav = workU.Get();
        ID3D11Buffer* cbp = cb.Get();
        ctx->CSSetShader(shader, nullptr, 0);
        ctx->CSSetShaderResources(0, 3, srvs);
        ctx->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
        ctx->CSSetConstantBuffers(0, 1, &cbp);
        dispatchFull(gx, gy);
        ctx->ClearState();
    };
    auto compositeBlend = [&](Layer& l) {
        ID3D11RenderTargetView* rtv = workRtv.Get();
        ctx->OMSetRenderTargets(1, &rtv, nullptr);
        const FLOAT bf[4] = {0, 0, 0, 0};
        ctx->OMSetBlendState(blend.Get(), bf, 0xFFFFFFFFu);
        D3D11_VIEWPORT vp{0.f, 0.f, static_cast<float>(fw), static_cast<float>(fh), 0.f, 1.f};
        ctx->RSSetViewports(1, &vp);
        ctx->RSSetState(rs2.Get());
        ctx->VSSetShader(bvs.Get(), nullptr, 0);
        ctx->PSSetShader(bps.Get(), nullptr, 0);
        ID3D11ShaderResourceView* srvs[2] = {nullptr, l.srv.Get()};
        ctx->PSSetShaderResources(0, 2, srvs);
        ID3D11Buffer* cbp = cb.Get();
        ctx->PSSetConstantBuffers(0, 1, &cbp);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->Draw(3, 0);
        ctx->ClearState();
    };
    auto compositePanels = [&](Layer& l) {
        ID3D11ShaderResourceView* srvs[3] = {nullptr, l.srv.Get(), nullptr};
        ID3D11UnorderedAccessView* uav = workU.Get();
        ctx->CSSetShader(oracle.Get(), nullptr, 0);
        ctx->CSSetShaderResources(0, 3, srvs);
        ctx->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
        for (size_t k = 0; k < panels.size(); ++k) {
            ID3D11Buffer* pc = panelCb[k].Get();
            ctx->CSSetConstantBuffers(0, 1, &pc);
            const UINT gx = static_cast<UINT>(((panels[k].right - panels[k].left) * fw / lw + 32) / 32);
            const UINT gy = static_cast<UINT>(((panels[k].bottom - panels[k].top) * fh / lh + 8) / 8);
            ctx->Dispatch(gx, gy, 1);
        }
        ctx->ClearState();
    };
    auto bareCopy = [&]() {
        ID3D11ShaderResourceView* srv = frameV.Get();
        ID3D11UnorderedAccessView* uav = outU.Get();
        ctx->CSSetShader(copyCs.Get(), nullptr, 0);
        ctx->CSSetShaderResources(0, 1, &srv);
        ctx->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
        dispatchFull(8, 8);
        ctx->ClearState();
    };
    const int gix = static_cast<int>(edvr::kUiLayerCompositeGroupX), giy = static_cast<int>(edvr::kUiLayerCompositeGroupY);

    // The equality half at the real size: the in-place shader over a fresh copy of the frame is the out-of-place output, byte for byte.
    const std::vector<std::pair<const char*, Layer*>> layers = {{"empty", &empty}, {"HUD panels", &hud}, {"covered", &full}};
    for (const auto& ly : layers) {
        compositeOut(*ly.second, outNarrow.Get(), 8, 8);   // the shader as it was before 2026-10-13: the reference
        const std::vector<uint8_t> want = readBytes(d, out.Get(), 4);
        ctx->UpdateSubresource(work.Get(), 0, nullptr, frameData.data(), static_cast<UINT>(fw * 4), 0);
        compositeIn(*ly.second, inplace.Get(), gix, giy);
        const std::vector<uint8_t> got = readBytes(d, work.Get(), 4);
        char what[160];
        std::snprintf(what, sizeof(what), "in place == out of place, byte for byte (layer %s, %dx%d over %dx%d)", ly.first, lw, lh, fw, fh);
        check(got == want, what);
        ctx->UpdateSubresource(work.Get(), 0, nullptr, frameData.data(), static_cast<UINT>(fw * 4), 0);
        compositeBlend(*ly.second);
        const std::vector<uint8_t> gotB = readBytes(d, work.Get(), 4);
        int maxDiff = 0;
        size_t differing = 0;
        for (size_t i = 0; i < want.size(); ++i) {
            const int df = std::abs(static_cast<int>(gotB[i]) - static_cast<int>(want[i]));
            if (df) ++differing;
            maxDiff = std::max(maxDiff, df);
        }
        std::printf("  blend variant vs out of place (layer %s): %zu bytes differ, max abs diff %d (the blend unit rounds its own way: not taken)\n",
                    ly.first, differing, maxDiff);
    }
    ctx->UpdateSubresource(work.Get(), 0, nullptr, frameData.data(), static_cast<UINT>(fw * 4), 0);

    std::vector<std::pair<std::string, std::function<void()>>> runs = {
        {"bare compute copy frame -> out (the floor)", bareCopy},
        {"CopyResource frame -> out", [&]() { ctx->CopyResource(out.Get(), frame.Get()); }},
    };
    for (const auto& ly : layers) {
        Layer* L = ly.second;
        const std::string nm = ly.first;
        runs.push_back({"(a0) before: out of place, 8x8 groups, layer " + nm, [&, L]() { compositeOut(*L, outNarrow.Get(), 8, 8); }});
        runs.push_back({"(a) out of place, 32x8 groups (the build), layer " + nm, [&, L]() { compositeOut(*L, cs.Get(), gix, giy); }});
        runs.push_back({"(b') in place, 8x8 groups, layer " + nm, [&, L]() { compositeIn(*L, inNarrow.Get(), 8, 8); }});
        runs.push_back({"(b) in place, 32x8 groups (the build), layer " + nm, [&, L]() { compositeIn(*L, inplace.Get(), gix, giy); }});
        runs.push_back({"(b2) in place by blend, not exact, layer " + nm, [&, L]() { compositeBlend(*L); }});
        if (L == &hud) runs.push_back({"(c) oracle: in place over the panels' rectangles only", [&, L]() { compositePanels(*L); }});
    }
    for (int i = 0; i < 40; ++i)
        for (const auto& r : runs) timeFn(d, 4, r.second);
    const int rounds = 201;
    std::vector<std::vector<double>> ms(runs.size());
    for (int round = 0; round < rounds; ++round)
        for (size_t k = 0; k < runs.size(); ++k) {
            const double t = timeFn(d, 8, runs[k].second);
            if (t > 0) ms[k].push_back(t);
        }
    std::printf("== %s: frame %dx%d RGBA8, layer %dx%d RGBA8; ms per call, one eye; median of %d samples of 8 dispatches, p10/p90 in brackets\n",
                cc.label, fw, fh, lw, lh, rounds);
    for (size_t k = 0; k < runs.size(); ++k) {
        std::vector<double> v = ms[k];
        std::sort(v.begin(), v.end());
        std::printf("  %-62s %8.4f  [%7.4f %7.4f]\n", runs[k].first.c_str(), median(ms[k]), v.empty() ? 0.0 : v[v.size() / 10], v.empty() ? 0.0 : v[v.size() * 9 / 10]);
    }
}

void runCompositeBench(Device& d) {
    if (d.warp) std::puts("note: --bench-composite on WARP times a CPU; use --adapter nvidia.");
    const bool loadOk = typedLoadOk(d, DXGI_FORMAT_R8G8B8A8_UNORM);
    std::printf("typed UAV load of R8G8B8A8_UNORM: %s\n", loadOk ? "supported" : "NOT supported");
    check(loadOk || d.warp, "the GPU loads typed R8G8B8A8_UNORM from a UAV");
    static const BenchSize cases[] = {
        {4032, 4032, 4032, 4032, "ui_quality 100 (layer = frame)"},
        {4032, 4032, 5040, 5040, "ui_quality 125 (layer 1.25x)"},
    };
    for (const auto& c : cases) runCompositeBenchCase(d, c);
}

// ------------------------------------------------------------------ thread-group shapes, at the eye's size
//
// ui_holo_pass_test --bench-groups. The composite's out-of-place shader launched 254,000 groups of 64 at
// 4032^2 and the launch, not the bytes, was its floor (0.120 ms against a 0.086 bare copy). The same
// experiment for the other full-frame compute passes: each shader's source rewritten to another group
// shape (or to 2x2 texels a thread), timed at the real size, and held byte for byte to the production
// shape's output. A pass whose threads share nothing (no groupshared) takes any shape; the table says
// which are worth taking.

struct GroupShape { int gx, gy, tx, ty; const char* name; };
static const GroupShape kGroupShapes[] = {
    {8, 8, 1, 1, "8x8"},
    {32, 8, 1, 1, "32x8"},
    {16, 16, 1, 1, "16x16"},
    {64, 4, 1, 1, "64x4"},
    {8, 8, 2, 2, "8x8, 2x2 texels a thread"},
    {16, 8, 2, 2, "16x8, 2x2 texels a thread"},
};

// `attr` is the shader's numthreads attribute as written and `sig` its entry's signature through the
// opening brace; the shape replaces the attribute, or (tiles above 1) turns the entry into a function
// that a new entry calls once for each texel it owns.
std::string reshapeShader(std::string src, const char* attr, const char* sig, const GroupShape& s) {
    const size_t sigAt = src.find(sig);
    if (sigAt == std::string::npos) { std::printf("FAIL: reshape: entry signature missing: %s\n", sig); std::exit(1); }
    const size_t attrAt = src.rfind(attr, sigAt);
    if (attrAt == std::string::npos) { std::printf("FAIL: reshape: numthreads missing: %s\n", attr); std::exit(1); }
    char nt[80];
    std::snprintf(nt, sizeof(nt), "[numthreads(%d, %d, 1)]", s.gx, s.gy);
    if (s.tx == 1 && s.ty == 1) {
        src.replace(sigAt, std::strlen(sig), sig);   // unchanged
        src.replace(attrAt, std::strlen(attr), nt);
        return src;
    }
    src.replace(sigAt, std::strlen(sig), "void body(uint3 id) {");
    src.replace(attrAt, std::strlen(attr), "");
    char wrapper[400];
    std::snprintf(wrapper, sizeof(wrapper),
                  "\n%s void main(uint3 gid : SV_DispatchThreadID) {\n"
                  "    [unroll] for (uint j = 0; j < %du; ++j) [unroll] for (uint i = 0; i < %du; ++i)\n"
                  "        body(uint3(gid.xy * uint2(%du, %du) + uint2(i, j), 0));\n}\n",
                  nt, s.ty, s.tx, s.tx, s.ty);
    return src + wrapper;
}
UINT groupsFor(int extent, const GroupShape& s, bool xAxis) {
    const int per = xAxis ? s.gx * s.tx : s.gy * s.ty;
    return static_cast<UINT>((extent + per - 1) / per);
}

void printMedians(const char* title, const std::vector<std::pair<std::string, std::function<void()>>>& runs, Device& d) {
    for (int i = 0; i < 40; ++i)
        for (const auto& r : runs) timeFn(d, 4, r.second);
    const int rounds = 201;
    std::vector<std::vector<double>> ms(runs.size());
    for (int round = 0; round < rounds; ++round)
        for (size_t k = 0; k < runs.size(); ++k) {
            const double t = timeFn(d, 8, runs[k].second);
            if (t > 0) ms[k].push_back(t);
        }
    std::printf("== %s; ms per dispatch, one eye; median of %d samples of 8, p10/p90 in brackets\n", title, rounds);
    const double base = median(ms[0]);
    for (size_t k = 0; k < runs.size(); ++k) {
        std::vector<double> v = ms[k];
        std::sort(v.begin(), v.end());
        std::printf("  %-58s %8.4f  [%7.4f %7.4f]  %+7.4f\n", runs[k].first.c_str(), median(ms[k]), v.empty() ? 0.0 : v[v.size() / 10],
                    v.empty() ? 0.0 : v[v.size() * 9 / 10], median(ms[k]) - base);
    }
}

// The out-of-place composite and its in-place twin, over the production layer (HUD panels), at both layer sizes.
void benchGroupsComposite(Device& d) {
    ID3D11Device* dev = d.dev.Get();
    ID3D11DeviceContext* ctx = d.ctx.Get();
    const int fw = 4032, fh = 4032;
    struct LayerSize { int w, h; const char* name; };
    static const LayerSize sizes[] = {{4032, 4032, "ui_quality 100"}, {5040, 5040, "ui_quality 125"}};
    static const char kVs[] =
        "float4 main(uint id : SV_VertexID) : SV_POSITION { float2 p = id == 0 ? float2(-1,-1) : (id == 1 ? float2(-1,3) : float2(3,-1)); return float4(p,0,1); }";
    static const char kPs[] =
        "float4 main(float4 p : SV_POSITION) : SV_Target { uint2 q = uint2(p.xy); bool ink = ((q.x >> 2) ^ (q.y >> 3)) % 5 == 0; return ink ? float4(0.9, 0.95, 1.0, 0.0) : float4(0.20, 0.35, 0.55, 0.35); }";
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    {
        ComPtr<ID3DBlob> v = compileBlob(kVs, nullptr, "vs", "vs_5_0");
        hr(dev->CreateVertexShader(v->GetBufferPointer(), v->GetBufferSize(), nullptr, &vs), "VS");
        ComPtr<ID3DBlob> p = compileBlob(kPs, nullptr, "ps", "ps_5_0");
        hr(dev->CreatePixelShader(p->GetBufferPointer(), p->GetBufferSize(), nullptr, &ps), "PS");
    }
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    rd.ScissorEnable = TRUE;
    ComPtr<ID3D11RasterizerState> rs;
    hr(dev->CreateRasterizerState(&rd, &rs), "rasterizer state");
    std::vector<uint8_t> frameData(static_cast<size_t>(fw) * fh * 4);
    Rng rng(777);
    for (auto& b : frameData) b = static_cast<uint8_t>(rng.next() >> 24);
    auto frame = tex(dev, fw, fh, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE, frameData.data(), fw * 4);
    auto frameV = srvOf(dev, frame.Get());
    auto out = tex(dev, fw, fh, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, nullptr, 0);
    auto outU = uavOf(dev, out.Get());
    auto work = tex(dev, fw, fh, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, frameData.data(), fw * 4);
    auto workU = uavOf(dev, work.Get());
    for (const LayerSize& ls : sizes) {
        auto layer = tex(dev, ls.w, ls.h, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, nullptr, 0);
        auto layerV = srvOf(dev, layer.Get());
        ComPtr<ID3D11RenderTargetView> rtv;
        hr(dev->CreateRenderTargetView(layer.Get(), nullptr, &rtv), "layer RTV");
        const FLOAT clear[4] = {0.f, 0.f, 0.f, 1.f};
        ctx->ClearRenderTargetView(rtv.Get(), clear);
        ID3D11RenderTargetView* r0 = rtv.Get();
        ctx->OMSetRenderTargets(1, &r0, nullptr);
        D3D11_VIEWPORT vp{0.f, 0.f, static_cast<float>(ls.w), static_cast<float>(ls.h), 0.f, 1.f};
        ctx->RSSetViewports(1, &vp);
        ctx->RSSetState(rs.Get());
        ctx->VSSetShader(vs.Get(), nullptr, 0);
        ctx->PSSetShader(ps.Get(), nullptr, 0);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        for (const auto& b : kHudBox) {
            D3D11_RECT r{static_cast<LONG>(b[0] * ls.w), static_cast<LONG>(b[1] * ls.h), static_cast<LONG>(b[2] * ls.w), static_cast<LONG>(b[3] * ls.h)};
            ctx->RSSetScissorRects(1, &r);
            ctx->Draw(3, 0);
        }
        ctx->ClearState();
        CompositeParams p{{0, 0, fw, fh}, {0.f, 0.f, 1.f, 1.f}, {static_cast<float>(ls.w), static_cast<float>(ls.h)},
                          {static_cast<uint32_t>(fw), static_cast<uint32_t>(fh)}, 0, 0, {0, 0}};
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = sizeof(p);
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        D3D11_SUBRESOURCE_DATA pd{&p, 0, 0};
        ComPtr<ID3D11Buffer> cb;
        hr(dev->CreateBuffer(&bd, &pd, &cb), "cbuffer");

        std::vector<std::pair<std::string, std::function<void()>>> runs;
        std::vector<std::vector<uint8_t>> outOfPlaceBytes, inPlaceBytes;
        std::vector<ComPtr<ID3D11ComputeShader>> keep;
        for (int inPlace = 0; inPlace < 2; ++inPlace) {
            for (const GroupShape& sh : kGroupShapes) {
                // The in-place shader as built is 32x8; its baseline for the table is its own shape.
                const char* attr = "[numthreads(32, 8, 1)]";   // both composites are 32x8 as built
                const std::string src = reshapeShader(inPlace ? edvr::kUiLayerCompositeInPlaceHlsl : edvr::kUiLayerCompositeHlsl, attr,
                                                      "void main(uint3 id : SV_DispatchThreadID) {", sh);
                ComPtr<ID3D11ComputeShader> cs = shaderFromSource(dev, src.c_str(), nullptr, "reshaped composite");
                keep.push_back(cs);
                ID3D11ComputeShader* csp = cs.Get();
                const GroupShape shc = sh;
                auto run = [&, csp, shc, inPlace]() {
                    ID3D11ShaderResourceView* srvs[3] = {inPlace ? nullptr : frameV.Get(), layerV.Get(), nullptr};
                    ID3D11UnorderedAccessView* uav = inPlace ? workU.Get() : outU.Get();
                    ID3D11Buffer* cbp = cb.Get();
                    ctx->CSSetShader(csp, nullptr, 0);
                    ctx->CSSetShaderResources(0, 3, srvs);
                    ctx->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
                    ctx->CSSetConstantBuffers(0, 1, &cbp);
                    ctx->Dispatch(groupsFor(fw, shc, true), groupsFor(fh, shc, false), 1);
                    ctx->ClearState();
                };
                // Exactness first: this shape's bytes against the first (8x8, out of place) shape's.
                if (inPlace) ctx->UpdateSubresource(work.Get(), 0, nullptr, frameData.data(), static_cast<UINT>(fw * 4), 0);
                run();
                (inPlace ? inPlaceBytes : outOfPlaceBytes).push_back(readBytes(d, inPlace ? work.Get() : out.Get(), 4));
                if (inPlace) ctx->UpdateSubresource(work.Get(), 0, nullptr, frameData.data(), static_cast<UINT>(fw * 4), 0);
                char nm[160];
                std::snprintf(nm, sizeof(nm), "%s, %s, layer %dx%d", inPlace ? "in place" : "out of place", sh.name, ls.w, ls.h);
                const auto& ref = outOfPlaceBytes[0];
                const auto& got = (inPlace ? inPlaceBytes : outOfPlaceBytes).back();
                char what[220];
                std::snprintf(what, sizeof(what), "composite %s: byte for byte the 8x8 out-of-place output", nm);
                check(got == ref, what);
                runs.push_back({nm, run});
            }
        }
        char title[120];
        std::snprintf(title, sizeof(title), "UI layer composite, frame %dx%d, %s (HUD panels)", fw, fh, ls.name);
        printMedians(title, runs, d);
    }
}

// RCAS, as sharpen_pass.cpp dispatches it: a clamped 3x3-cross read of the source, one thread a texel.
void benchGroupsRcas(Device& d) {
    ID3D11Device* dev = d.dev.Get();
    ID3D11DeviceContext* ctx = d.ctx.Get();
    struct RcasCb { uint32_t con[4]; int32_t region[4]; int32_t outSize[2]; int32_t pad0[2]; };
    static_assert(sizeof(RcasCb) == 48, "the RCAS cbuffer is three rows");
    struct Case { int w, h, rx, ry, rw, rh; const char* name; };
    // The eye's size; and an odd region inside a larger texture, to hold the clamp at the region's edge for every shape.
    static const Case cases[] = {{4032, 4032, 0, 0, 4032, 4032, "4032x4032 (the eye)"}, {4032, 3896, 0, 0, 4032, 3896, "4032x3896 (the carrier flight's eye)"},
                                 {131, 67, 3, 2, 120, 60, "131x67 texture, region 120x60 at (3,2): the clamp at the region's edge"}};
    for (const Case& c : cases) {
        std::vector<uint8_t> data(static_cast<size_t>(c.w) * c.h * 4);
        Rng rng(31337);
        for (auto& b : data) b = static_cast<uint8_t>(rng.next() >> 24);
        auto src = tex(dev, c.w, c.h, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE, data.data(), c.w * 4);
        auto srcV = srvOf(dev, src.Get());
        auto dst = tex(dev, c.rw, c.rh, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, nullptr, 0);
        auto dstU = uavOf(dev, dst.Get());
        RcasCb p{};
        FsrRcasCon(reinterpret_cast<AU1*>(p.con), static_cast<AF1>(1.4f));   // fix.render_sharpness 0.3: 2 * (1 - 0.3) stops
        p.region[0] = c.rx; p.region[1] = c.ry; p.region[2] = c.rx + c.rw; p.region[3] = c.ry + c.rh;
        p.outSize[0] = c.rw; p.outSize[1] = c.rh;
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = sizeof(p);
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        D3D11_SUBRESOURCE_DATA pd{&p, 0, 0};
        ComPtr<ID3D11Buffer> cb;
        hr(dev->CreateBuffer(&bd, &pd, &cb), "cbuffer");
        std::vector<std::pair<std::string, std::function<void()>>> runs;
        std::vector<ComPtr<ID3D11ComputeShader>> keep;
        std::vector<uint8_t> want;
        for (const GroupShape& sh : kGroupShapes) {
            const std::string text = reshapeShader(edvr::fixed_extra_source::sharpen_pass::kRcasSource, "[numthreads(16,16,1)]",
                                                   "void main(uint3 id : SV_DispatchThreadID) {", sh);
            ComPtr<ID3D11ComputeShader> cs = shaderFromSource(dev, text.c_str(), nullptr, "reshaped RCAS");
            keep.push_back(cs);
            ID3D11ComputeShader* csp = cs.Get();
            const GroupShape shc = sh;
            auto run = [&, csp, shc]() {
                ID3D11ShaderResourceView* sv = srcV.Get();
                ID3D11UnorderedAccessView* uav = dstU.Get();
                ID3D11Buffer* cbp = cb.Get();
                ctx->CSSetShader(csp, nullptr, 0);
                ctx->CSSetShaderResources(0, 1, &sv);
                ctx->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
                ctx->CSSetConstantBuffers(0, 1, &cbp);
                ctx->Dispatch(groupsFor(c.rw, shc, true), groupsFor(c.rh, shc, false), 1);
                ctx->ClearState();
            };
            const FLOAT poison[4] = {1.f, 0.f, 1.f, 0.03f};
            ctx->ClearUnorderedAccessViewFloat(dstU.Get(), poison);
            run();
            const std::vector<uint8_t> got = readBytes(d, dst.Get(), 4);
            if (want.empty()) want = got;
            char what[220];
            std::snprintf(what, sizeof(what), "RCAS %s, %s: byte for byte the 8x8 output", c.name, sh.name);
            check(got == want, what);
            runs.push_back({sh.name, run});
        }
        char title[120];
        std::snprintf(title, sizeof(title), "RCAS sharpen, %s", c.name);
        printMedians(title, runs, d);
    }
}

// The UI resolve, on the input grid (2016x1948 in, 4032x3896 out), with the carrier flight's bindings.
void benchGroupsUiResolve(Device& d) {
    ID3D11Device* dev = d.dev.Get();
    ID3D11DeviceContext* ctx = d.ctx.Get();
    const float tol = 12 / 255.f, hold = 64 / 255.f;
    constexpr uint32_t kCover = 1u << 2, kHistory = 1u << 3, kEdits = 1u << 5;
    (void)kCover; (void)kHistory;
    struct Scene { Fix f; uint32_t unbind; const char* name; };
    const std::vector<Scene> scenes = {
        {{"idle, everything bound", 201, 2016, 1948, 4032, 3896, 0.25f, -0.4f, tol, hold, 3, 10, 3, 1, 0, 0, -1, 0}, 0, "idle, everything bound"},
        {{"HUD (15% marked), edit mask unbound (a still HUD)", 202, 2016, 1948, 4032, 3896, 0.25f, -0.4f, tol, hold, 3, 9, 4, 1, 0, 0, -1, 0}, kEdits, "HUD, source-edit mask unbound"},
    };
    for (const Scene& scene : scenes) {
        const Fix& f = scene.f;
        const Inputs in = makeInputs(f);
        Setup s;
        makeSetup(d, f, in, Hist::RgbaA, s, scene.unbind);
        std::vector<std::pair<std::string, std::function<void()>>> runs;
        std::vector<ComPtr<ID3D11ComputeShader>> keep;
        Result want;
        bool haveWant = false;
        for (const GroupShape& sh : kGroupShapes) {
            const std::string text = reshapeShader(edvr::kUiResolve, "[numthreads(8,8,1)]", "void main(uint3 id:SV_DispatchThreadID){", sh);
            ComPtr<ID3D11ComputeShader> cs = shaderFromSource(dev, text.c_str(), nullptr, "reshaped UI resolve");
            keep.push_back(cs);
            ID3D11ComputeShader* csp = cs.Get();
            const GroupShape shc = sh;
            auto run = [&, csp, shc]() {
                bindAndClear(d, csp, s);
                ctx->Dispatch(groupsFor(f.w, shc, true), groupsFor(f.h, shc, false), 1);
                ctx->ClearState();
            };
            run();
            const Result got = readback(d, s);
            if (!haveWant) { want = got; haveWant = true; }
            char what[220];
            std::snprintf(what, sizeof(what), "UI resolve (%s), %s: byte for byte the 8x8 output and history", scene.name, sh.name);
            check(compare(want, got).same(), what);
            runs.push_back({sh.name, run});
        }
        char title[160];
        std::snprintf(title, sizeof(title), "UI resolve, %dx%d in, %dx%d out, %s", f.w, f.h, f.ow, f.oh, scene.name);
        printMedians(title, runs, d);
    }
}

// The hologram resolve's near-light pass, as ui_depth.cpp dispatches it: one 8x8 block a group, one output texel a block
// (the map is 1/8 resolution). A wider group keeps that: it holds several blocks, each with its own flag and its own texel.
std::string nearLightShaped(int gx, int gy) {
    std::string src = edvr_reference::kNearLightReference;   // one 8x8 block a group, as it ran before 2026-10-13
    if (gx == 8 && gy == 8) return src;
    const int bx = gx / 8, by = gy / 8, nb = bx * by;
    auto swap = [&](const std::string& from, const std::string& to) {
        const size_t at = src.find(from);
        if (at == std::string::npos) { std::printf("FAIL: near-light anchor missing: %s\n", from.c_str()); std::exit(1); }
        src.replace(at, from.size(), to);
    };
    char t[400];
    std::snprintf(t, sizeof(t), "groupshared uint lightAny[%d];", nb);
    swap("groupshared uint lightAny;", t);
    std::snprintf(t, sizeof(t), "[numthreads(%d,%d,1)]", gx, gy);
    swap("[numthreads(8,8,1)]", t);
    std::snprintf(t, sizeof(t), "uint blk = (tid.y >> 3) * %du + (tid.x >> 3); uint2 loc = tid.xy & 7u; if (all(loc == uint2(0, 0))) lightAny[blk] = 0;", bx);
    swap("if (all(tid.xy == uint2(0, 0))) lightAny = 0;", t);
    swap("InterlockedOr(lightAny, 1u);", "InterlockedOr(lightAny[blk], 1u);");
    std::snprintf(t, sizeof(t), "if (all(loc == uint2(0, 0))) NearLight[gid.xy * uint2(%du, %du) + uint2(blk %% %du, blk / %du)] = lightAny[blk] ? 1.0 : 0.0;", bx, by, bx, bx);
    swap("if (all(tid.xy == uint2(0, 0))) NearLight[gid.xy] = lightAny ? 1.0 : 0.0;", t);
    return src;
}

void benchGroupsNearLight(Device& d) {
    ID3D11Device* dev = d.dev.Get();
    ID3D11DeviceContext* ctx = d.ctx.Get();
    const int w = 2016, h = 1948;
    Rng rng(5150);
    std::vector<uint16_t> contrib(static_cast<size_t>(w) * h * 4, 0);
    std::vector<float> depth(static_cast<size_t>(w) * h, 0.f);
    // Three elements an eye (the flight's count), straddling block edges.
    static const int rects[3][4] = {{301, 205, 477, 351}, {1203, 899, 1259, 925}, {1660, 1500, 1789, 1545}};
    static const uint16_t halves[3] = {0x0000, 0x34CD, 0x3800};
    for (const auto& r : rects)
        for (int y = r[1]; y < r[3]; ++y)
            for (int x = r[0]; x < r[2]; ++x) {
                depth[static_cast<size_t>(y) * w + x] = 0.9f;
                for (int c = 0; c < 3; ++c) contrib[(static_cast<size_t>(y) * w + x) * 4 + c] = halves[(rng.next() >> 20) % 3];
            }
    std::vector<uint8_t> targetData(static_cast<size_t>(w) * h * 4), displayData(static_cast<size_t>(w) * h * 4);
    for (auto& b : targetData) b = static_cast<uint8_t>(rng.next() >> 24);
    for (auto& b : displayData) b = static_cast<uint8_t>(rng.next() >> 24);
    auto tContrib = tex(dev, w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_SHADER_RESOURCE, contrib.data(), w * 8);
    auto tDepth = tex(dev, w, h, DXGI_FORMAT_R32_FLOAT, D3D11_BIND_SHADER_RESOURCE, depth.data(), w * 4);
    auto tTarget = tex(dev, w, h, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE, targetData.data(), w * 4);
    auto tDisplay = tex(dev, w, h, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE, displayData.data(), w * 4);
    auto tOut = tex(dev, (w + 7) / 8, (h + 7) / 8, DXGI_FORMAT_R8_UNORM, D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE, nullptr, 0);
    ComPtr<ID3D11ShaderResourceView> sv[4] = {srvOf(dev, tContrib.Get()), srvOf(dev, tDepth.Get()), srvOf(dev, tTarget.Get()), srvOf(dev, tDisplay.Get())};
    auto outU = uavOf(dev, tOut.Get());
    struct Cb { float floorValue, share; uint32_t flags; float radiusDepth, fillerDepth, p0, p1, p2; } cbData{0.5f, 0.5f, 2u | 4u, 0.f, 0.f, 0, 0, 0};
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = sizeof(cbData);
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA pd{&cbData, 0, 0};
    ComPtr<ID3D11Buffer> cb;
    hr(dev->CreateBuffer(&bd, &pd, &cb), "cbuffer");
    struct Shape { int gx, gy; const char* name; };
    static const Shape shapes[] = {{8, 8, "8x8 (the shader as it is), one block a group"}, {16, 8, "16x8, two blocks a group"}, {32, 8, "32x8, four blocks a group"},
                                   {16, 16, "16x16, four blocks a group"}, {32, 16, "32x16, eight blocks a group"}};
    std::vector<std::pair<std::string, std::function<void()>>> runs;
    std::vector<ComPtr<ID3D11ComputeShader>> keep;
    std::vector<uint8_t> want;
    for (const Shape& sh : shapes) {
        const std::string text = nearLightShaped(sh.gx, sh.gy);
        ComPtr<ID3D11ComputeShader> cs = shaderFromSource(dev, text.c_str(), nullptr, "near-light");
        keep.push_back(cs);
        ID3D11ComputeShader* csp = cs.Get();
        const Shape shc = sh;
        auto run = [&, csp, shc]() {
            ID3D11ShaderResourceView* in[4] = {sv[0].Get(), sv[1].Get(), sv[2].Get(), sv[3].Get()};
            ID3D11UnorderedAccessView* uav = outU.Get();
            ID3D11Buffer* cbp = cb.Get();
            ctx->CSSetShader(csp, nullptr, 0);
            ctx->CSSetShaderResources(0, 4, in);
            ctx->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
            ctx->CSSetConstantBuffers(0, 1, &cbp);
            ctx->Dispatch(static_cast<UINT>((w + shc.gx - 1) / shc.gx), static_cast<UINT>((h + shc.gy - 1) / shc.gy), 1);
            ctx->ClearState();
        };
        const FLOAT poison[4] = {0.5f, 0, 0, 0};
        ctx->ClearUnorderedAccessViewFloat(outU.Get(), poison);
        run();
        const std::vector<uint8_t> got = readBytes(d, tOut.Get(), 1);
        if (want.empty()) {
            want = got;
            size_t lit = 0;
            for (uint8_t b : got) lit += b == 255;
            check(lit > 0 && lit < got.size() / 4, "near-light bench: the map has lit blocks and dark ones (the equality below is not vacuous)");
        }
        char what[200];
        std::snprintf(what, sizeof(what), "near-light %s: byte for byte the 8x8 map", sh.name);
        check(got == want, what);
        runs.push_back({sh.name, run});
    }
    printMedians("hologram resolve near-light compute, 2016x1948, 3 elements an eye", runs, d);
}

// The group-shape changes of 2026-10-13, held to the shaders as they were: the production bytecode (RCAS at 16x16, the near-light
// pass at four blocks a group) against the old shape of the same text, byte for byte, on odd sizes and offset regions; and a
// mutant of each that must fail, so a green run cannot be vacuous.
void testGroupShapes(Device& d) {
    ID3D11Device* dev = d.dev.Get();
    ID3D11DeviceContext* ctx = d.ctx.Get();
    const GroupShape oldShape{8, 8, 1, 1, "8x8"};

    // ---- RCAS
    {
        const std::string& text = edvr::fixed_extra_source::sharpen_pass::kRcasSource;
        ComPtr<ID3DBlob> code = compileBlob(text.c_str(), nullptr, "render_sharpen_cs");
        check(code->GetBufferSize() == sizeof(edvr::kSharpenBytecode) &&
                  !std::memcmp(code->GetBufferPointer(), edvr::kSharpenBytecode, sizeof(edvr::kSharpenBytecode)),
              "the RCAS bytecode header is current: kRcasSource compiled, byte for byte");
        ComPtr<ID3D11ComputeShader> prod = shaderFromBytecode(dev, edvr::kSharpenBytecode, sizeof(edvr::kSharpenBytecode));
        const char* attr = "[numthreads(16,16,1)]";
        const char* sig = "void main(uint3 id : SV_DispatchThreadID) {";
        ComPtr<ID3D11ComputeShader> old = shaderFromSource(dev, reshapeShader(text, attr, sig, oldShape).c_str(), nullptr, "RCAS at 8x8");
        struct Mut { const char* name; const char* from; const char* to; };
        static const Mut muts[] = {
            {"clamps to the texture, not the region", "clamp(int2(p), region.xy, region.zw - 1)", "clamp(int2(p), int2(0, 0), int2(100000, 100000))"},
            {"drops the alpha the source carried", "Src.Load(int3(ip, 0)).a", "1.0"},
            {"leaves the last row and column to nobody", "id.x >= (uint)outSize.x || id.y >= (uint)outSize.y", "id.x + 1 >= (uint)outSize.x || id.y + 1 >= (uint)outSize.y"},
        };
        ComPtr<ID3D11ComputeShader> mutCs[std::size(muts)];
        for (size_t i = 0; i < std::size(muts); ++i) {
            std::string m = text;
            const size_t at = m.find(muts[i].from);
            if (at == std::string::npos) { std::printf("FAIL: RCAS mutant anchor missing: %s\n", muts[i].name); std::exit(1); }
            m.replace(at, std::strlen(muts[i].from), muts[i].to);
            mutCs[i] = shaderFromSource(dev, m.c_str(), nullptr, muts[i].name);
        }
        struct RcasCb { uint32_t con[4]; int32_t region[4]; int32_t outSize[2]; int32_t pad0[2]; };
        struct Case { int w, h, rx, ry, rw, rh; float stops; };
        static const Case cases[] = {
            {61, 47, 0, 0, 61, 47, 1.4f}, {131, 67, 3, 2, 120, 60, 1.4f}, {17, 9, 0, 0, 17, 9, 0.4f}, {256, 256, 0, 0, 256, 256, 1.4f},
            {33, 31, 1, 1, 32, 30, 0.4f}, {16, 16, 0, 0, 16, 16, 1.4f}, {15, 17, 0, 0, 15, 17, 1.0f}, {300, 20, 40, 3, 200, 14, 1.4f},
        };
        size_t caught[std::size(muts)] = {}, texels = 0;
        for (const Case& c : cases) {
            Rng rng(0xBEEFu + static_cast<uint32_t>(c.w * 7 + c.h));
            std::vector<uint8_t> data(static_cast<size_t>(c.w) * c.h * 4);
            for (auto& b : data) b = static_cast<uint8_t>(rng.next() >> 24);
            auto src = tex(dev, c.w, c.h, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE, data.data(), c.w * 4);
            auto srcV = srvOf(dev, src.Get());
            RcasCb p{};
            FsrRcasCon(reinterpret_cast<AU1*>(p.con), static_cast<AF1>(c.stops));
            p.region[0] = c.rx; p.region[1] = c.ry; p.region[2] = c.rx + c.rw; p.region[3] = c.ry + c.rh;
            p.outSize[0] = c.rw; p.outSize[1] = c.rh;
            D3D11_BUFFER_DESC bd{};
            bd.ByteWidth = sizeof(p);
            bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            D3D11_SUBRESOURCE_DATA pd{&p, 0, 0};
            ComPtr<ID3D11Buffer> cb;
            hr(dev->CreateBuffer(&bd, &pd, &cb), "cbuffer");
            auto runOne = [&](ID3D11ComputeShader* cs, int gx, int gy) {
                auto dst = tex(dev, c.rw, c.rh, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, nullptr, 0);
                auto dstU = uavOf(dev, dst.Get());
                const FLOAT poison[4] = {1.f, 0.f, 1.f, 0.03f};
                ctx->ClearUnorderedAccessViewFloat(dstU.Get(), poison);
                ID3D11ShaderResourceView* sv = srcV.Get();
                ID3D11UnorderedAccessView* uav = dstU.Get();
                ID3D11Buffer* cbp = cb.Get();
                ctx->CSSetShader(cs, nullptr, 0);
                ctx->CSSetShaderResources(0, 1, &sv);
                ctx->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
                ctx->CSSetConstantBuffers(0, 1, &cbp);
                ctx->Dispatch(static_cast<UINT>((c.rw + gx - 1) / gx), static_cast<UINT>((c.rh + gy - 1) / gy), 1);
                ctx->ClearState();
                return readBytes(d, dst.Get(), 4);
            };
            const std::vector<uint8_t> want = runOne(old.Get(), 8, 8);
            const std::vector<uint8_t> got = runOne(prod.Get(), 16, 16);
            char what[200];
            std::snprintf(what, sizeof(what), "RCAS %dx%d, region %dx%d at (%d,%d), %.1f stops: the 16x16 shader is the 8x8 shader, byte for byte", c.w, c.h, c.rw, c.rh, c.rx, c.ry, static_cast<double>(c.stops));
            check(got == want, what);
            texels += got.size() / 4;
            for (size_t i = 0; i < std::size(muts); ++i)
                if (runOne(mutCs[i].Get(), 16, 16) != want) ++caught[i];
        }
        for (size_t i = 0; i < std::size(muts); ++i) {
            char what[200];
            std::snprintf(what, sizeof(what), "CONTROL: an RCAS shader that %s is caught (%zu of %zu cases differ)", muts[i].name, caught[i], std::size(cases));
            check(caught[i] >= 1, what);
        }
        std::printf("RCAS 16x16 against 8x8: %zu cases, %zu texels\n", std::size(cases), texels);
    }

    // ---- the near-light pass
    {
        ComPtr<ID3DBlob> code = compileBlob(edvr::kHoloNearLightCsHlsl, nullptr, "ui_depth_holo_near_light_cs");
        check(code->GetBufferSize() == sizeof(edvr::kUiHoloNearLightBytecode) &&
                  !std::memcmp(code->GetBufferPointer(), edvr::kUiHoloNearLightBytecode, sizeof(edvr::kUiHoloNearLightBytecode)),
              "the near-light bytecode header is current: kHoloNearLightCsHlsl compiled, byte for byte");
        ComPtr<ID3D11ComputeShader> prod = shaderFromBytecode(dev, edvr::kUiHoloNearLightBytecode, sizeof(edvr::kUiHoloNearLightBytecode));
        ComPtr<ID3D11ComputeShader> old = shaderFromSource(dev, edvr_reference::kNearLightReference, nullptr, "near-light, one block a group");
        struct Mut { const char* name; const char* from; const char* to; };
        static const Mut muts[] = {
            {"gives every block in a group the one flag", "uint blk = (tid.y >> 3) * 2u + (tid.x >> 3);", "uint blk = 0u;"},
            {"writes each group's texel unscaled", "NearLight[gid.xy * 2u + uint2(blk & 1u, blk >> 1u)]", "NearLight[gid.xy]"},
            {"swaps the blocks' rows and columns", "NearLight[gid.xy * 2u + uint2(blk & 1u, blk >> 1u)]", "NearLight[gid.xy * 2u + uint2(blk >> 1u, blk & 1u)]"},
        };
        ComPtr<ID3D11ComputeShader> mutCs[std::size(muts)];
        for (size_t i = 0; i < std::size(muts); ++i) {
            std::string m = edvr::kHoloNearLightCsHlsl;
            const size_t at = m.find(muts[i].from);
            if (at == std::string::npos) { std::printf("FAIL: near-light mutant anchor missing: %s\n", muts[i].name); std::exit(1); }
            m.replace(at, std::strlen(muts[i].from), muts[i].to);
            mutCs[i] = shaderFromSource(dev, m.c_str(), nullptr, muts[i].name);
        }
        struct Case { int w, h; uint32_t flags; };
        // Odd counts of 8x8 blocks (200 = 25 blocks, 136 = 17) leave the last group's far half past the map; flags run the
        // linear blend, the target's share test, the display floor and its sRGB decode.
        static const Case cases[] = {
            {200, 136, 2u | 4u}, {197, 131, 4u}, {200, 136, 1u}, {128, 96, 2u | 4u | 8u}, {64, 64, 0u}, {9, 9, 2u | 4u}, {16, 16, 4u | 8u}, {2016, 1948, 2u | 4u},
        };
        size_t caught[std::size(muts)] = {}, blocks = 0, lit = 0;
        for (const Case& c : cases) {
            Rng rng(0x51CEu + static_cast<uint32_t>(c.w * 3 + c.h));
            std::vector<uint16_t> contrib(static_cast<size_t>(c.w) * c.h * 4, 0);
            std::vector<float> depth(static_cast<size_t>(c.w) * c.h, 0.f);
            static const uint16_t halves[4] = {0x0000, 0x34CD, 0x3800, 0x3B33};
            for (size_t i = 0; i < depth.size(); ++i)
                if ((rng.next() >> 8) % 150u == 0u) {
                    depth[i] = 0.9f;
                    for (int ch = 0; ch < 3; ++ch) contrib[i * 4 + ch] = halves[(rng.next() >> 20) % 4];
                }
            std::vector<uint8_t> targetData(static_cast<size_t>(c.w) * c.h * 4), displayData(static_cast<size_t>(c.w) * c.h * 4);
            for (auto& b : targetData) b = static_cast<uint8_t>(rng.next() >> 24);
            for (auto& b : displayData) b = static_cast<uint8_t>(rng.next() >> 24);
            auto tContrib = tex(dev, c.w, c.h, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_SHADER_RESOURCE, contrib.data(), c.w * 8);
            auto tDepth = tex(dev, c.w, c.h, DXGI_FORMAT_R32_FLOAT, D3D11_BIND_SHADER_RESOURCE, depth.data(), c.w * 4);
            auto tTarget = tex(dev, c.w, c.h, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE, targetData.data(), c.w * 4);
            auto tDisplay = tex(dev, c.w, c.h, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE, displayData.data(), c.w * 4);
            const int mw = (c.w + 7) / 8, mh = (c.h + 7) / 8;
            ComPtr<ID3D11ShaderResourceView> sv[4] = {srvOf(dev, tContrib.Get()), srvOf(dev, tDepth.Get()), srvOf(dev, tTarget.Get()), srvOf(dev, tDisplay.Get())};
            struct Cb { float floorValue, share; uint32_t flags; float radiusDepth, fillerDepth, p0, p1, p2; } cbData{0.5f, 0.5f, c.flags, 0.f, 0.f, 0, 0, 0};
            D3D11_BUFFER_DESC bd{};
            bd.ByteWidth = sizeof(cbData);
            bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            D3D11_SUBRESOURCE_DATA pd{&cbData, 0, 0};
            ComPtr<ID3D11Buffer> cb;
            hr(dev->CreateBuffer(&bd, &pd, &cb), "cbuffer");
            auto runOne = [&](ID3D11ComputeShader* cs, int gx, int gy) {
                auto out = tex(dev, mw, mh, DXGI_FORMAT_R8_UNORM, D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE, nullptr, 0);
                auto outU = uavOf(dev, out.Get());
                const FLOAT poison[4] = {0.5f, 0, 0, 0};
                ctx->ClearUnorderedAccessViewFloat(outU.Get(), poison);
                ID3D11ShaderResourceView* in[4] = {sv[0].Get(), sv[1].Get(), sv[2].Get(), sv[3].Get()};
                ID3D11UnorderedAccessView* uav = outU.Get();
                ID3D11Buffer* cbp = cb.Get();
                ctx->CSSetShader(cs, nullptr, 0);
                ctx->CSSetShaderResources(0, 4, in);
                ctx->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
                ctx->CSSetConstantBuffers(0, 1, &cbp);
                ctx->Dispatch(static_cast<UINT>((c.w + gx - 1) / gx), static_cast<UINT>((c.h + gy - 1) / gy), 1);
                ctx->ClearState();
                return readBytes(d, out.Get(), 1);
            };
            const std::vector<uint8_t> want = runOne(old.Get(), 8, 8);
            const std::vector<uint8_t> got = runOne(prod.Get(), 16, 16);
            char what[200];
            std::snprintf(what, sizeof(what), "near-light %dx%d, flags %u: four blocks a group is one block a group, byte for byte (%zu blocks)", c.w, c.h, c.flags, want.size());
            check(got == want, what);
            blocks += want.size();
            for (uint8_t b : want) lit += b == 255;
            for (size_t i = 0; i < std::size(muts); ++i)
                if (runOne(mutCs[i].Get(), 16, 16) != want) ++caught[i];
        }
        check(lit > 0 && lit * 2 < blocks, "the near-light fixtures hold lit blocks and dark ones (the equality is not vacuous)");
        for (size_t i = 0; i < std::size(muts); ++i) {
            char what[200];
            std::snprintf(what, sizeof(what), "CONTROL: a near-light shader that %s is caught (%zu of %zu cases differ)", muts[i].name, caught[i], std::size(cases));
            check(caught[i] >= 1, what);
        }
        std::printf("near-light four blocks a group against one: %zu cases, %zu blocks, %zu lit\n", std::size(cases), blocks, lit);
    }
}

void runGroupBench(Device& d) {
    if (d.warp) std::puts("note: --bench-groups on WARP times a CPU; use --adapter nvidia.");
    benchGroupsComposite(d);
    benchGroupsRcas(d);
    benchGroupsUiResolve(d);
    benchGroupsNearLight(d);
}

}  // namespace

int main(int argc, char** argv) {
    bool selfTest = false, dry = false, print = false, nvidia = false, stats = false, bench = false, benchComposite = false, benchGroups = false;
    int fuzz = 240;
    const char* usage =
        "usage: ui_holo_pass_test --self-test|--dry-run|--print-goldens|--stats|--bench|--bench-composite|--bench-groups [--fuzz N] [--adapter nvidia]";
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--self-test")) selfTest = true;
        else if (!std::strcmp(argv[i], "--dry-run")) dry = true;
        else if (!std::strcmp(argv[i], "--print-goldens")) print = true;
        else if (!std::strcmp(argv[i], "--stats")) stats = true;
        else if (!std::strcmp(argv[i], "--bench")) bench = true;
        else if (!std::strcmp(argv[i], "--bench-composite")) benchComposite = true;
        else if (!std::strcmp(argv[i], "--bench-groups")) benchGroups = true;
        else if (!std::strcmp(argv[i], "--fuzz") && i + 1 < argc) fuzz = std::max(0, std::atoi(argv[++i]));
        else if (!std::strcmp(argv[i], "--adapter") && i + 1 < argc && !std::strcmp(argv[i + 1], "nvidia")) { nvidia = true; ++i; }
        else { std::puts(usage); return 2; }
    }
    if (dry) {
        std::puts("Would compare the production UI resolve against its frozen reference on WARP, byte for byte, over the fixtures and fuzz cases; writes no files.");
        return 0;
    }
    if (!selfTest && !print && !stats && !bench && !benchComposite && !benchGroups) { std::puts(usage); return 2; }

    Device d = makeDevice(nvidia, !benchComposite && !benchGroups);   // the bench times the GPU; the debug layer would time the CPU
    ID3D11Device* dev = d.dev.Get();
    if (benchGroups) {
        runGroupBench(d);
        if (g_failures) { std::printf("FAILED: %d of %d checks\n", g_failures, g_checks); return 1; }
        return 0;
    }
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
        testCompositeInPlace(d);
        testGroupShapes(d);
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

