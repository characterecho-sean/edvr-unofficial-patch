#pragma once
// THE DEFERRED LIGHTING PASS OVER AN EMPTY G-BUFFER (src/d3d11/flat_runtime_model.h: FlatRuntimeReached, flatRuntimeEmptyGBuffer,
// FlatDeferredDonorWhy::EmptyGBuffer). The Detailed Surface Scanner draws no geometry into the G-buffer: its colours and depth are cleared, and what
// is drawn into them before the lighting pass is a pair that never reads b1 (the inert filters, the sun-shadow resolve, the star's depth-only
// pre-pass). The pass found no source record and no witness entry, so every jittered frame failed deferred-inverse-unproven. Nothing a camera placed
// is in its inputs, so it is neither verified nor failed (edvr_gfx_20261022_112019, census 1).
//
// These cases judge the rule: the census's own sequence admits, every path by which something reaches the inputs defeats it, the scenes that have
// a donor keep theirs, and the wiring in the verifier and the hooks says what the rule needs it to.
#include "flat_deferred_witness_tests.h"
#include "flat_hdr_route_tests.h"
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

namespace deferred_empty_gbuffer_test {
using namespace edvr;
using deferred_witness_test::Scene;
using deferred_witness_test::countOf;
using deferred_witness_test::between;
using deferred_witness_test::slurp;
using laser_scene_tone_test::token;
using laser_scene_tone_test::draw;
using laser_scene_tone_test::camera;

constexpr uint64_t kInertAVs = 0xFC1193AFFC596F74ull, kInertBVs = 0xE8FDC0D92EEBA6D7ull, kInertPs = 0x258B95AC99520C1Full;
constexpr uint64_t kShadowVs = 0x53211E8C072CD02Eull, kShadowResolvePs = 0x7EAC71963E66C5FEull, kShadowInertPs = 0xB403F48CB35D9739ull;
constexpr uint64_t kFullscreenVs = 0xDEF19B035D5EDEDCull, kDepthCopyPs = 0xCB95394B50D737D6ull, kAlphaPs = 0x06CD42A941C4C7C9ull;
constexpr uint64_t kForwardVs = 0xD8D1067D5723C1D4ull, kForwardPs = 0x06332CA168B6DA63ull;   // the first forward writer after the lighting pass
constexpr uint64_t kMaskingVs = 0x5DA53D8Bull;                                               // the earlier scanner's depth-only masking quads

// One Detailed Surface Scanner frame, in the census's order. Tokens: the Scene's depth, dsv, SRV0 (gbuf) and HDR, and SRV1, SRV2, the shadow
// mask and the depth copy beside them.
struct Dss {
    Scene s;
    const void *g1 = token(0x1100), *g2 = token(0x1200), *shadow = token(0x1300), *depthCopy = token(0x1400);
    const void* g0() const { return s.gbuf; }
    void clear(const void* resource) { flatRuntimeWritten(*s.p, resource, true); }
    // A pass the way the runtime builds it: the target, with the scene's depth or none, no b1 camera unless asked.
    FlatRuntimeDraw pass(const void* colour, uint32_t format, bool withDepth, uint64_t vs, uint64_t ps, bool withCamera = false) {
        auto d = draw(colour, format, withDepth ? s.depth : nullptr, withDepth ? s.dsv : nullptr, vs, ps);
        d.key.kind = kFlatContractScreen;
        if (withCamera) camera(d, deferred_witness_test::kFrame, s.p->sequence + 1);
        return d;
    }
    void see(const FlatRuntimeDraw& d) { s.see(d); }
    // depth cleared, the inert stencil draw, the colours and HDR cleared, the star's pre-pass twice, the depth copy, the second inert draw, the
    // alpha pass of SRV2 with no depth, the sun-shadow resolve in its two passes (the census, draws 110 to 117). Clears are left out by name.
    void frame(const void* skip = nullptr, const void* skip2 = nullptr) {
        const auto cl = [&](const void* r) { if (r != skip && r != skip2) clear(r); };
        cl(s.depth);
        see(pass(g0(), 23, true, kInertAVs, kInertPs));
        cl(g0()); cl(g1); cl(g2); cl(s.hdr);
        see(s.star()); see(s.star());
        see(pass(depthCopy, 39, false, kFullscreenVs, kDepthCopyPs, true));
        see(pass(g0(), 23, true, kInertBVs, kInertPs));
        see(pass(g2, 27, false, kFullscreenVs, kAlphaPs, true));
        see(pass(shadow, 60, true, kShadowVs, kShadowResolvePs, true));
        see(pass(shadow, 60, true, kShadowVs, kShadowInertPs));
    }
    FlatRuntimeDraw lighting() const { return s.lighting(); }
    FlatDeferredDonor donor(const FlatRuntimeDraw& h, const void* g0Override = nullptr) const {
        const void* gb[3] = {g0Override ? g0Override : s.gbuf, g1, g2};
        return flatRuntimeDeferredDonor(*s.p, h.key, s.p->sequence + 1, gb);
    }
    FlatEmptyGBuffer empty(const FlatRuntimeDraw& h) const {
        const void* gb[3] = {s.gbuf, g1, g2};
        return flatRuntimeEmptyGBuffer(*s.p, h.key, gb);
    }
};

inline unsigned run() {
    unsigned failures = 0, checks = 0;
    const auto expect = [&](bool ok, const char* name) { ++checks; if (!ok) { ++failures; std::printf("FAIL: deferred empty gbuffer %s\n", name); } };
    using Why = FlatDeferredDonorWhy;
    using Role = FlatDeferredDonorRole;

    // ---- the census's own sequence admits ---------------------------------------------------------------------------------------------
    {
        Dss d; d.frame();
        const auto h = d.lighting();
        const auto donor = d.donor(h);
        expect(!donor && donor.role == Role::None && donor.why == Why::EmptyGBuffer, "the Detailed Surface Scanner's sequence: no donor, why empty-gbuffer");
        expect(donor.poolRecords == 0 && donor.witnessEntries == 0 && !donor.entry && !donor.overflow, "(with no source record, no witness entry, no overflow)");
        expect(std::string(flatDeferredDonorWhyName(donor.why)) == "empty-gbuffer" && std::string(flatDeferredDonorWhyName(Why::GBufferReached)) == "gbuffer-reached",
               "the outcome names are what the flight reads in the witness line");
        expect(d.empty(h) == FlatEmptyGBuffer::Empty && !d.s.p->uncertain, "the model's answer is empty, and nothing marked the prefix uncertain");
        expect(std::string(flatDeferredDonorWhyName(Why::NoEntry)) == "no-witness-entry" && std::string(flatDeferredDonorWhyName(Why::None)) == "accepted",
               "the older names are what they were");
        // What the lighting pass is, on the model's side: an unverified write into the cleared HDR, carrying no camera. It is neither a donor
        // nor a conflict, and the later forward writers give the HDR its camera as they do in a scene with no lighting pass.
        d.see(h);
        const auto* t = flatRuntimeTarget(*d.s.p, d.s.hdr);
        expect(t && t->writes.draws == 1 && !t->hdrCamera && !t->hdrBad && !t->tone.deferredCamera && !d.s.p->uncertain,
               "the lighting pass is one camera-less write into the HDR: no camera, no conflict, no uncertainty");
        auto forward = d.pass(d.s.hdr, 26, true, kForwardVs, kForwardPs, true);
        d.see(forward);
        t = flatRuntimeTarget(*d.s.p, d.s.hdr);
        expect(t && t->hdrCamera && !t->hdrBad && !t->tone.deferredCamera && t->tone.key.vs == kForwardVs && t->writes.draws == 2,
               "the first forward writer after it sets the HDR's camera");
        expect(flat_mono_detail::cameraCurrent(t->tone, deferred_witness_test::kFrame), "(a camera that is current)");
        for (int i = 0; i < 6; ++i) d.see(d.pass(d.s.hdr, 26, true, kForwardVs, kForwardPs, true));
        const auto facts = flatSceneFacts(*d.s.p, 1920, 1200);
        expect(facts.onShape && facts.draws == 8 && facts.resource == d.s.hdr, "and the scene picker counts its eight draws without the lit-scene allowance");
    }
    {
        // The same scene, fed as a recorded trace is: the unverified lighting pass and the rest replay to the same state.
        Dss a, b; a.frame(); b.frame();
        const auto ha = a.lighting(), hb = b.lighting();
        a.see(ha);
        auto event = flatTraceEventFromDraw(hb, false);
        b.see(flatTraceEventToDraw(event));
        const auto* ta = flatRuntimeTarget(*a.s.p, a.s.hdr);
        const auto* tb = flatRuntimeTarget(*b.s.p, b.s.hdr);
        expect(ta && tb && ta->writes.draws == tb->writes.draws && ta->hdrCamera == tb->hdrCamera && a.s.p->uncertain == b.s.p->uncertain,
               "the lighting pass through the trace is what it is direct");
    }

    // ---- the selector takes the frame: an unverified lighting pass, the forward writers' camera, a tone pass ------------------------------
    {
        using namespace hdr_route_test;
        Stream st;
        const void *depth = st.sc.hDepth, *h = st.sc.h, *g0 = tok(0x7000), *g1 = tok(0x7100), *g2 = tok(0x7200), *shadow = tok(0x7300);
        const uint32_t W = st.sc.hW, H = st.sc.hH;
        const auto clr = [&](const void* r) { flatRuntimeWritten(*st.prefix, r, true); };
        clr(depth);
        st.draw(st.make(g0, depth, W, H, 23, kInertAVs, kInertPs, false, false));
        clr(g0); clr(g1); clr(g2); clr(h);
        st.draw(st.make(g0, depth, W, H, 23, 0x0EE43D81E394E70Cull, 0, false, false));
        st.draw(st.make(g2, nullptr, W, H, 27, kFullscreenVs, kAlphaPs, false, false));
        st.draw(st.make(shadow, depth, W, H, 60, kShadowVs, kShadowResolvePs, false, false));
        auto lit = st.make(h, depth, W, H, 26, kFlatDeferredHdrVs, kFlatDeferredHdrPs, false, false);
        lit.key.srvView[0] = tok(0x7001); lit.key.srvResource[0] = g0;
        const void* gb[3] = {g0, g1, g2};
        const auto donor = flatRuntimeDeferredDonor(*st.prefix, lit.key, st.prefix->sequence + 1, gb);
        expect(donor.why == Why::EmptyGBuffer && !donor, "(the stream's own lighting pass is judged empty)");
        st.draw(lit);
        st.sceneDraws(0, 8);
        st.toneTrigger();
        const FlatMonoFrame sel = st.select();
        expect(sel.selected() && sel.sourceFree && sel.hdr == h && sel.depth == depth && !st.prefix->uncertain,
               "the HDR route selects the frame source-free, through the forward writers' camera");
        const auto* t = flatRuntimeTarget(*st.prefix, h);
        expect(t && t->hdrCamera && !t->tone.deferredCamera && t->writes.draws == 9, "(its camera is a forward writer's, nine draws in the HDR)");
        expect(flatSceneFacts(*st.prefix, st.sc.outW, st.sc.outH).draws == 9, "and the scene picker counts it without the lit-scene allowance");
    }

    // ---- every path by which something reaches the inputs defeats it ---------------------------------------------------------------------
    {
        const auto defeated = [&](const char* name, FlatEmptyGBuffer want, void (*mutate)(Dss&)) {
            Dss d; d.frame(); mutate(d);
            const auto h = d.lighting();
            const auto donor = d.donor(h);
            const bool ok = d.empty(h) == want && donor.why != Why::EmptyGBuffer;
            if (!ok) std::printf("deferred empty gbuffer case `%s`: answer %u why=%s\n", name, unsigned(d.empty(h)), flatDeferredDonorWhyName(donor.why));
            expect(ok, name);
        };
        // a camera writer, of the pairs the model keeps: a pool source, a witnessed Screen-kind writer
        defeated("a pool source on the depth", FlatEmptyGBuffer::Unknown, [](Dss& d) { d.see(d.s.pool(d.g0())); });
        defeated("a pool source in another colour of the depth", FlatEmptyGBuffer::Unknown, [](Dss& d) { d.see(d.s.pool(d.g1)); });
        defeated("one witnessed camera writer", FlatEmptyGBuffer::Unknown, [](Dss& d) { d.see(d.s.writer()); });
        defeated("one witnessed camera writer into another colour of the depth", FlatEmptyGBuffer::Unknown, [](Dss& d) { d.see(d.s.writer(d.g1)); });
        defeated("a witnessed writer with no camera of a pair that is not exempt", FlatEmptyGBuffer::Unknown, [](Dss& d) {
            d.see(d.pass(d.g0(), 23, true, 0x1234, 0x5678)); });
        defeated("the star's vertex shader with a pixel shader: the exemption is the exact pair", FlatEmptyGBuffer::Unknown, [](Dss& d) {
            d.see(d.pass(d.g0(), 23, true, 0x0EE43D81E394E70Cull, 0x9)); });
        // draws the witness does not see
        defeated("a colour-writing draw of an unknown pair with no depth into SRV0", FlatEmptyGBuffer::Reached, [](Dss& d) {
            d.see(d.pass(d.g0(), 23, false, 0x1234, 0x5678)); });
        defeated("a draw of an unknown pair into a target of no screen format on the depth", FlatEmptyGBuffer::Reached, [](Dss& d) {
            auto x = draw(d.g1, 10, d.s.depth, d.s.dsv, 0x1234, 0x5678); d.see(x); });
        defeated("a draw into the depth with no colour target", FlatEmptyGBuffer::Reached, [](Dss& d) {
            auto x = draw(nullptr, 0, d.s.depth, d.s.dsv, 0x1234, 0x5678); x.key.rtv = nullptr; d.see(x); });
        defeated("a draw with a depth and no dsv into SRV0", FlatEmptyGBuffer::Reached, [](Dss& d) {
            auto x = draw(d.g0(), 23, d.s.depth, nullptr, 0x1234, 0x5678); x.key.kind = kFlatContractScreen; d.see(x); });
        defeated("a supported draw of a target the pool kind does not take", FlatEmptyGBuffer::Reached, [](Dss& d) {
            auto x = draw(d.g1, 10, d.s.depth, d.s.dsv, 0x1234, 0x5678); x.supported = true; d.see(x); });
        // writes that are not draws
        defeated("a copy into SRV0", FlatEmptyGBuffer::Reached, [](Dss& d) { flatRuntimeWritten(*d.s.p, d.g0()); });
        defeated("a copy into SRV1", FlatEmptyGBuffer::Reached, [](Dss& d) { flatRuntimeWritten(*d.s.p, d.g1); });
        defeated("a copy into SRV2", FlatEmptyGBuffer::Reached, [](Dss& d) { flatRuntimeWritten(*d.s.p, d.g2); });
        defeated("a copy into the depth", FlatEmptyGBuffer::Reached, [](Dss& d) { flatRuntimeWritten(*d.s.p, d.s.depth); });
        defeated("a compute write to SRV0", FlatEmptyGBuffer::Reached, [](Dss& d) { flatRuntimeDispatchObserveWritten(*d.s.p, d.g0()); });
        defeated("a compute write to SRV1", FlatEmptyGBuffer::Reached, [](Dss& d) { flatRuntimeDispatchObserveWritten(*d.s.p, d.g1); });
        defeated("a compute write to SRV2", FlatEmptyGBuffer::Reached, [](Dss& d) { flatRuntimeDispatchObserveWritten(*d.s.p, d.g2); });
        defeated("a compute write to the depth", FlatEmptyGBuffer::Reached, [](Dss& d) { flatRuntimeDispatchObserveWritten(*d.s.p, d.s.depth); });
        // the HDR
        defeated("a copy into the HDR", FlatEmptyGBuffer::Unknown, [](Dss& d) { flatRuntimeWritten(*d.s.p, d.s.hdr); });
        defeated("a compute write to the HDR", FlatEmptyGBuffer::Unknown, [](Dss& d) { flatRuntimeDispatchObserveWritten(*d.s.p, d.s.hdr); });
        defeated("a prior draw into the HDR", FlatEmptyGBuffer::Unknown, [](Dss& d) { d.see(d.pass(d.s.hdr, 26, false, kFullscreenVs, kAlphaPs)); });
        defeated("a prior draw into the HDR of a pair that never reads b1", FlatEmptyGBuffer::Unknown, [](Dss& d) {
            d.see(d.pass(d.s.hdr, 26, true, kInertAVs, kInertPs)); });
        defeated("a bad HDR", FlatEmptyGBuffer::Unknown, [](Dss& d) {
            auto* t = flatRuntimeTarget(*d.s.p, d.s.hdr); t->hdrBad = true; });
        defeated("an HDR whose first write was an image copy", FlatEmptyGBuffer::Unknown, [](Dss& d) {
            auto* t = flatRuntimeTarget(*d.s.p, d.s.hdr); t->imageCopyFirst = true; });
        defeated("an HDR that already has a camera", FlatEmptyGBuffer::Unknown, [](Dss& d) {
            auto* t = flatRuntimeTarget(*d.s.p, d.s.hdr); t->hdrCamera = true; });
        // the prefix
        defeated("an uncertain prefix", FlatEmptyGBuffer::Unknown, [](Dss& d) { flatRuntimeMarkUncertain(*d.s.p, FlatRuntimeUncertainCause::ForeignWork); });
        defeated("a witness table that overflowed", FlatEmptyGBuffer::Unknown, [](Dss& d) { d.s.p->witnessOverflow = true; });
    }

    // ---- an input nothing cleared is unknown, not empty ------------------------------------------------------------------------------------
    {
        const char* names[] = {"depth", "SRV0", "SRV1", "SRV2", "HDR"};
        for (unsigned which = 0; which < 5; ++which) {
            Dss d;
            const void* skip = which == 0 ? d.s.depth : which == 1 ? d.g0() : which == 2 ? d.g1 : which == 3 ? d.g2 : d.s.hdr;
            d.frame(skip);
            const auto h = d.lighting();
            const auto donor = d.donor(h);
            const bool ok = d.empty(h) == FlatEmptyGBuffer::Unknown && donor.why == Why::NoEntry && !donor;
            if (!ok) std::printf("deferred empty gbuffer uncleared %s: answer %u why=%s\n", names[which], unsigned(d.empty(h)), flatDeferredDonorWhyName(donor.why));
            expect(ok, "an input the game did not clear this frame is unknown (the old no-witness-entry)");
        }
        Dss none;   // no clear at all: the older cases' scenes, unchanged
        none.see(none.pass(none.g0(), 23, true, kInertAVs, kInertPs));
        expect(none.donor(none.lighting()).why == Why::NoEntry && none.empty(none.lighting()) == FlatEmptyGBuffer::Unknown,
               "an inert pair alone with no clear is still no-witness-entry");
    }

    // ---- a clear erases what came before it, and counts what comes after -------------------------------------------------------------------
    {
        Dss a;   // content, then the clear: empty
        a.clear(a.s.depth); a.clear(a.g0()); a.clear(a.g1); a.clear(a.g2); a.clear(a.s.hdr);
        flatRuntimeWritten(*a.s.p, a.g0());
        a.see(a.pass(a.g0(), 23, false, 0x1234, 0x5678));
        flatRuntimeDispatchObserveWritten(*a.s.p, a.g1);
        flatRuntimeWritten(*a.s.p, a.s.depth);
        expect(a.empty(a.lighting()) == FlatEmptyGBuffer::Reached, "(four inputs written since their clear)");
        a.clear(a.s.depth); a.clear(a.g0()); a.clear(a.g1);
        expect(a.empty(a.lighting()) == FlatEmptyGBuffer::Empty, "a second clear erases the writes since the first");
        Dss b;   // a write before the first clear: there is no entry to count it on, and the clear would have erased it
        flatRuntimeWritten(*b.s.p, b.g0());
        flatRuntimeDispatchObserveWritten(*b.s.p, b.s.depth);
        b.see(b.pass(b.g0(), 23, false, 0x1234, 0x5678));
        b.frame();
        expect(b.empty(b.lighting()) == FlatEmptyGBuffer::Empty && b.s.p->reachedUsed >= 5, "a write before the clear is no content after it");
        Dss c;   // the clear that is the first write to a never-written resource makes its entry with nothing on it
        c.clear(c.g0());
        const auto* e = flatRuntimeReachedFind(*c.s.p, c.g0());
        expect(e && e->draws == 0 && e->writes == 0 && c.s.p->reachedUsed == 1, "a clear makes an entry with no draws and no writes");
        flatRuntimeWritten(*c.s.p, token(0xABCDE0));
        flatRuntimeDispatchObserveWritten(*c.s.p, token(0xABCDE1));
        expect(c.s.p->reachedUsed == 1 && !c.s.p->reachedOverflow, "a write to a resource with no entry makes none (the buffers the game maps never fill the table)");
    }

    // ---- SRV1 and SRV2 are not charged for a draw of their own; the depth is -------------------------------------------------------------
    {
        Dss d; d.frame();
        expect(flatRuntimeReachedFind(*d.s.p, d.g2)->draws == 1 && d.empty(d.lighting()) == FlatEmptyGBuffer::Empty,
               "the census's alpha pass into SRV2 (mask 8, no depth) is counted against SRV2 and does not make the pass non-empty");
        expect(flatRuntimeReachedFind(*d.s.p, d.s.depth)->draws == 0 && flatRuntimeReachedFind(*d.s.p, d.g0())->draws == 0,
               "while the exempt pairs are not draws against the depth or SRV0");
        d.see(d.pass(d.g1, 27, false, 0x1234, 0x5678));
        expect(d.empty(d.lighting()) == FlatEmptyGBuffer::Empty, "a draw with no depth into SRV1 changes nothing, as the alpha pass into SRV2");
        d.see(d.pass(d.g1, 27, true, 0x1234, 0x5678));
        expect(d.empty(d.lighting()) == FlatEmptyGBuffer::Unknown, "a draw with the depth is a writer on it, and is not empty");
    }

    // ---- the inputs the caller names are checked ---------------------------------------------------------------------------------------------
    {
        Dss d; d.frame();
        const auto h = d.lighting();
        const void* ok[3] = {d.g0(), d.g1, d.g2};
        expect(flatRuntimeEmptyGBuffer(*d.s.p, h.key, ok) == FlatEmptyGBuffer::Empty, "(the right three)");
        expect(flatRuntimeEmptyGBuffer(*d.s.p, h.key, nullptr) == FlatEmptyGBuffer::Unknown, "no inputs named: unknown");
        const void* noSrv1[3] = {d.g0(), nullptr, d.g2}; const void* noSrv2[3] = {d.g0(), d.g1, nullptr};
        expect(flatRuntimeEmptyGBuffer(*d.s.p, h.key, noSrv1) == FlatEmptyGBuffer::Unknown &&
               flatRuntimeEmptyGBuffer(*d.s.p, h.key, noSrv2) == FlatEmptyGBuffer::Unknown, "SRV1 or SRV2 unresolved: unknown");
        const void* other[3] = {d.g1, d.g2, d.g0()};   // all three cleared, but SRV0 is not the resource the key names
        const void* unseen[3] = {token(0x9990), d.g1, d.g2};
        expect(flatRuntimeEmptyGBuffer(*d.s.p, h.key, other) == FlatEmptyGBuffer::Unknown &&
               flatRuntimeEmptyGBuffer(*d.s.p, h.key, unseen) == FlatEmptyGBuffer::Unknown,
               "SRV0 that is not the resource the pass's key names, or that nothing cleared: unknown");
        const void* self[3] = {d.g0(), d.s.hdr, d.g2}; const void* depthIn[3] = {d.g0(), d.g1, d.s.depth};
        expect(flatRuntimeEmptyGBuffer(*d.s.p, h.key, self) == FlatEmptyGBuffer::Unknown &&
               flatRuntimeEmptyGBuffer(*d.s.p, h.key, depthIn) == FlatEmptyGBuffer::Unknown, "an input that is the pass's own HDR or its depth: unknown");
        auto noDepth = h; noDepth.key.depth = nullptr;
        expect(flatRuntimeEmptyGBuffer(*d.s.p, noDepth.key, ok) == FlatEmptyGBuffer::Unknown, "no depth: unknown");
        // the table's capacity: sixty-four resources cleared in a frame, a sixty-fifth is an overflow and the answer is unknown
        Dss full; full.frame();
        for (uintptr_t i = full.s.p->reachedUsed; i < kFlatRuntimeReached; ++i) full.clear(token(0xC000 + i * 0x10));
        expect(full.s.p->reachedUsed == kFlatRuntimeReached && !full.s.p->reachedOverflow && full.empty(full.lighting()) == FlatEmptyGBuffer::Empty,
               "a full table is not an overflow");
        full.clear(token(0xD000));
        expect(full.s.p->reachedOverflow && full.empty(full.lighting()) == FlatEmptyGBuffer::Unknown, "a clear the table cannot hold fails closed");
        full.clear(full.g0());   // clearing a resource that has an entry never needs a new one
        Dss many; many.frame();
        for (int i = 0; i < 500; ++i) { many.clear(many.g0()); many.see(many.pass(many.g0(), 23, true, kInertAVs, kInertPs)); }
        expect(!many.s.p->reachedOverflow && many.empty(many.lighting()) == FlatEmptyGBuffer::Empty, "five hundred clears of one resource are one entry");
    }

    // ---- the scenes that have a donor keep theirs ------------------------------------------------------------------------------------------
    {
        Dss quads;   // the earlier scanner: depth-only masking quads with a camera, a witness entry, accepted
        quads.clear(quads.s.depth); quads.clear(quads.g0()); quads.clear(quads.g1); quads.clear(quads.g2); quads.clear(quads.s.hdr);
        for (int i = 0; i < 80; ++i) quads.see(quads.s.writer(nullptr, kMaskingVs));
        const auto donor = quads.donor(quads.lighting());
        expect(donor.role == Role::Witness && donor.why == Why::None && donor.witnessEntries == 1 && donor.writers == 80,
               "eighty masking quads with a camera still take the witness path, clears and all");
        Dss pool;   // a scene with a pool source: the pool rule decides, the ledger is not asked
        pool.frame();
        pool.see(pool.s.pool(pool.g0()));
        const auto p = pool.donor(pool.lighting());
        expect(p.role == Role::Pool && p.why == Why::None && p.poolRecords == 1, "a pool source in SRV0 is the pool donor, with every input cleared");
        Dss sourceElsewhere;   // a source on the depth that is not SRV0's: the pool rule says no, and nothing says empty
        sourceElsewhere.frame();
        sourceElsewhere.see(sourceElsewhere.s.pool(sourceElsewhere.g1));
        expect(sourceElsewhere.donor(sourceElsewhere.lighting()).why == Why::PoolRule, "a source in another colour: the pool rule's refusal, never empty");
        Dss accepted;   // the witness entry, then a clear of its colour restarts it empty: the restart is an entry, so the answer is not empty
        accepted.frame();
        accepted.see(accepted.s.writer());
        accepted.clear(accepted.g0());
        expect(accepted.donor(accepted.lighting()).why == Why::NoEntry && accepted.donor(accepted.lighting()).witnessEntries == 1,
               "an entry a clear restarted still stands on the depth: its writers drew there, so the answer is not empty");
    }

    // ---- the wiring: the verifier, the hooks, the model --------------------------------------------------------------------------------------
    {
        const std::string runtime = slurp("src/d3d11/flat_runtime.cpp"), model = slurp("src/d3d11/flat_runtime_model.h"),
                          vscreen = slurp("src/d3d11/vscreen.cpp"), header = slurp("src/d3d11/flat_runtime.h");
        expect(!runtime.empty() && !model.empty() && !vscreen.empty() && !header.empty(), "the sources are readable from the repo root");
        const std::string verifier = between(runtime, "const FlatProjectionBindingPlan* verifyDeferredHdrCamera(", "void reportHdrProjectionCapture(");
        expect(countOf(verifier, "flatRuntimeDeferredDonor(s.prefix,k,s.prefix.sequence+1,gbuffer)") == 1 &&
               countOf(verifier, "const void* const gbuffer[3]={source.Get(),source1.Get(),source2.Get()};") == 1 &&
               countOf(verifier, "PSGetShaderResources(1,1,&srv1);s.context->PSGetShaderResources(2,1,&srv2);") == 1,
               "the verifier names SRV0, SRV1 and SRV2 to the one resolver, from the context");
        expect(countOf(verifier, "k.srvResource[1]") == 0 && countOf(verifier, "k.srvView[1]") == 0,
               "and does not put them in the key, whose views the frame contract hashes");
        expect(countOf(verifier, "const bool emptyGBuffer=donor.why==FlatDeferredDonorWhy::EmptyGBuffer && actual && input && !foreignWork.load(std::memory_order_acquire);") == 1,
               "the pass is empty only where the shape, the views and the formats were verified, and no other thread worked");
        expect(countOf(verifier, "if(flatCameraInjectUpstreamOwns() && nonzero && !executionCoherent && !repairing && !emptyGBuffer) {") == 1 &&
               countOf(verifier, "reportFailure(\"deferred-inverse-unproven\");") == 1 && countOf(verifier, "failPhase(s,\"deferred-inverse-unproven\");") == 1,
               "an empty pass is not the failure that fails the phase and marks the prefix uncertain");
        expect(countOf(verifier, "} else if(emptyGBuffer)++s.deferredEmptyGBuffer;\n    else ++s.deferredCameraRefused;") == 1 &&
               countOf(verifier, "empty-gbuffer-total=%llu work=%s") == 1 && countOf(verifier, "(unsigned long long)s.deferredEmptyGBuffer,flatWorkName(s.work));") == 1,
               "the outcome is counted apart from a refusal, and the HDR camera line carries the total");
        expect(countOf(verifier, "draw.deferredHdrVerified=firstWitness && executionCoherent;") == 1,
               "(and the pass is still not verified: the repairs and the donor-based qualification are untouched)");
        const std::string repairs = between(verifier, "const bool repairable=", "const bool executionCoherent=");
        expect(countOf(repairs, "emptyGBuffer") == 0 && countOf(repairs, "qualified &&") >= 1,
               "no repair is built for an empty pass: both are gated on a qualified pass, which needs a donor");
        expect(verifier.find("if(s.work==FlatWork::Probe && nonzero)return nullptr;") != std::string::npos &&
               verifier.find("if(flatCameraInjectUpstreamOwns() && !flatDeferredVerifyInPhase(nonzero,phaseDomain))return nullptr;") != std::string::npos,
               "the early returns, the Probe work path's and the phase domain's, are where they were");
        expect(runtime.find("flatRuntimeDeferredDonor(s.prefix,k,s.prefix.sequence+1,nullptr,d.camera)") != std::string::npos && countOf(runtime, "flatRuntimeDeferredDonor(") == 3,
               "the binding-failure path asks the resolver without the inputs: it never reports an empty pass");
        // the hooks that say a write is a clear
        expect(countOf(vscreen, "flatRuntimeWritten(static_cast<ID3D11Resource*>(info.resource), FlatOverlayMutationOp::Written, true);") == 1 &&
               countOf(vscreen, "flatRuntimeWritten(static_cast<ID3D11Resource*>(info.resource),FlatOverlayMutationOp::ClearRtv);") == 1 &&
               countOf(vscreen, "flatRuntimeWritten(dst);") == 1,
               "the depth clear says it is a constant fill (its provenance stays Written for the history), the colour clear says ClearRtv, a resolve says nothing");
        expect(header.find("bool constantFill=false);") != std::string::npos &&
               countOf(runtime, "clear || constantFill);") == 1 && countOf(runtime, "flatRuntimeWritten(s.prefix, res, constantFill);") == 1,
               "the flag reaches the model from the clears and from nothing else");
        // the model: the draw count sits before the colourless return, the exemption is the shared one
        expect(model.find("flatRuntimeReachedDraw(p, k);   // a draw reaches its depth and its colour, and a depth-only draw its depth\n"
                          "    if (d.poolFamilyVs) ++p.unsupportedFamilyDraws;\n    if (!k.color) return out;") != std::string::npos,
               "the model counts a draw before the colourless return, so a depth-only draw reaches the depth");
        const std::string drawCount = between(model, "inline void flatRuntimeReachedDraw(", "// Whether the deferred lighting pass reads inputs");
        expect(drawCount.find("flatProjectionB1Unread(k.vs, k.ps)") != std::string::npos, "and the pairs that never read b1 are the exemption the witness already makes");
        expect(countOf(model, "flatRuntimeReachedWrite(p, resource, constantFill);") == 1 && countOf(model, "flatRuntimeReachedWrite(p, resource, false);") == 1,
               "the model counts an explicit write and a compute write against the ledger");
        const std::string resolver = between(model, "inline FlatDeferredDonor flatRuntimeDeferredDonor(", "inline bool flatRuntimeQualifiedDeferredCamera(");
        expect(countOf(resolver, "flatRuntimeEmptyGBuffer(p,k,gbuffer)") == 1 && countOf(resolver, "if(!out.witnessEntries) {") == 1 &&
               countOf(resolver, "out.role=FlatDeferredDonorRole::") == 3,
               "the resolver asks for the answer only where no entry stands on the depth, and still makes a donor in exactly three places (the pool rule, the agreeing witness, the named camera of a two-camera entry)");
    }
    std::printf("flat deferred empty g-buffer: %u checks, %u failure(s)\n", checks, failures);
    return failures;
}
} // namespace deferred_empty_gbuffer_test
