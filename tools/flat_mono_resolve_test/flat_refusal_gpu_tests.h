// The VR world route's refusal census and view (design doc section 82, stage 2 experiment build) on WARP: the prep's class byte, the counting
// pass and its read-back, what the steady-detail rule (steadyDetail, the depth-validated form) does to the counts, the HDR finish's paint, and that a frame which
// asks for neither makes and touches none of it.
//
// One fixture throughout (flat_resolve_fixture.h: 16 x 16, a still camera, no engine slot anywhere unless a case writes one, one pool
// record, the game's pipeline to hand back) with an R11G11B10F scene target H, so every number below is a pixel count the layout of the
// slots decides, and the end-to-end invariant is the strongest one: the census's refused total equals the number of texels the backend
// was handed a rejection mask of 255 for, frame by frame, because the class byte and the mask come from the same decision.
#pragma once
#include "../../src/d3d11/flat_foreground_motion_shader.h"
#include "../../src/d3d11/flat_mono_refusal.h"
#include "../../src/d3d11/flat_mono_shader_source.h"
#include <cstdlib>
#include <cstring>
#include <string>

// flat_mono_resolve.cpp, at its foot: true while any of the census's or the view's resources (class texture, counting pass, counter
// buffer) exists. Test-only, not part of flat_mono_resolve.h's contract.
namespace edvr { bool flatMonoResolveTestRefusalResources(); }

namespace refusalgpu {
// The number the shader writes for a class: `name=<digits>` in the HLSL's `static const uint` line. -1 when it is not there.
inline int hlslConstant(const std::string& source, const char* name) {
    const std::string needle = std::string(name) + "=";
    const size_t at = source.find(needle);
    if (at == std::string::npos) return -1;
    return std::atoi(source.c_str() + at + needle.size());
}
struct Taken {
    uint64_t asked = 0, sampled = 0, dropped = 0, frames = 0, pixels = 0, checked = 0, skipped = 0, skinned = 0;
    uint64_t counts[edvr::kFlatMonoRefusalSlots] = {};
    uint64_t weaponReasons[edvr::kFlatMonoWeaponReasons] = {};
    void add(const edvr::FlatMonoRefusalCensus& c) {
        asked += c.asked; sampled += c.sampled; dropped += c.dropped; frames += c.frames; pixels += c.pixels;
        checked += c.checked; skipped += c.skipped; skinned += c.skinned;
        for (uint32_t i = 0; i < edvr::kFlatMonoRefusalSlots; ++i) counts[i] += c.counts[i];
        for (uint32_t i = 0; i < edvr::kFlatMonoWeaponReasons; ++i) weaponReasons[i] += c.weaponReasons[i];
    }
    uint64_t reasoned() const { uint64_t n = 0; for (uint32_t i = 0; i < edvr::kFlatMonoWeaponReasons; ++i) n += weaponReasons[i]; return n; }
    uint64_t refused() const { uint64_t n = 0; for (uint32_t i = 0; i < edvr::kFlatMonoRefusalStaleKept; ++i) n += counts[i]; return n; }
};
}  // namespace refusalgpu

inline void refusalGpuTests(ID3D11Device* device, ID3D11DeviceContext* context) {
    using edvr::FlatMonoResolveFrame;
    using edvr::FlatMonoResolveMode;
    using namespace edvr;
    const UINT w = ResolveFixture::w, h = ResolveFixture::h;

    // ---- 0. the HLSL's class numbers are the header's ---------------------------------------------------------------------------------
    {
        const std::string source = kFlatMonoShaderSource;
        struct Pair { const char* name; uint32_t value; };
        const Pair pairs[] = {
            {"kClassNone", kFlatMonoClassNone}, {"kClassJoined", kFlatMonoClassJoined}, {"kClassMasked", kFlatMonoClassMasked},
            {"kClassNotRig", kFlatMonoClassNotRig}, {"kClassStale", kFlatMonoClassStale}, {"kClassCorrupt", kFlatMonoClassCorrupt},
            {"kClassStaleStamp", kFlatMonoClassStaleStamp}, {"kClassSentinel", kFlatMonoClassSentinel},
            {"kClassUnreprojectable", kFlatMonoClassUnreprojectable}, {"kClassCamera", kFlatMonoClassCamera},
            {"kClassRange", kFlatMonoClassRange}, {"kClassDepth", kFlatMonoClassDepth}, {"kClassWeapon", kFlatMonoClassWeapon},
            {"kClassWeaponRefused", kFlatMonoClassWeaponRefused}, {"kClassReset", kFlatMonoClassReset},
            {"kClassSkinned", kFlatMonoClassSkinned}};
        bool same = true;
        for (const Pair& p : pairs) same = same && refusalgpu::hlslConstant(source, p.name) == static_cast<int>(p.value);
        check(same, "refusal: the HLSL's kClass* numbers are flat_mono_refusal.h's, all sixteen");
        // The byte is the class in bits 0-3, a refused first-person pixel's reason in bits 4-6 and the refused flag in bit 7. Every reader masks
        // what it reads: a reader that still took the low seven bits as the class would read a reason as a class number.
        const auto occurrences = [&](const char* needle) {
            size_t n = 0, at = 0;
            const std::string s(needle);
            while ((at = source.find(s, at)) != std::string::npos) { ++n; at += s.size(); }
            return n;
        };
        check(source.find("(reject!=0?0x80u:0u)") != std::string::npos && source.find("&0x7Fu") == std::string::npos &&
                  occurrences("const uint kind=v&0x0Fu;") == 2 && occurrences("best=max(best,v&0x8Fu)") == 1 &&
                  kFlatMonoClassRefusedBit == 0x80u && kFlatMonoClassMask == 0x0Fu && kFlatMonoClassReasonMask == 0x70u &&
                  kFlatMonoClassReasonShift == 4 && kFlatMonoClassSkinned <= kFlatMonoClassMask &&
                  (kFlatMonoClassMask | kFlatMonoClassReasonMask | kFlatMonoClassRefusedBit) == 0xFFu &&
                  (kFlatMonoClassMask & kFlatMonoClassReasonMask) == 0 && (kFlatMonoClassReasonMask >> kFlatMonoClassReasonShift) + 1 == kFlatMonoWeaponReasons,
              "refusal: the class byte is the class (bits 0-3, every class fits), the reason (bits 4-6) and the refused bit (bit 7), and the shader's readers mask it so");
        // The reason numbers the foreground map's vertex shader writes are the header's, all eight.
        const std::string vs = kFlatForegroundMotionVs;
        const Pair reasons[] = {
            {"kReasonNone", kFlatMonoWeaponReasonNone}, {"kReasonInvalidCurrent", kFlatMonoWeaponReasonInvalidCurrent},
            {"kReasonNotAuthentic", kFlatMonoWeaponReasonNotAuthentic}, {"kReasonNoPrior", kFlatMonoWeaponReasonNoPrior},
            {"kReasonPriorPositions", kFlatMonoWeaponReasonPriorPositions}, {"kReasonIdentityDiffers", kFlatMonoWeaponReasonIdentityDiffers},
            {"kReasonPriorIdentityInvalid", kFlatMonoWeaponReasonPriorIdentityInvalid}, {"kReasonAmbiguous", kFlatMonoWeaponReasonAmbiguous}};
        bool sameReasons = true;
        for (const Pair& r : reasons) sameReasons = sameReasons && refusalgpu::hlslConstant(vs, r.name) == static_cast<int>(r.value);
        check(sameReasons, "refusal: the foreground map's kReason* numbers are flat_mono_refusal.h's, all eight");
        bool named = true;
        for (uint32_t r = 0; r < kFlatMonoWeaponReasons; ++r) named = named && std::strcmp(flatMonoWeaponReasonName(r), "?") != 0;
        check(named && std::strcmp(flatMonoWeaponReasonName(kFlatMonoWeaponReasons), "?") == 0 && kFlatMonoRefusalCounters == 25 && kFlatMonoRefusalSlots == 16,
              "refusal: every reason has a name, the counters of a stripe are the sixteen class slots, the eight reasons and the skinned slot, and the class slots stay sixteen");
    }

    // ---- the fixture ----------------------------------------------------------------------------------------------------------------
    flatMonoResolveReset();
    ResolveFixture fx(device, context);
    std::vector<float> z(w * h, .01f), slots(w * h * 2);
    auto noSlots = [&] { for (size_t i = 0; i < slots.size(); i += 2) { slots[i] = -1; slots[i + 1] = .01f; } };
    auto put = [&](UINT x, UINT y, float code, float slotDepth) { slots[(size_t(y) * w + x) * 2] = code; slots[(size_t(y) * w + x) * 2 + 1] = slotDepth; };
    noSlots();
    auto depth = texture(device, w, h, DXGI_FORMAT_R32_FLOAT, D3D11_BIND_SHADER_RESOURCE, z.data(), w * 4);
    auto depthView = view(device, depth.Get());
    auto hTexture = texture(device, w, h, DXGI_FORMAT_R11G11B10_FLOAT, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET);
    auto hSrv = view(device, hTexture.Get());
    // What the game rendered: a uniform (1, 1, 1), re-uploaded before every frame (the resolver writes its result back into H).
    std::vector<uint32_t> input(w * h, hdrgpu::pack(1.0, 1.0, 1.0));
    auto upload = [&] {
        context->UpdateSubresource(hTexture.Get(), 0, nullptr, input.data(), w * 4, 0);
        context->UpdateSubresource(fx.slotTexture.Get(), 0, nullptr, slots.data(), w * 8, 0);
        context->UpdateSubresource(depth.Get(), 0, nullptr, z.data(), w * 4, 0);
    };
    // The record every slot code 1 points at, as the main rig builds its joined pixel: a rigid pose moving -.3125 in x, whose marker word is
    // joined (a rig record EDVR followed), masked (one it could not), or absent (not a rig record). kStamp 77 is the fixture's EN[276].x.
    uint32_t record[84]{};
    auto setRecord = [&](uint32_t markerKind /* 0 none, 1 joined, 2 masked */) {
        record[1] = record[77] = bits(1); record[2] = record[78] = 0x7fff7fff; record[3] = record[79] = 0xfffe7fff;
        record[4] = bits(0); record[5] = bits(0); record[6] = bits(2.5f);
        record[73] = bits(-.3125f); record[74] = bits(0); record[75] = bits(2.5f);
        edvr::engine_velocity_emit::Pose np{{record[4], record[5], record[6], record[2], record[3]}};
        edvr::engine_velocity_emit::Pose pp{{record[73], record[74], record[75], record[78], record[79]}};
        record[72] = markerKind == 0 ? 0u : (markerKind == 1 ? 0x7FC0ED01u : 0x7FC0ED02u) ^ edvr::engine_velocity_emit::markerHash(np, pp, 77);
        context->UpdateSubresource(fx.pool.Get(), 0, nullptr, record, 0, 0);
    };
    setRecord(0);
    FlatMonoResolveFrame f{};
    f.color = hSrv.Get(); f.depth = depthView.Get(); f.renderWidth = w; f.renderHeight = h; f.outputWidth = w; f.outputHeight = h;
    f.deltaMs = 16; camera(f.camera); camera(f.previousCamera); fx.engine(f);
    f.mode = FlatMonoResolveMode::Dlss; f.frame = 5000; f.hdr = true; f.reset = true;
    expectedJx = expectedJy = 0;
    auto run = [&](const char* what) {
        upload();
        fx.bindOriginal();
        ComPtr<ID3D11ShaderResourceView> out; const char* why = nullptr;
        const bool ok = flatMonoResolve(device, context, f, out.GetAddressOf(), &why);
        if (!ok) std::printf("info: refusal scenario \"%s\": resolver reason %s\n", what, why ? why : "none");
        check(ok && fx.restored(), what);
        ++f.frame;
        return ok;
    };
    auto popcountMask = [&] { unsigned n = 0; for (unsigned char b : observedMaskAll) n += b != 0; return n; };
    auto takeAll = [&](uint64_t wantFrames) {
        refusalgpu::Taken t;
        for (int i = 0; i < 400 && t.frames < wantFrames; ++i) {
            context->Flush();
            t.add(flatMonoResolveTakeRefusalCensus());
            if (t.frames < wantFrames) Sleep(2);
        }
        return t;
    };

    // ---- 1. a frame that asks for neither makes and touches nothing ---------------------------------------------------------------------
    check(!flatMonoResolveTestRefusalResources(), "refusal: before any frame asks, the census and the view own no resource");
    run("refusal: the reset frame that starts the run");
    f.reset = false;
    put(5, 5, 1, .02f);   // a stale texel: the prep refuses it in every case below that does not forgive it
    run("refusal: a continuing frame that asks for nothing");
    check(!flatMonoResolveTestRefusalResources(),
          "refusal: a frame with the census and the view off makes no class texture, no counting pass and no counter buffer");
    {
        const auto c = flatMonoResolveTakeRefusalCensus();
        check(c.asked == 0 && c.sampled == 0 && c.frames == 0 && c.pixels == 0 && c.refused() == 0 && c.dropped == 0,
              "refusal: nothing asked, nothing sampled: every counter is zero");
    }

    // ---- 2. the census counts what the prep refuses, by cause ----------------------------------------------------------------------------
    // The layout: a 4 x 4 stale block, one corrupt (even) code, the out-of-range sentinel, a sky texel with a slot, and one masked record.
    noSlots();
    for (UINT y = 4; y < 8; ++y) for (UINT x = 4; x < 8; ++x) put(x, y, 1, .02f);   // stale: the slot's depth is not the pixel's
    put(2, 2, 2, .01f);                                                              // corrupt: an even slot code
    put(13, 3, 4294967296.0f, .02f);                                                 // sentinel: the out-of-range marker
    z[12 * w + 12] = 0.0f; put(12, 12, 1, .02f);                                     // sentinel: a slot under a pixel with no depth (sky)
    put(8, 8, 1, .01f);                                                              // masked: its record's marker says so
    setRecord(2);
    f.refusalCensus = true;
    f.staticScene = false;
    for (unsigned i = 0; i < 4; ++i) run("refusal: census step A (key off): four asking frames, one of which is sampled");
    const uint64_t hashA = observedMotionHash;
    const unsigned maskPopA = popcountMask();
    const auto a = takeAll(1);
    check(flatMonoResolveTestRefusalResources(), "refusal: a frame that asks makes the class texture, the counting pass and the counter buffer");
    check(a.asked == 4 && a.sampled == 1 && a.frames == 1 && a.dropped == 0 && a.pixels == uint64_t(w) * h,
          "refusal: four asking frames take one sample (one in kFlatMonoRefusalEvery), read it back, and count its pixels");
    check(a.counts[kFlatMonoClassStale] == 16 && a.counts[kFlatMonoClassMasked] == 1 && a.counts[kFlatMonoClassCorrupt] == 1 &&
              a.counts[kFlatMonoClassSentinel] == 2 && a.counts[kFlatMonoRefusalStaleKept] == 0,
          "refusal: the stale block is 16 pixels, the masked record 1, the corrupt code 1, the sentinel and the sky 2; none kept");
    check(a.refused() == 20 && a.refused() == maskPopA,
          "refusal: the census's refused total is exactly the number of texels the backend was handed a rejection of 255 for");
    check(a.refused() == a.counts[kFlatMonoClassStale] + a.counts[kFlatMonoClassMasked] + a.counts[kFlatMonoClassCorrupt] +
                             a.counts[kFlatMonoClassSentinel],
          "refusal: no other class counts a pixel in this layout (a still camera refuses nothing at the image's edge)");
    std::printf("flat mono resolve: refusal census (key off): %llu of %llu pixels refused: stale %llu, masked %llu, corrupt %llu, sentinel %llu\n",
                static_cast<unsigned long long>(a.refused()), static_cast<unsigned long long>(a.pixels),
                static_cast<unsigned long long>(a.counts[kFlatMonoClassStale]), static_cast<unsigned long long>(a.counts[kFlatMonoClassMasked]),
                static_cast<unsigned long long>(a.counts[kFlatMonoClassCorrupt]), static_cast<unsigned long long>(a.counts[kFlatMonoClassSentinel]));

    // Steady detail on (FlatMonoResolveFrame::steadyDetail, which both routes always set; this rig drives it both ways): the scene is still (last frame's depth is this frame's), so
    // the stale block takes the camera term where last frame's depth confirms it, is no longer refused and is counted as kept; the masked
    // record, the corrupt code and the sentinel stay refused. The depth-check cases themselves are flat_steady_depth_gpu_tests.h's.
    f.steadyDetail = true;
    for (unsigned i = 0; i < 4; ++i) run("refusal: census step B (steady detail on): the stale block takes the camera term");
    const unsigned maskPopB = popcountMask();
    const auto b = takeAll(1);
    check(b.counts[kFlatMonoClassStale] == 0 && b.counts[kFlatMonoRefusalStaleKept] == 16 && b.counts[kFlatMonoClassMasked] == 1 &&
              b.counts[kFlatMonoClassCorrupt] == 1 && b.counts[kFlatMonoClassSentinel] == 2,
          "refusal: with the steady-detail rule on, in a still scene, the 16 stale pixels move from stale-refused to stale-kept and the masked record, corrupt code and sentinel stay refused");
    check(b.checked == 4 && b.skipped == 0 && a.checked == 0 && a.skipped == 0,
          "refusal: the depth check's frames are counted with the key on (four) and never with it off");
    check(b.refused() == 4 && b.refused() == maskPopB && maskPopA - maskPopB == 16,
          "refusal: the refused total falls by exactly the stale block, and still equals the rejection mask's count");
    check(observedMotionHash != hashA, "refusal: the key changes what the backend is handed (the stale block's motion and mask), so the counts are not a fiction");
    f.steadyDetail = false;

    // Counts accumulate across samples: eight asking frames are two samples.
    for (unsigned i = 0; i < 8; ++i) run("refusal: census step C: eight asking frames, two samples");
    {
        const auto c = takeAll(2);
        check(c.asked == 8 && c.sampled == 2 && c.frames == 2 && c.pixels == 2ull * w * h && c.counts[kFlatMonoClassStale] == 32 &&
                  c.counts[kFlatMonoClassMasked] == 2 && c.counts[kFlatMonoClassCorrupt] == 2 && c.counts[kFlatMonoClassSentinel] == 4,
              "refusal: eight asking frames take two samples and the counts add (2 x the layout)");
    }
    // A reset frame never asks (every pixel is refused there, which says nothing).
    {
        f.reset = true; run("refusal: a reset frame with the census on"); f.reset = false;
        const auto c = flatMonoResolveTakeRefusalCensus();
        check(c.asked == 0 && c.sampled == 0, "refusal: a reset frame is not an asking frame and takes no sample");
    }
    // The census off again: nothing more is asked or sampled.
    f.refusalCensus = false;
    for (unsigned i = 0; i < 4; ++i) run("refusal: census off again");
    {
        const auto c = flatMonoResolveTakeRefusalCensus();
        check(c.asked == 0 && c.sampled == 0 && c.frames == 0, "refusal: with the census off again no frame asks, however many pass");
    }

    // ---- 3. the view paints the classes, in the eye path's colours ------------------------------------------------------------------------
    // Input (1,1,1) everywhere; the stub backend's result (History) is (0,1,0). An accepted pixel shows History, a refused one the raw input;
    // the paint scales its colour by twice the displayed pixel's luma (floor .04), so: refused -> 2 x (1,1,1); accepted -> 1.4304 x (0,1,0)
    // for classes with a colour, and a quarter of (0,1,0) for a pixel with no engine slot.
    auto readH = [&](std::vector<uint32_t>& out) {
        std::vector<unsigned char> bytes;
        if (!readWhole(context, hTexture.Get(), bytes, 4)) return false;
        out.resize(w * h);
        std::memcpy(out.data(), bytes.data(), bytes.size());
        return true;
    };
    auto close = [](double got, double want) { return std::abs(got - want) <= 0.03 * std::abs(want) + 0.02; };
    auto isColour = [&](const std::vector<uint32_t>& px, UINT x, UINT y, double r, double g, double bl) {
        double c[3]; hdrgpu::unpack(px[size_t(y) * w + x], c);
        return close(c[0], r) && close(c[1], g) && close(c[2], bl);
    };
    const double y1 = 2.0 * 0.7152;   // twice the luma of the stub backend's (0,1,0)
    noSlots();
    z.assign(w * h, .01f);
    for (UINT yy = 4; yy < 8; ++yy) for (UINT xx = 4; xx < 8; ++xx) put(xx, yy, 1, .02f);   // stale (refused)
    put(2, 2, 2, .01f);                                                                      // corrupt (refused)
    put(13, 3, 4294967296.0f, .02f);                                                         // sentinel (refused)
    put(10, 2, 1, .01f);                                                                     // a pool surface that is not a rig record (accepted)
    setRecord(0);
    f.staticScene = false;
    // The control: the view off. The same frame, painted nothing: a refused pixel shows its raw input.
    f.refusalView = 0;
    run("refusal: the view off, the control frame");
    const uint64_t viewOffHash = observedMotionHash;
    std::vector<uint32_t> px;
    check(readH(px) && isColour(px, 5, 5, 1.0, 1.0, 1.0) && isColour(px, 0, 0, 0.0, 1.0, 0.0) && isColour(px, 10, 2, 0.0, 1.0, 0.0),
          "refusal: with the view off a refused pixel shows its raw input (1,1,1) and an accepted one the backend's result (0,1,0): nothing is painted");
    f.refusalView = 1;
    run("refusal: the view on");
    check(observedMotionHash == viewOffHash, "refusal: the view changes nothing the backend is handed (depth, motion and rejection are bit-identical)");
    check(readH(px), "refusal: the painted H is readable");
    // A stale slot the prep REFUSED (class 4 with bit 7 set: the pixel shows the raw frame) is pink, y x (1, .4, .7); one the steady-detail rule
    // KEPT (class 4, bit 7 clear) stays yellow, y x (1, 1, 0), and is asserted below. The refused block shows the raw (1,1,1), so y = 2.
    check(isColour(px, 5, 5, 2.0, 0.8, 1.4) && isColour(px, 6, 6, 2.0, 0.8, 1.4),
          "refusal view: a REFUSED stale slot is pink (twice the pixel's own level in red, .4 of that in green, .7 of that in blue), not yellow");
    check(isColour(px, 2, 2, 2.0, 0.0, 2.0), "refusal view: a corrupt slot is magenta");
    check(isColour(px, 13, 3, 2.0, 2.0, 2.0), "refusal view: the out-of-range sentinel is white (any other refusal)");
    check(isColour(px, 10, 2, 0.0, 0.3 * y1, y1), "refusal view: a pool surface that is not a rig record (the camera term) is blue");
    check(isColour(px, 0, 0, 0.0, 0.25, 0.0) && isColour(px, 15, 15, 0.0, 0.25, 0.0),
          "refusal view: a pixel with no engine slot is dimmed to a quarter of what it showed");
    // The record kinds: masked is red, joined is green.
    put(8, 8, 1, .01f); setRecord(2);
    run("refusal: the view on, a masked record");
    check(readH(px) && isColour(px, 8, 8, 2.0, 0.0, 0.0), "refusal view: a masked record is red (refused, so it shows the raw input scaled by its own level)");
    setRecord(1);
    run("refusal: the view on, a joined record");
    check(readH(px) && isColour(px, 8, 8, 0.0, y1, 0.0), "refusal view: a joined record is green (accepted: the backend's result scaled by its own level)");
    // With the steady-detail rule on, a stale pixel the depth check KEEPS is accepted and stays yellow (class 4 with bit 7 clear: the view says
    // what the pixel IS, the census how it was treated); only a refused one is pink (above). Same texel, same class, the other bit.
    f.steadyDetail = true;
    run("refusal: the view on, steady detail on");
    check(readH(px) && isColour(px, 5, 5, y1, y1, 0.0),
          "refusal view: a stale pixel the steady-detail rule KEPT is yellow, painted from the backend's result (not from the raw input, and not pink)");
    f.steadyDetail = false;
    f.refusalView = 0;
    run("refusal: the view off again");
    check(readH(px) && isColour(px, 5, 5, 1.0, 1.0, 1.0), "refusal: the view off again paints nothing");
    flatMonoResolveReset();
}
