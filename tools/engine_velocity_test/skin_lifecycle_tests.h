#pragma once
// skin_lifecycle_tests: the engine's draw half (the linked engine_velocity.cpp) with the second skin live, end to end on WARP. A skinned character is
// drawn in both eyes through the REAL path -- the game's palette chain is told about (a job table at t0, a palette at u0, the chain dispatch hook's
// call), the pool is written and torn the way the game's Map tees report it, the patched vertex and pixel shaders are made and bound, target 7 is
// created, cleared, bound and read back through the view the compose gets. Nothing here is a model of the engine: the engine runs. Cases (every
// check carries a label "L<case>.<what>"; tools\skin_engine_test\mutants.py names the case that must catch each mutation):
//   L1  arming: configure arms the second skin (the hook asked once, its gate opened), and before any frame nothing is bound
//   L2  the first frame has no history: the drawn pixels carry valid 0 and E 0, never a stale answer; target 7 is zero outside the drawn pixels; the entry
//       fade's signal (engineMotionReady) says skinned jobs with the join not live
//   L3  a steady second frame (the prefix join, the hook stood down): E is EXACTLY zero and valid 1 at every drawn pixel, in both eyes; the signal says the
//       join is live
//   L4  the character moves: E = 100 x (previous - current position) in centimetres
//   L5  the job table changes shape: no history for that frame, then the next frame has it again
//   L6  what the draw binds and what is put back: target 7 and the three views while the patched pair draws, the blend state's mask for target 7
//       (all four channels for the pair that exports E, none for a skinned family's other pixel shader and for a rigid family), and nothing left
//       bound at the frame boundary; a game that binds its own target 7 keeps it
//   L7  the hook's list as the identity (the stubbed hook hands the join the entry list): the same frames join by the hook, a list that disagrees
//       with the job table falls back to the prefix and says so
//   L8  the periodic lines: the join's counters read back from the GPU, the second-skin line, the hook's line; the two halves of the join line describe the
//       same frames; the entry fade's signal (armed only while the compose asks for the views, a run needs both eyes)
//   L9  a previous palette buffer too small to hold a job's previous rows: no history for those jobs, with no list at all
//   L10 which record of the character's base is the live one: the pool also holds a stale second record (above or below the live one, read by no draw),
//       both records read, a draw list that cannot be exact; the instance stream at an IA offset; the pose witness line
//   L11 a skinned family's pixel shader that exports no E draws over an exporting one's pixels: it writes no history there, not the E beneath
//   L12 the palette chain dispatched twice in one present frame (the game does, in the settlement: F13): the join is ONE per present frame over the dispatches'
//       job tables in dispatch order, whichever job table the drawn character is in, one buffer rewritten between the dispatches, a one-two-one sequence, a
//       second dispatch after a skinned draw has needed the join (late), one on another palette buffer, a frame whose chain no skinned draw needed, the hook's
//       one list per frame judged against the union, and the counters that say so
//   L13 the on-foot source (F2 on foot, the first-person panel): the source pass's skinned draws write the source's own target 7, handed to the screen shader and
//       the world route (EngineVelocityViews::skin) only while a skinned draw wrote it this frame, cleared at each frame's first skinned draw, E exact, no history
//       as valid 0 and E 0, never in the eyes' views, and never in the flat profile (no target 7 is created there)
//   L14 the GPU census slots the second skin fills (F16): the source's and the eyes' target 7 clears, the join, the join-clear probe and the pose table are each begun
//       by the work they price and by nothing else, and the probe leaves the join's tables as the join wrote them

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "engine_motion_ready.h"
#include "lifecycle_tests.h"
#include "skin_clone_tests.h"
#include "../skin_clone_test/synthetic_skin.h"
#include "../skin_join_test/skin_join_world.h"

namespace skin_lifecycle_tests {
using Microsoft::WRL::ComPtr;
namespace lt = lifecycle_tests;
namespace sct = skin_clone_tests;
namespace sjw = skin_join_world;

constexpr uint64_t kPairVs = 0xD99AFDC250D19A3Full, kPairPs = 0xE86271E464CCDC1Dull;    // a skinned family and its E-exporting pixel shader
constexpr uint64_t kPlainVs = 0x61AE8EB05FDC18DDull, kPlainPs = 0xFC43E42710010343ull;  // another skinned family, a pixel shader that exports no E

inline float halfToFloat(uint16_t v) {
    const uint32_t sign = (v >> 15) & 1u, exp = (v >> 10) & 31u, mant = v & 1023u;
    float out;
    if (exp == 0) out = std::ldexp(float(mant), -24);
    else if (exp == 31) out = mant ? std::numeric_limits<float>::quiet_NaN() : std::numeric_limits<float>::infinity();
    else out = std::ldexp(float(mant + 1024u), int(exp) - 25);
    return sign ? -out : out;
}

// What the draw bound, read from the context between the engine's call and the game's draw.
struct Seen {
    bool rt7 = false;               // target 7 is bound
    bool rt7IsOurs = false;
    UINT blend7 = 99, blend6 = 99;  // the derived blend state's write masks for targets 7 and 6
    bool vsPatched = false;
    bool srv[3] = {};               // t108 t109 t110 bound
    bool derived = false;
};

struct Fixture {
    const lt::Harness& h;
    lt::Game& g;
    ID3D11Device* dev;
    ID3D11DeviceContext* ctx;
    ComPtr<ID3D11VertexShader> vsPair, vsPlain;
    ComPtr<ID3D11PixelShader> psPair, psPlain;
    ComPtr<ID3D11InputLayout> layout;
    ComPtr<ID3D11Buffer> verts, instances, jobs, jobs2, palette[2];
    ComPtr<ID3D11ShaderResourceView> jobsSrv, jobs2Srv, paletteSrv[2];
    ComPtr<ID3D11UnorderedAccessView> paletteUav[2];
    sct::Vertex tri[3];
    sct::State state;
    uint32_t slot = 5, bones = 3, base = 1;
    unsigned count = 0;                  // frames drawn
    std::vector<float> rows[2];
    sjw::Built built;                    // the world the job table and the hook's list come from
    uint64_t hookSeq = 0;
    bool useHookList = false;
    uint32_t paletteRows = 64;
    Seen seen;
    // The pose table's live record (L10): the pool can hold a stale second record of the character's base with another pose (the F12 flight's shape),
    // at a slot above the live one (11) or below it (2). The instance stream is {junk, live 5, 11, 2} read from an IA offset of 8 (entry 0 = the live
    // record, entry 1 = slot 11, entry 2 = slot 2); the draws read entry 0 and, when a test says so, the stale record's entry too.
    int staleSlot = -1;                  // the slot the stale record is written at (-1: none)
    uint32_t staleEntry = 1;             // the stream entry that names it
    float staleShift = 0.12f;            // how far its pose is from the live one (m)
    bool readStale = false;              // a second draw per eye reads the stale record (both records are read)
    bool secondStream = false;           // a second vertex buffer of stride 8 is bound: the draws' stream is ambiguous
    bool overdrawPlain = false;          // after each eye's pair draw, a skinned family's pixel shader that exports no E draws over the same pixels (L11)
    UINT drawEntry = 0;                  // the stream entry the draws read (3 names slot 3, a record no base owns: nothing of the character is read)
    // The palette chain in two dispatches (L12): the jobs of built.jobs listed in `secondDispatch` go in the frame's second dispatch, the rest in the first.
    std::vector<size_t> secondDispatch;
    bool sameJobsBuffer = false;         // the second dispatch rewrites the first's job table buffer (the join must take each table at its dispatch)
    bool lateSecond = false;             // the second dispatch comes after the frame's first pass: a skinned draw has already needed the join
    bool secondOnOtherPalette = false;   // the second dispatch writes the other palette buffer
    unsigned chainK = 0;                 // the palette buffer of the frame's chain
    std::vector<sjw::JobRow> pendingSecond;
    ComPtr<ID3D11DepthStencilState> alwaysDepth;

    Fixture(const lt::Harness& harness, lt::Game& game) : h(harness), g(game), dev(harness.device), ctx(harness.context) {}

    ComPtr<ID3D11Buffer> structured(const void* data, UINT stride, UINT count_, UINT bind) {
        D3D11_BUFFER_DESC d{};
        d.ByteWidth = stride * count_; d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = bind;
        d.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED; d.StructureByteStride = stride;
        D3D11_SUBRESOURCE_DATA init{data, 0, 0};
        ComPtr<ID3D11Buffer> b;
        h.check(SUCCEEDED(dev->CreateBuffer(&d, data ? &init : nullptr, &b)), "L: a structured buffer");
        return b;
    }

    bool setup() {
        const lt::Harness& hh = h;
        const shader_tests::Harness sh{dev, ctx, hh.check};
        const auto vsCode = shader_tests::compile(sh, skin_clone_synthetic::vertexSource(false), "vs_5_0");
        const auto psCode = shader_tests::compile(sh, skin_clone_synthetic::pixelSource(), "ps_5_0");
        if (!vsCode || !psCode) return false;
        h.check(SUCCEEDED(dev->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(), nullptr, &vsPair)) &&
                SUCCEEDED(dev->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(), nullptr, &vsPlain)) &&
                SUCCEEDED(dev->CreatePixelShader(psCode->GetBufferPointer(), psCode->GetBufferSize(), nullptr, &psPair)) &&
                SUCCEEDED(dev->CreatePixelShader(psCode->GetBufferPointer(), psCode->GetBufferSize(), nullptr, &psPlain)), "L: the skinned shaders create");
        // device_hook's creation tees
        edvr::engineVelocityRememberVs(vsPair.Get(), kPairVs, vsCode->GetBufferPointer(), vsCode->GetBufferSize(), false);
        edvr::engineVelocityRememberVs(vsPlain.Get(), kPlainVs, vsCode->GetBufferPointer(), vsCode->GetBufferSize(), false);
        edvr::engineVelocityRememberPs(psPair.Get(), kPairPs, psCode->GetBufferPointer(), psCode->GetBufferSize(), false);
        edvr::engineVelocityRememberPs(psPlain.Get(), kPlainPs, psCode->GetBufferPointer(), psCode->GetBufferSize(), false);
        const D3D11_INPUT_ELEMENT_DESC elements[] = {
            {"INSTANCEANDMODELDATAINDEX", 0, DXGI_FORMAT_R32G32_UINT, 1, 0, D3D11_INPUT_PER_INSTANCE_DATA, 1},
            {"PACKEDVERTEXDATAA", 0, DXGI_FORMAT_R32G32B32A32_UINT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"PACKEDVERTEXDATAB", 0, DXGI_FORMAT_R32G32B32A32_UINT, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"PACKEDVERTEXDATAC", 0, DXGI_FORMAT_R32G32B32A32_UINT, 0, 32, D3D11_INPUT_PER_VERTEX_DATA, 0},
        };
        h.check(SUCCEEDED(dev->CreateInputLayout(elements, 4, vsCode->GetBufferPointer(), vsCode->GetBufferSize(), &layout)), "L: the skinned input layout");
        sct::Rng rng(31);
        sct::makeVertices(rng, bones, tri);
        state = sct::makeState(rng, bones, base);
        D3D11_BUFFER_DESC vd{};
        vd.ByteWidth = sizeof(tri); vd.Usage = D3D11_USAGE_DEFAULT; vd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        D3D11_SUBRESOURCE_DATA vinit{tri, 0, 0};
        h.check(SUCCEEDED(dev->CreateBuffer(&vd, &vinit, &verts)), "L: the vertices");
        const uint32_t instance[10] = {0xFFFFu, 0xFFFFu, slot, 0, 11, 0, 2, 0, 3, 0};   // one junk entry, then the live record's, the two stale slots' and an empty slot's; bound at an offset of 8
        vd.ByteWidth = sizeof(instance);
        D3D11_SUBRESOURCE_DATA iinit{instance, 0, 0};
        h.check(SUCCEEDED(dev->CreateBuffer(&vd, &iinit, &instances)), "L: the instance");
        {
            D3D11_DEPTH_STENCIL_DESC dsd{};
            dsd.DepthEnable = TRUE; dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL; dsd.DepthFunc = D3D11_COMPARISON_ALWAYS;
            h.check(SUCCEEDED(dev->CreateDepthStencilState(&dsd, &alwaysDepth)), "L: a depth state that always passes");
        }
        jobs = structured(nullptr, 16, 8, D3D11_BIND_SHADER_RESOURCE);
        h.check(SUCCEEDED(dev->CreateShaderResourceView(jobs.Get(), nullptr, &jobsSrv)), "L: the job table's view");
        jobs2 = structured(nullptr, 16, 8, D3D11_BIND_SHADER_RESOURCE);
        h.check(SUCCEEDED(dev->CreateShaderResourceView(jobs2.Get(), nullptr, &jobs2Srv)), "L: the second job table's view");
        for (int i = 0; i < 2; ++i) {
            palette[i] = structured(nullptr, 48, paletteRows, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS);
            h.check(SUCCEEDED(dev->CreateShaderResourceView(palette[i].Get(), nullptr, &paletteSrv[i])) &&
                    SUCCEEDED(dev->CreateUnorderedAccessView(palette[i].Get(), nullptr, &paletteUav[i])), "L: a palette buffer's views");
        }
        for (int eye = 0; eye < 2; ++eye) rows[eye].assign(lt::kSceneFloats, 0.0f);
        setWorld(bones);
        return true;
    }

    // The world the chain and the hook describe: one character (one entity, one job of `n` bones).
    void setWorld(uint32_t n) {
        bones = n;
        sjw::World w;
        w.push_back(sjw::Ent{900, 0xA11CE, 0, {{77, n}}});
        built = sjw::build(w, 1);
        base = built.jobs.front().dst;
    }

    // The same, plus an extra character after it (one entity, one job of 20 bones): the world of a frame whose chain has a second dispatch.
    void setWorldTwo(uint32_t n) {
        bones = n;
        sjw::World w;
        w.push_back(sjw::Ent{900, 0xA11CE, 0, {{77, n}}});
        w.push_back(sjw::Ent{901, 0xA11CE, 0, {{78, 20}}});
        built = sjw::build(w, 1);
        base = built.jobs.front().dst;
    }

    void writeSceneRows() {
        for (int eye = 0; eye < 2; ++eye) {
            auto& r = rows[eye];
            std::fill(r.begin(), r.end(), 0.0f);
            r[270 * 4 + 0] = 0.4f;
            r[271 * 4 + 1] = 0.4f;
            r[273 * 4 + 2] = 0.5f;
            r[273 * 4 + 3] = 1.0f;
            r[273 * 4 + 0] = eye ? 0.01f : 0.0f;   // the eyes' own offsets
            std::memcpy(&r[275 * 4], state.pos, 12);
        }
    }

    // The game's palette chain for this frame: the palette written into one of its two buffers, the job table at t0, the chain's dispatch.
    void chain(unsigned frameIndex) {
        const unsigned k = frameIndex & 1u;
        std::vector<float> cur(size_t(paletteRows) * 12, 0.0f);
        for (size_t i = 0; i < state.rows.size() && size_t(base) * 12 + i < cur.size(); ++i) cur[size_t(base) * 12 + i] = state.rows[i];   // (rows past the buffer are the game's to lose)
        ctx->UpdateSubresource(palette[k].Get(), 0, nullptr, cur.data(), 0, 0);
        chainK = k;
        std::vector<sjw::JobRow> first, second;
        for (size_t i = 0; i < built.jobs.size(); ++i) {
            const bool inSecond = std::find(secondDispatch.begin(), secondDispatch.end(), i) != secondDispatch.end();
            (inSecond ? second : first).push_back(built.jobs[i]);
        }
        dispatchPart(first, false);
        pendingSecond = second;
        if (!second.empty() && !lateSecond) { dispatchPart(second, true); pendingSecond.clear(); }
    }
    // The second dispatch of a frame whose second dispatch comes late (after the first pass), if there is one.
    void chainLate() {
        if (pendingSecond.empty()) return;
        dispatchPart(pendingSecond, true);
        pendingSecond.clear();
    }
    // One dispatch of the chain: a job table (the game's own buffer for each dispatch, or the first's rewritten) at t0, the palette at u0, the engine told.
    void dispatchPart(const std::vector<sjw::JobRow>& part, bool isSecond) {
        ID3D11Buffer* buf = (isSecond && !sameJobsBuffer) ? jobs2.Get() : jobs.Get();
        ID3D11ShaderResourceView* t0 = (isSecond && !sameJobsBuffer) ? jobs2Srv.Get() : jobsSrv.Get();
        std::vector<sjw::JobRow> table = part;
        table.resize(8);
        ctx->UpdateSubresource(buf, 0, nullptr, table.data(), 0, 0);
        ctx->CSSetShaderResources(0, 1, &t0);
        ID3D11UnorderedAccessView* u0 = paletteUav[(isSecond && secondOnOtherPalette) ? (chainK ^ 1u) : chainK].Get();
        ctx->CSSetUnorderedAccessViews(0, 1, &u0, nullptr);
        edvr::engineVelocityNoteChainDispatch(ctx, uint32_t(part.size()));
        ID3D11ShaderResourceView* none = nullptr;
        ctx->CSSetShaderResources(0, 1, &none);
        ID3D11UnorderedAccessView* noUav = nullptr;
        ctx->CSSetUnorderedAccessViews(0, 1, &noUav, nullptr);
    }

    // The triangle at `scale` of its size (the pixels it covered before are no longer drawn: target 7 must be zero there again).
    void setTriangleScale(float scale) {
        static const int px[3] = {-700, 700, 0}, py[3] = {-700, -700, 700};
        for (int k = 0; k < 3; ++k) {
            const uint32_t x = uint32_t((px[k] * scale + 1000) * 65535 / 2000), y = uint32_t((py[k] * scale + 1000) * 65535 / 2000);
            tri[k].a[0] = x | (y << 16);
        }
        ctx->UpdateSubresource(verts.Get(), 0, nullptr, tri, 0, 0);
    }

    void writePool() {
        uint8_t rec[336];
        sct::writeRecord(state, 99u, rec);
        std::memcpy(g.pool[slot].words, rec, 336);
        // the stale second record of the same base: another pose, written at the stale slot; the other stale slot holds nothing
        for (const uint32_t s : {11u, 2u}) std::memset(g.pool[s].words, 0, 336);
        if (staleSlot >= 0) {
            sct::State shifted = state;
            shifted.pos[0] += staleShift;
            uint8_t stale[336];
            sct::writeRecord(shifted, 99u, stale);
            std::memcpy(g.pool[uint32_t(staleSlot)].words, stale, 336);
        }
        g.writePool(g.poolA.Get(), D3D11_MAP_WRITE_DISCARD);
    }

    // One eye's pass with the skinned pair (or the plain skinned family): the game's state, the engine's call, what it bound, the draw. `start` is the draw's
    // StartInstanceLocation (the stream entry it reads); the draw hook's second call (the draw's instance window) is made as vscreen's thunk makes it.
    // eye == kSource (L13) is the on-foot source's pass: the same draws into the source's own targets (a depth the screen shader named, no eye RTV), the
    // way Game::sourcePass draws the rigid ones.
    static constexpr UINT kDefaultEntry = 0xFFFFFFFFu;
    static constexpr int kSource = 2;
    void sourceTargets() {
        const float black[4] = {};
        for (auto& r : g.sourceRtv) ctx->ClearRenderTargetView(r.Get(), black);
        ctx->ClearDepthStencilView(g.sourceDsv.Get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 0.0f, 0);
        ID3D11RenderTargetView* r[4] = {g.sourceRtv[0].Get(), g.sourceRtv[1].Get(), g.sourceRtv[2].Get(), g.sourceRtv[3].Get()};
        ctx->OMSetRenderTargets(4, r, g.sourceDsv.Get());
        g.shadow(lt::BindSlot::Rtv0, r[0]);
        g.shadow(lt::BindSlot::Dsv0, g.sourceDsv.Get());
    }
    void pass(int eye, bool pair = true, bool observe = false, UINT startEntry = kDefaultEntry, ID3D11DepthStencilState* depthOverride = nullptr) {
        const UINT start = startEntry == kDefaultEntry ? drawEntry : startEntry;
        if (eye == kSource) sourceTargets(); else g.setTargets(eye);
        ctx->OMSetDepthStencilState(depthOverride ? depthOverride : g.depthState.Get(), 0);
        D3D11_VIEWPORT vp{0, 0, float(eye == kSource ? g.sourceW : lt::kW), float(eye == kSource ? g.sourceH : lt::kH), 0, 1};
        ctx->RSSetViewports(1, &vp);
        ctx->RSSetState(g.raster.Get());
        ctx->IASetInputLayout(layout.Get());
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D11Buffer* vbs[3] = {verts.Get(), instances.Get(), secondStream ? instances.Get() : nullptr};
        UINT strides[3] = {sizeof(sct::Vertex), 8, secondStream ? 8u : 0u}, offsets[3] = {0, 8, 0};
        ctx->IASetVertexBuffers(0, 3, vbs, strides, offsets);
        ID3D11ShaderResourceView* t38 = paletteSrv[count & 1u].Get();
        ctx->VSSetShaderResources(38, 1, &t38);
        if (pair) { g.setVs(vsPair.Get(), kPairVs); g.setPs(psPair.Get(), kPairPs); }
        else { g.setVs(vsPlain.Get(), kPlainVs); g.setPs(psPlain.Get(), kPlainPs); }
        edvr::engineVelocityBeforeDraw(ctx, eye != kSource);
        edvr::engineVelocityNoteSkinDraw(ctx, start, 1);
        if (observe) look();
        ctx->DrawInstanced(3, 1, 0, start);
    }

    // What the engine bound for the draw about to be issued.
    void look() {
        seen = Seen{};
        ID3D11RenderTargetView* rt[8] = {};
        ComPtr<ID3D11DepthStencilView> dsv;
        ctx->OMGetRenderTargets(8, rt, &dsv);
        seen.rt7 = rt[7] != nullptr;
        for (auto* r : rt) if (r) r->Release();
        ComPtr<ID3D11BlendState> bs;
        float f[4];
        UINT mask;
        ctx->OMGetBlendState(&bs, f, &mask);
        if (bs) {
            D3D11_BLEND_DESC d{};
            bs->GetDesc(&d);
            seen.blend7 = d.RenderTarget[7].RenderTargetWriteMask;
            seen.blend6 = d.RenderTarget[6].RenderTargetWriteMask;
            seen.derived = d.IndependentBlendEnable != FALSE;
        }
        ComPtr<ID3D11VertexShader> vs;
        ctx->VSGetShader(&vs, nullptr, nullptr);
        seen.vsPatched = vs && vs.Get() != vsPair.Get() && vs.Get() != vsPlain.Get();
        ID3D11ShaderResourceView* srv[3] = {};
        ctx->VSGetShaderResources(108, 3, srv);
        for (int i = 0; i < 3; ++i) { seen.srv[i] = srv[i] != nullptr; if (srv[i]) srv[i]->Release(); }
    }

    // A whole frame: the chain, the pool, the scene rows, both eyes; the hook's list when asked; then `after` before the boundary.
    template <class After> void frame(bool pair, After after, bool summary = false, bool observe = true) {
        g.beginFrame();
        writeSceneRows();
        chain(count);
        if (useHookList) {
            sjw::Built b = built;
            b.snap.seq = ++hookSeq;
            hookSnapshot = b.snap;
            lifecycle_fake::g_hookSnap = &hookSnapshot;
        } else {
            lifecycle_fake::g_hookSnap = nullptr;
        }
        writePool();
        g.writeScene(g.sceneA.Get(), rows[0]);
        pass(0, pair, observe);
        chainLate();   // (a no-op unless the test says the frame's second dispatch comes after a skinned draw)
        if (readStale) pass(0, pair, false, staleEntry);
        if (overdrawPlain) pass(0, false, false, 0, alwaysDepth.Get());
        g.writeScene(g.sceneA.Get(), rows[1]);
        pass(1, pair, false);
        if (readStale) pass(1, pair, false, staleEntry);
        if (overdrawPlain) pass(1, false, false, 0, alwaysDepth.Get());
        after();
        g.endFrame(summary);
        ctx->Flush();   // (a present would: the counters' staging copies finish)
        ++count;
    }
    template <class After> void frame(After after) { frame(true, after); }
    // A frame whose chain no skinned draw needed (a character out of view): the chain, the hook's list and the pool, no pass.
    void chainOnlyFrame(bool summary = false) {
        g.beginFrame();
        writeSceneRows();
        chain(count);
        if (useHookList) {
            sjw::Built b = built;
            b.snap.seq = ++hookSeq;
            hookSnapshot = b.snap;
            lifecycle_fake::g_hookSnap = &hookSnapshot;
        } else {
            lifecycle_fake::g_hookSnap = nullptr;
        }
        writePool();
        g.endFrame(summary);
        ctx->Flush();
        ++count;
    }
    edvr::skinjoin::Snapshot hookSnapshot;

    // An on-foot frame whose pool draw is the skinned pair (L13): the chain, the pool, the source's rows, the screen shader naming the source depth, then
    // the pass into the source's targets. `after` runs before the boundary (the screen shader's views are asked there).
    template <class After> void sourceFrame(bool pair, After after, bool summary = false, bool observe = false) {
        g.beginFrame();
        writeSceneRows();
        chain(count);
        lifecycle_fake::g_hookSnap = nullptr;
        writePool();
        g.writeScene(g.sceneA.Get(), rows[0]);
        edvr::engineVelocityNoteSource(g.sourceDepth.Get(), g.sceneA.Get());
        pass(kSource, pair, observe);
        after();
        g.endFrame(summary);
        ctx->Flush();
        ++count;
    }
    template <class After> void sourceFrame(After after) { sourceFrame(true, after); }
    // The same frame with a RIGID pool draw in the source pass and no skinned draw at all.
    template <class After> void sourceRigidFrame(After after) {
        g.beginFrame();
        writeSceneRows();
        chain(count);
        lifecycle_fake::g_hookSnap = nullptr;
        writePool();
        g.writeScene(g.sceneA.Get(), rows[0]);
        edvr::engineVelocityNoteSource(g.sourceDepth.Get(), g.sceneA.Get());
        g.sourcePass();
        after();
        g.endFrame();
        ctx->Flush();
        ++count;
    }

    // Target 7 of `eye` as the compose gets it (null view: nothing was written this frame), as floats, and the drawn pixels from the eye's depth.
    struct Eye { bool given = false; std::vector<float> e; std::vector<float> depth; UINT w = 0; };
    Eye read(int eye) {
        Eye out;
        ID3D11ShaderResourceView* view = edvr::engineVelocitySkinView(eye, g.depth[eye].Get());
        out.given = view != nullptr;
        if (!view) return out;
        fillFrom(view, out);
        view->Release();
        UINT dw = 0;
        out.depth = lt::readTexture(h, g.depth[eye].Get(), 1, &dw);
        return out;
    }
    // Target 7 of the on-foot source as the screen shader gets it: the source's views (EngineVelocityViews::skin), or `views` true and `given` false when the
    // views came without one (no skinned draw wrote it this frame), as floats, and the drawn pixels from the source depth.
    struct SourceEye : Eye { bool views = false; edvr::EngineVelocityViews v{}; };
    SourceEye readSource() {
        SourceEye out;
        edvr::EngineVelocityViews v{};
        out.views = edvr::engineVelocitySourceViews(g.sourceDepth.Get(), &v);
        out.v = v;
        out.given = v.skin != nullptr;
        if (v.slots) v.slots->Release();
        if (v.pool) v.pool->Release();
        if (v.sceneNow) v.sceneNow->Release();
        if (v.scenePrev) v.scenePrev->Release();
        if (v.gameMark) v.gameMark->Release();
        if (v.skin) {
            fillFrom(v.skin, out);
            v.skin->Release();
            UINT dw = 0;
            const std::vector<float> d2 = lt::readTexture(h, g.sourceDepth.Get(), 2, &dw);   // D32_FLOAT_S8X24: eight bytes a texel, the depth first
            out.depth.resize(d2.size() / 2);
            for (size_t p = 0; p < out.depth.size(); ++p) out.depth[p] = d2[p * 2];
        }
        out.v = {};
        return out;
    }
    void fillFrom(ID3D11ShaderResourceView* view, Eye& out) {
        ComPtr<ID3D11Resource> res;
        view->GetResource(&res);
        ComPtr<ID3D11Texture2D> tex;
        res.As(&tex);
        D3D11_TEXTURE2D_DESC d{};
        tex->GetDesc(&d);
        h.check(d.Format == DXGI_FORMAT_R16G16B16A16_FLOAT, "L: target 7 is R16G16B16A16_FLOAT");
        d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ; d.MiscFlags = 0;
        ComPtr<ID3D11Texture2D> staging;
        h.check(SUCCEEDED(dev->CreateTexture2D(&d, nullptr, &staging)), "L: a staging texture for target 7");
        ctx->CopyResource(staging.Get(), tex.Get());
        D3D11_MAPPED_SUBRESOURCE m{};
        h.check(SUCCEEDED(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m)), "L: map target 7");
        out.w = d.Width;
        out.e.resize(size_t(d.Width) * d.Height * 4);
        for (UINT y = 0; y < d.Height; ++y) {
            const uint16_t* row = reinterpret_cast<const uint16_t*>(static_cast<const BYTE*>(m.pData) + y * m.RowPitch);
            for (UINT x = 0; x < d.Width * 4; ++x) out.e[size_t(y) * d.Width * 4 + x] = halfToFloat(row[x]);
        }
        ctx->Unmap(staging.Get(), 0);
    }
};

// Over the drawn pixels: how many, how many carry valid 1, and the worst deviation of E from `expect` (cm).
struct Judged { unsigned drawn = 0, valid = 0, invalid = 0; double worst = 0.0; double outside = 0.0; };
inline Judged judge(const Fixture::Eye& eye, const float (&expect)[3]) {
    Judged j;
    for (size_t p = 0; p < eye.depth.size(); ++p) {
        const float* e = &eye.e[p * 4];
        if (!(eye.depth[p] > 0.0f)) {
            j.outside = std::max<double>(j.outside, std::max({std::fabs(double(e[0])), std::fabs(double(e[1])), std::fabs(double(e[2])), std::fabs(double(e[3]))}));
            continue;
        }
        ++j.drawn;
        if (e[3] > 0.99f && e[3] < 1.01f) ++j.valid;
        else if (e[3] == 0.0f) ++j.invalid;
        for (int c = 0; c < 3; ++c) j.worst = std::max(j.worst, std::fabs(double(e[c]) - double(expect[c])));
    }
    return j;
}

// The first line since `from` that starts with prefix (the first window holds the counters; a later one with no new read-back reports zeros).
inline std::string firstLine(const char* prefix, size_t from) {
    for (size_t i = from; i < lt::g_log.size(); ++i)
        if (lt::g_log[i].rfind(prefix, 0) == 0) return lt::g_log[i];
    return {};
}

// The last line since `from` that starts with prefix.
inline std::string lastLine(const char* prefix, size_t from) {
    for (size_t i = lt::g_log.size(); i > from; --i)
        if (lt::g_log[i - 1].rfind(prefix, 0) == 0) return lt::g_log[i - 1];
    return {};
}

inline void run(const lt::Harness& h) {
    edvr::g_clockForTest = &lifecycle_fake::fakeClock;
    const size_t mark = lt::g_log.size();
    lifecycle_fake::g_hookSnap = nullptr;
    lifecycle_fake::g_hookArmed = false;
    lifecycle_fake::g_hookArms = 0;
    lt::Game g(h);
    g.setup();
    edvr::engineVelocityConfigure(true);
    Fixture f(h, g);
    if (!f.setup()) return;
    const float zero[3] = {0, 0, 0};

    // L1
    h.check(lt::logged("skin join: the second skin is live (VR)", mark), "L1.a configure says the second skin is live");
    h.check(lifecycle_fake::g_hookArms == 1 && lifecycle_fake::g_hookGate, "L1.b the hook is asked to arm once and its gate is opened");
    h.check(lt::logged("skin join: the hook stood down", mark) || lt::logged("skin join: rig: the entity hook is not linked", mark), "L1.c the stand-down of the hook is said, with its reason");
    h.check(edvr::engineVelocitySkinWanted(), "L1.d the chain dispatch hook is told to feed the join");

    // L2: the first frame
    Fixture::Eye first0, first1;
    f.frame([&] { first0 = f.read(0); first1 = f.read(1); f.g.views(0); f.g.views(1); });   // (the compose asks for both eyes' views: the entry fade's signal is armed from here)
    h.check(f.seen.rt7 && f.seen.vsPatched, "L6.a the first skinned draw has target 7 bound and its patched vertex shader");
    h.check(first0.given && first1.given, "L2.a both eyes give the compose a target-7 view after a frame that wrote E");
    {
        const Judged a = judge(first0, zero), b = judge(first1, zero);
        h.check(a.drawn > 20 && b.drawn > 20, "L2.b the triangle is drawn in both eyes");
        h.check(a.valid == 0 && b.valid == 0 && a.worst == 0.0 && b.worst == 0.0, "L2.c with no history every drawn pixel carries valid 0 and E 0 (no stale answer)");
        h.check(a.outside == 0.0 && b.outside == 0.0, "L2.d target 7 is zero where nothing was drawn (cleared with the eye-frame)");
        const edvr::EngineMotionReady m = edvr::engineMotionReady();
        h.check(m.armed && m.skinJobs && !m.skinLive, "L2.e the entry fade's signal for that frame: skinned jobs, the join not live (no history yet: the fade waits for it)");
        if (!(m.armed && m.skinJobs && !m.skinLive)) std::fprintf(stderr, "  signal: armed %d views run %u skin jobs %d live %d\n", int(m.armed), m.viewsRun, int(m.skinJobs), int(m.skinLive));
    }

    // L3: a steady second frame, the prefix join
    Fixture::Eye steady0, steady1;
    f.frame([&] { steady0 = f.read(0); steady1 = f.read(1); f.g.views(0); f.g.views(1); });
    {
        const Judged a = judge(steady0, zero), b = judge(steady1, zero);
        h.check(a.drawn > 20 && a.valid == a.drawn && b.valid == b.drawn, "L3.a a steady frame: valid 1 at every drawn pixel in both eyes");
        h.check(a.worst == 0.0 && b.worst == 0.0, "L3.b and E is exactly zero (previous state = current state through the whole engine path)");
        h.check(a.outside == 0.0, "L3.c and still zero outside the drawn pixels");
        const edvr::EngineMotionReady m = edvr::engineMotionReady();
        h.check(m.armed && m.skinJobs && m.skinLive && m.viewsRun >= 1, "L3.d the entry fade's signal for that frame: skinned jobs, the join live, the views given to both eyes");
    }
    // a smaller triangle: the pixels the larger one wrote are zero again (target 7 is cleared with every eye-frame)
    {
        f.setTriangleScale(0.5f);
        Fixture::Eye smaller0;
        f.frame([&] { smaller0 = f.read(0); });
        f.setTriangleScale(1.0f);
        const Judged a = judge(smaller0, zero), larger = judge(steady0, zero);
        h.check(a.drawn > 5 && a.drawn < larger.drawn && a.valid == a.drawn && a.outside == 0.0, "L3.d a smaller triangle: its pixels are valid and the pixels the larger one wrote are zero again (cleared per eye-frame)");
    }
    // the compose's view counts: a frame with no exporting draw gives none
    {
        f.frame(false, [&] { const auto e0 = f.read(0); h.check(!e0.given, "L6.b a skinned family's other pixel shader writes no E: the compose is given no target-7 view"); });
    }

    // L4: the character moves 0.15 m, -0.1 m, 0.05 m between frames
    f.frame([&] {});
    sct::State was = f.state;
    for (int c = 0; c < 3; ++c) f.state.pos[c] += (c == 0 ? 0.15f : c == 1 ? -0.1f : 0.05f);
    Fixture::Eye moved0, moved1;
    f.frame([&] { moved0 = f.read(0); moved1 = f.read(1); });
    {
        const float expect[3] = {100.0f * (was.pos[0] - f.state.pos[0]), 100.0f * (was.pos[1] - f.state.pos[1]), 100.0f * (was.pos[2] - f.state.pos[2])};
        const Judged a = judge(moved0, expect), b = judge(moved1, expect);
        if (a.worst > 0.1) std::fprintf(stderr, "  L4: worst deviation %.4f cm from (%.2f %.2f %.2f)\n", a.worst, expect[0], expect[1], expect[2]);
        h.check(a.drawn > 20 && a.valid == a.drawn && b.valid == b.drawn, "L4.a a moving character is valid in both eyes");
        h.check(a.worst <= 0.1 && b.worst <= 0.1, "L4.b E = 100 x (previous - current position), within half-float rounding");
        h.check(std::fabs(expect[0]) > 10.0, "L4.c (and the move was large enough to tell from zero)");
    }
    f.frame([&] {});   // the move is over: steady again

    // L5: the job table changes shape (the character gains a bone: its job count differs from last frame's)
    f.setWorld(4);
    {
        sct::Rng rng(31);
        sct::Vertex scratch[3];
        sct::makeVertices(rng, 4, scratch);
        f.state = sct::makeState(rng, 4, f.base);
    }
    Fixture::Eye changed0;
    f.frame([&] { changed0 = f.read(0); });
    {
        const Judged a = judge(changed0, zero);
        h.check(a.drawn > 20 && a.valid == 0 && a.worst == 0.0, "L5.a a job whose bone count changed has no history for that frame (valid 0, E 0)");
    }
    Fixture::Eye again0;
    f.frame([&] {});
    f.frame([&] { again0 = f.read(0); });
    {
        const Judged a = judge(again0, zero);
        h.check(a.drawn > 20 && a.valid == a.drawn && a.worst == 0.0, "L5.b and two frames on the character has history again");
    }

    // L6: what is bound for the draws, and put back
    {
        f.frame(true, [&] {}, false, true);
        const Seen pair = f.seen;
        h.check(pair.rt7 && pair.vsPatched && pair.srv[0] && pair.srv[1] && pair.srv[2], "L6.c the E-exporting pair draws with target 7 bound, the patched vertex shader, and the previous palette, join and pose views");
        h.check(pair.derived && pair.blend7 == D3D11_COLOR_WRITE_ENABLE_ALL && pair.blend6 == (D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN),
                "L6.d its derived blend state writes target 7's four channels and target 6's R and G");
        f.frame(false, [&] {}, false, true);
        const Seen plain = f.seen;
        h.check(plain.rt7 && plain.vsPatched && plain.derived && plain.blend7 == D3D11_COLOR_WRITE_ENABLE_ALL,
                "L6.e a skinned family's other pixel shader draws with target 7 written (it writes no history there: all four channels, never left to an earlier draw's E)");
        // a rigid family's draw: target 7 bound for the eye, its writes off
        g.beginFrame();
        f.writeSceneRows();
        g.writeScene(g.sceneA.Get(), f.rows[0]);
        g.pass(0);
        f.look();
        h.check(f.seen.derived && f.seen.blend7 == 0, "L6.f a rigid family's substituted draw leaves target 7 alone (write mask 0)");
        g.endFrame();
        // after the frame boundary the game's own state is back: no target 7, no skin views
        ComPtr<ID3D11RenderTargetView> rt[8];
        ID3D11RenderTargetView* raw[8] = {};
        h.context->OMGetRenderTargets(8, raw, nullptr);
        for (int i = 0; i < 8; ++i) rt[i].Attach(raw[i]);
        ID3D11ShaderResourceView* srv[3] = {};
        h.context->VSGetShaderResources(108, 3, srv);
        bool none = true;
        for (auto* s : srv) { none = none && s == nullptr; if (s) s->Release(); }
        ComPtr<ID3D11BlendState> bs;
        float bf[4];
        UINT bm;
        h.context->OMGetBlendState(&bs, bf, &bm);
        ComPtr<ID3D11VertexShader> vsNow;
        h.context->VSGetShader(&vsNow, nullptr, nullptr);
        h.check(none && !bs && vsNow.Get() == g.vs.Get(), "L6.g at the frame boundary the three skin views are let go and the game's blend state and vertex shader are back");
    }
    // a game that binds its own target 7 keeps it
    {
        f.frame([&] {});   // a normal frame first so the eye is set up
        g.beginFrame();
        f.writeSceneRows();
        f.chain(f.count);
        f.writePool();
        g.writeScene(g.sceneA.Get(), f.rows[0]);
        ComPtr<ID3D11Texture2D> gameTarget;
        D3D11_TEXTURE2D_DESC td{};
        td.Width = lt::kW; td.Height = lt::kH; td.MipLevels = 1; td.ArraySize = 1; td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_RENDER_TARGET; td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        ComPtr<ID3D11RenderTargetView> gameRtv;
        h.check(SUCCEEDED(h.device->CreateTexture2D(&td, nullptr, &gameTarget)) && SUCCEEDED(h.device->CreateRenderTargetView(gameTarget.Get(), nullptr, &gameRtv)), "L: a target of the game's own");
        ID3D11RenderTargetView* r[8] = {g.rtv[0][0].Get(), g.rtv[0][1].Get(), g.rtv[0][2].Get(), g.rtv[0][3].Get(), nullptr, nullptr, nullptr, gameRtv.Get()};
        h.context->OMSetRenderTargets(8, r, g.dsv[0].Get());
        lifecycle_fake::g_slots[static_cast<unsigned>(edvr::BindSlot::Rtv0)].ptr = r[0];
        ++lifecycle_fake::g_slots[static_cast<unsigned>(edvr::BindSlot::Rtv0)].gen;
        lifecycle_fake::g_slots[static_cast<unsigned>(edvr::BindSlot::Dsv0)].ptr = g.dsv[0].Get();
        ++lifecycle_fake::g_slots[static_cast<unsigned>(edvr::BindSlot::Dsv0)].gen;
        h.context->OMSetDepthStencilState(g.depthState.Get(), 0);
        D3D11_VIEWPORT vp{0, 0, float(lt::kW), float(lt::kH), 0, 1};
        h.context->RSSetViewports(1, &vp);
        h.context->RSSetState(g.raster.Get());
        h.context->IASetInputLayout(f.layout.Get());
        h.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D11Buffer* vbs[2] = {f.verts.Get(), f.instances.Get()};
        UINT strides[2] = {sizeof(sct::Vertex), 8}, offsets[2] = {0, 8};
        h.context->IASetVertexBuffers(0, 2, vbs, strides, offsets);
        ID3D11ShaderResourceView* t38 = f.paletteSrv[f.count & 1u].Get();
        h.context->VSSetShaderResources(38, 1, &t38);
        g.setVs(f.vsPair.Get(), kPairVs);
        g.setPs(f.psPair.Get(), kPairPs);
        edvr::engineVelocityBeforeDraw(h.context, true);
        ID3D11RenderTargetView* now[8] = {};
        h.context->OMGetRenderTargets(8, now, nullptr);
        const bool kept = now[7] == gameRtv.Get();
        for (auto* x : now) if (x) x->Release();
        h.check(kept, "L6.h a game that binds its own target 7 keeps it: the engine does not replace it");
        h.context->DrawInstanced(3, 1, 0, 0);
        g.endFrame();
        ++f.count;
    }

    // L7: the hook's list as the identity
    lifecycle_fake::g_hookArmed = true;
    f.useHookList = true;
    f.setWorld(3);
    {
        sct::Rng rng(31);
        sct::Vertex scratch[3];
        sct::makeVertices(rng, 3, scratch);
        f.state = sct::makeState(rng, 3, f.base);
    }
    for (int i = 0; i < 3; ++i) f.frame([&] {});
    Fixture::Eye hooked0;
    f.frame([&] { hooked0 = f.read(0); });
    {
        const Judged a = judge(hooked0, zero);
        h.check(a.drawn > 20 && a.valid == a.drawn && a.worst == 0.0, "L7.a with the hook's list the steady frames join: valid 1, E exactly zero");
    }
    // the table disagrees with the (well formed) list: the GPU finds it, the frame uses the prefix join (which sees a job whose bone count changed:
    // no history) and the counters say so (L8 reads them)
    {
        const sjw::Built keep = f.built;
        f.built.jobs[0].count -= 1;
        Fixture::Eye fallback0;
        f.frame([&] { fallback0 = f.read(0); });
        f.built = keep;
        const Judged a = judge(fallback0, zero);
        h.check(a.drawn > 20 && a.valid == 0, "L7.b a job table that disagrees with the hook's list is not joined by the list: that frame has no history (the prefix saw the table change)");
    }
    for (int i = 0; i < 3; ++i) f.frame([&] {});
    Fixture::Eye recovered0;
    f.frame([&] { recovered0 = f.read(0); });
    {
        const Judged a = judge(recovered0, zero);
        h.check(a.drawn > 20 && a.valid == a.drawn && a.worst == 0.0, "L7.c and the frames after it join by the hook again");
    }
    h.check(!lifecycle_fake::g_hookChainJobs.empty() && lifecycle_fake::g_hookChainJobs.back() == uint32_t(f.built.jobs.size()) &&
                lifecycle_fake::g_hookChainJobs.size() >= 10,
            "L7.d every chain dispatch the join takes tells the hook how many jobs its table has (an empty list is judged only against a table with jobs)");

    // L8: the periodic lines (the counters come back from the GPU after 120 chain frames, never waited for; the summary is due every 30 s)
    // (WARP runs behind the CPU: each frame reads target 7 back, which waits for everything queued, the counters' staging copy included)
    while (f.count < 128) f.frame(true, [&] { f.read(0); }, false, false);
    for (int i = 0; i < 3; ++i) f.frame(true, [&] { f.read(0); }, true, false);
    {
        const std::string join = firstLine("skin join: source=", mark);
        h.check(!join.empty(), "L8.a the join line is logged once the first counters are read back");
        h.check(join.find("hook=armed") != std::string::npos && lt::number(join, "frames=") >= 100 && lt::number(join, "joined=") > 0 && lt::number(join, "jobs=") > 0,
                "L8.b it names the hook's state, the frames the join ran and the jobs it joined");
        h.check(join.find("source=hook") != std::string::npos, "L8.c the source is the hook's list (with the prefix where the table disagreed)");
        h.check(lt::number(join, "hook/t0 disagreements ") >= 1, "L8.d the disagreement of L7.b is counted");
        const std::string second = firstLine("skin join: second skin this window:", mark);
        h.check(!second.empty() && lt::number(second, "binds writing E ") > 0, "L8.e the second-skin line counts the binds that wrote E");
        h.check(!firstLine("skin join: hook window:", mark).empty(), "L8.f the hook's line says what the hook saw in the window");
        const std::string witness = firstLine("skin join: pose witness:", mark);
        h.check(!witness.empty() && witness.find("conflicts resolved ") != std::string::npos && witness.find("reference lists exact ") != std::string::npos &&
                    lt::number(witness, "tables built ") >= 100 && lt::number(witness, "reference lists exact ") >= 100,
                "L8.g the pose witness line follows the join line: the tables built and the draw lists that were exact");
        if (join.empty() || join.find("hook=armed") == std::string::npos || lt::number(join, "hook/t0 disagreements ") < 1)
            std::fprintf(stderr, "  join line: %s\n", join.c_str());
        if (join.empty())
            for (size_t i = mark; i < lt::g_log.size(); ++i)
                if (lt::g_log[i].rfind("skin join:", 0) == 0) std::fprintf(stderr, "  log: %.300s\n", lt::g_log[i].c_str());
    }

    // L8.h: the join line's two halves are the same frames. A summary frame that waits for nothing (no read-back of target 7): the newest finished
    // counter read-back is then older than the CPU's own counters, and a CPU half taken at the summary instead of with the GPU's copy would count a
    // frame the GPU half does not.
    // (Frames that read target 7 back come first, so the window the summary closes holds several finished GPU frames: a window of none would pass the
    // equality below for any CPU half that counted nothing.)
    for (int i = 0; i < 5; ++i) f.frame(true, [&] { f.read(0); }, false, false);
    f.frame(true, [&] {}, true, false);
    {
        const std::string last = lastLine("skin join: source=", mark);
        const auto n = [&](const char* label) { return lt::number(last, label); };
        const unsigned long long declined = n("declined [no history ") + n(", no snapshot ") + n(", stale ") + n(", gap ") + n(", unusable ") + n(", no previous ");
        h.check(!last.empty() && n("frames=") >= 3 && n("frames=") == n("offered ") + declined,
                "L8.h the join line's GPU half (frames the join ran) equals its CPU half (frames the feeder offered the hook or declined): the same frames");
        if (!last.empty() && (n("frames=") < 3 || n("frames=") != n("offered ") + declined)) std::fprintf(stderr, "  join line: %s\n", last.c_str());
    }

    // L8.i-k: the signals the entry fade waits on (engineMotionReady). The compose asks for the views of both eyes each frame: after a few such frames the
    // engine's motion is armed with a run of live views and the join live for the skinned jobs. A temporal pass that asks for one eye only never builds a
    // run, and one that never asks leaves the signal unarmed (there is nothing to wait for; a pass that does not consume the views would never satisfy it).
    for (int i = 0; i < 4; ++i) f.frame(true, [&] { f.g.views(0); f.g.views(1); }, false, false);
    {
        const edvr::EngineMotionReady m = edvr::engineMotionReady();
        h.check(m.armed && m.viewsRun >= 3 && m.skinJobs && m.skinLive,
                "L8.i the entry fade's signal after four frames in which both eyes were given the views: armed, a run of at least 3 frames, skinned jobs, the join live");
    }
    for (int i = 0; i < 4; ++i) f.frame(true, [&] { f.g.views(0); }, false, false);
    {
        const edvr::EngineMotionReady m = edvr::engineMotionReady();
        h.check(m.armed && m.viewsRun == 0, "L8.j a pass that asks for one eye's views only never builds a run (armed: it consumes them, but both eyes are needed)");
    }
    for (int i = 0; i < 34; ++i) f.frame(true, [&] {}, false, false);
    {
        const edvr::EngineMotionReady m = edvr::engineMotionReady();
        h.check(!m.armed && m.viewsRun == 0 && !m.skinJobs && !m.skinLive, "L8.k a pass that has not asked for the views in 34 frames leaves the signal unarmed: nothing is waited for");
    }

    // L10: which record of the character's base is the live one. The pool also holds a stale second record of the same base and another pose (the F12
    // flight's shape); the draws' own instance-stream entries say which record they read, and nothing else does. The stream is bound at an IA offset of
    // 8 (entry 0 is the live record), and holds entries naming the stale slots that no draw's window covers.
    lifecycle_fake::g_hookSnap = nullptr;
    f.useHookList = false;
    lifecycle_fake::g_hookArmed = false;
    f.setWorld(3);
    {
        sct::Rng rng(31);
        sct::Vertex scratch[3];
        sct::makeVertices(rng, 3, scratch);
        f.state = sct::makeState(rng, 3, f.base);
    }
    const auto settle = [&] { for (int i = 0; i < 4; ++i) f.frame([&] {}); };
    const auto seen0 = [&] { Fixture::Eye e0; f.frame([&] { e0 = f.read(0); }); return e0; };
    settle();
    {
        const Judged a = judge(seen0(), zero);
        h.check(a.drawn > 20 && a.valid == a.drawn && a.worst == 0.0, "L10.a (control) with no stale record the character has history and E is exactly zero");
    }
    f.staleSlot = 11; f.staleEntry = 1;
    settle();
    {
        const Judged a = judge(seen0(), zero);
        h.check(a.drawn > 20 && a.valid == a.drawn && a.worst == 0.0, "L10.b a stale second record of the base at a HIGHER slot, read by no draw (its pose 0.12 m off): the live one is kept, valid 1 and E exactly zero");
    }
    f.staleSlot = 2; f.staleEntry = 2;
    settle();
    {
        const Judged a = judge(seen0(), zero);
        h.check(a.drawn > 20 && a.valid == a.drawn && a.worst == 0.0, "L10.c ...and at a LOWER slot (the first writer is not the live record)");
    }
    f.staleSlot = 11; f.staleEntry = 1; f.readStale = true;
    settle();
    {
        const Judged a = judge(seen0(), zero);
        h.check(a.drawn > 20 && a.valid == 0 && a.worst == 0.0, "L10.d both records read by draws: no history for the base (valid 0, E 0), never a guess between them");
    }
    f.readStale = false;
    settle();
    {
        const Judged a = judge(seen0(), zero);
        h.check(a.drawn > 20 && a.valid == a.drawn && a.worst == 0.0, "L10.e the frames after the draws stop reading the stale record have history again");
    }
    f.secondStream = true;
    settle();
    {
        const Judged a = judge(seen0(), zero);
        h.check(a.drawn > 20 && a.valid == 0 && a.worst == 0.0, "L10.f a frame whose draw list cannot be exact (a second vertex buffer of stride 8 is bound) lets every record decide: the stale record kills the base");
    }
    f.secondStream = false;
    settle();
    {
        const Judged a = judge(seen0(), zero);
        h.check(a.drawn > 20 && a.valid == a.drawn && a.worst == 0.0, "L10.g ...and with the list exact again the base has history");
    }
    f.staleSlot = -1;
    settle();
    f.frame(true, [&] {}, true, false);
    {
        const std::string pw = lastLine("skin join: pose witness:", mark);
        h.check(!pw.empty() && lt::number(pw, "conflicts resolved ") >= 5 && lt::number(pw, "bases dropped ") >= 5 && lt::number(pw, "unresolved ") >= 5 && lt::number(pw, "no exact list ") >= 1 &&
                    lt::number(pw, "not complete ") >= 3,
                "L10.h the witness line counts the stale records overruled, the bases dropped and what they were (unresolved: both records read; no exact list: the second stream) and the lists that were not complete");
        if (pw.empty() || lt::number(pw, "conflicts resolved ") < 5) std::fprintf(stderr, "  pose witness: %s\n", pw.c_str());
    }
    // L10.i: a base whose records disagree and which NO draw reads is idle (a character out of view), not unresolved: the draws read an empty slot's record
    // (entry 3), the live record and the stale one are both unread.
    f.staleSlot = 11; f.staleEntry = 1; f.drawEntry = 3;
    // (frames that read target 7 back wait for the GPU, so the counters of each have come back when the summary is written)
    for (int i = 0; i < 3; ++i) f.frame(true, [&] { f.read(0); }, false, false);
    f.frame(true, [&] { f.read(0); }, true, false);   // closes the window the lines above were read from
    for (int i = 0; i < 4; ++i) f.frame(true, [&] { f.read(0); }, false, false);
    f.frame(true, [&] { f.read(0); }, true, false);
    {
        const std::string pw = lastLine("skin join: pose witness:", mark);
        h.check(!pw.empty() && lt::number(pw, "idle ") >= 2 && lt::number(pw, "unresolved ") == 0,
                "L10.i a base whose records disagree and which no draw read is idle, not unresolved: the witness tells a character out of view from a conflict between records a draw read");
        if (pw.empty() || lt::number(pw, "idle ") < 2 || lt::number(pw, "unresolved ") != 0) std::fprintf(stderr, "  pose witness: %s\n", pw.c_str());
    }
    f.drawEntry = 0; f.staleSlot = -1;
    settle();

    // L11: a skinned family's pixel shader that exports no E draws over pixels an exporting one has just written. It writes no history there (valid 0,
    // exactly zero): masked off instead, those pixels would keep the E under the surface it draws, which is another surface's answer.
    settle();
    f.overdrawPlain = true;
    Fixture::Eye over0, over1;
    f.frame([&] { over0 = f.read(0); over1 = f.read(1); });
    f.overdrawPlain = false;
    {
        const Judged a = judge(over0, zero), b = judge(over1, zero);
        h.check(over0.given && over1.given && a.drawn > 20 && a.valid == 0 && b.valid == 0 && a.worst == 0.0 && b.worst == 0.0 && a.outside == 0.0,
                "L11.a pixels an E-exporting draw wrote and a skinned family's other pixel shader then drew over carry valid 0 and E 0 (it writes no history), not the E beneath");
    }
    settle();
    {
        const Judged a = judge(seen0(), zero);
        h.check(a.drawn > 20 && a.valid == a.drawn && a.worst == 0.0, "L11.b the frame after it, with the exporting pair alone, is whole again");
    }

    // L12: the palette chain dispatched twice in one present frame. The game does (F13, the settlement: dispatch 0 the characters it always had, dispatch 1 a few
    // more into the same palette buffer, each dispatch its own job table). The join is ONE per present frame over the union of the dispatches' job tables in
    // dispatch order, so the character in either dispatch keeps its history, on the double frame and on the frame after it.
    lifecycle_fake::g_hookSnap = nullptr;
    f.useHookList = false;
    lifecycle_fake::g_hookArmed = false;
    f.staleSlot = -1; f.drawEntry = 0; f.readStale = false; f.secondStream = false; f.overdrawPlain = false;
    const auto single = [&] { f.setWorld(3); f.secondDispatch.clear(); f.sameJobsBuffer = false; f.lateSecond = false; f.secondOnOtherPalette = false; };
    const auto doubled = [&] { f.setWorldTwo(3); f.secondDispatch = {1}; f.sameJobsBuffer = false; f.lateSecond = false; f.secondOnOtherPalette = false; };
    single();
    {
        sct::Rng rng(31);
        sct::Vertex scratch[3];
        sct::makeVertices(rng, 3, scratch);
        f.state = sct::makeState(rng, 3, f.base);
    }
    const auto judged = [&] { Fixture::Eye e0; f.frame([&] { e0 = f.read(0); }); return judge(e0, zero); };
    const auto whole = [&](const Judged& a) { return a.drawn > 20 && a.valid == a.drawn && a.worst == 0.0; };
    settle();
    doubled();
    {
        const Judged a = judged();
        h.check(whole(a), "L12.a a frame whose chain is dispatched twice (the drawn character in the first, another in the second): the character has history, valid 1 and E exactly zero");
    }
    single();
    {
        const Judged a = judged();
        h.check(whole(a), "L12.b ...and the frame after it, whose previous tables are the double frame's union, has history too");
    }
    {
        const int pattern[] = {1, 1, 2, 2, 2, 1, 1, 2, 1, 2, 2, 1};   // one, two, one: the F13 dump's shape
        unsigned bad = 0, total = 0;
        for (const int k : pattern) {
            if (k == 2) doubled(); else single();
            const Judged a = judged();
            ++total;
            bad += whole(a) ? 0u : 1u;
        }
        h.check(bad == 0 && total == 12, "L12.c a one-two-one sequence of frames (single, double, double, double, single ...): the character is whole in every one of them");
    }
    doubled();
    f.secondDispatch = {0};   // the drawn character in the SECOND dispatch, the extra one first: the union is not in row order
    settle();
    {
        const Judged a = judged();
        h.check(whole(a), "L12.d the drawn character in the second dispatch, the first dispatch's job at a higher row: the union's order does not matter");
    }
    doubled();
    f.sameJobsBuffer = true;   // the game rewrites ONE job table buffer between the dispatches: each table is taken at its own dispatch
    settle();
    {
        const Judged a = judged();
        h.check(whole(a), "L12.e one job table buffer rewritten between the two dispatches: both tables are joined (each is copied when its dispatch comes)");
    }
    single(); settle();
    doubled();
    f.lateSecond = true;   // the second dispatch comes after the frame's first skinned draw has needed the join
    {
        const Judged a = judged();
        h.check(whole(a), "L12.f a second dispatch AFTER a skinned draw has needed the join: the first dispatch's character still has history (the join is not run again)");
    }
    f.lateSecond = false;
    single();
    {
        const Judged a = judged();
        h.check(whole(a), "L12.g ...and the frame after it has history");
    }
    single(); settle();
    doubled();
    f.secondOnOtherPalette = true;   // the second dispatch writes the other palette buffer: which is last frame's is not measured
    {
        const Judged a = judged();
        h.check(a.drawn > 20 && a.valid == 0 && a.worst == 0.0, "L12.h a frame whose two dispatches wrote two palette buffers has no history (valid 0, E 0)");
    }
    f.secondOnOtherPalette = false;
    single();
    {
        const Judged a = judged();
        h.check(whole(a), "L12.i ...and the next frame has it again");
    }
    single();
    for (int i = 0; i < 4; ++i) f.frame(true, [&] { f.g.views(0); f.g.views(1); }, false, false);
    {
        const edvr::EngineMotionReady m = edvr::engineMotionReady();
        h.check(m.armed && m.skinJobs && m.skinLive && m.viewsRun >= 3, "L12.j (control) the entry fade's signal after single-dispatch frames: skinned jobs, the join live");
    }
    doubled();
    for (int i = 0; i < 4; ++i) f.frame(true, [&] { f.g.views(0); f.g.views(1); }, false, false);
    {
        const edvr::EngineMotionReady m = edvr::engineMotionReady();
        h.check(m.armed && m.skinJobs && m.skinLive && m.viewsRun >= 3,
                "L12.k the entry fade's signal after double-dispatch frames: skinned jobs and the join live (the frame's one join, not the second dispatch's)");
    }
    // a frame whose chain no skinned draw needed (a character out of view) is joined at the frame boundary all the same, so the signal describes THIS frame: a
    // chain-only frame whose two dispatches wrote two palette buffers has no history, and the entry fade must not read the frame before's join as live
    f.secondOnOtherPalette = true;
    f.chainOnlyFrame();
    f.secondOnOtherPalette = false;
    {
        const edvr::EngineMotionReady m = edvr::engineMotionReady();
        h.check(m.armed && m.skinJobs && !m.skinLive, "L12.o a frame whose chain no skinned draw needed is joined at the frame boundary: the signal says its join has no history, not the frame before's");
    }
    single();
    for (int i = 0; i < 3; ++i) f.frame([&] {});
    // the counters, in a window of nothing else: every frame has two dispatches, the hook's one list covers both tables
    lifecycle_fake::g_hookArmed = true;
    f.useHookList = true;
    doubled();
    for (int i = 0; i < 6; ++i) f.frame(true, [&] { f.read(0); }, false, false);
    f.frame(true, [&] { f.read(0); }, true, false);   // (closes the window before)
    for (int i = 0; i < 12; ++i) f.frame(true, [&] { f.read(0); }, false, false);
    f.frame(true, [&] { f.read(0); }, true, false);
    {
        const std::string chain = lastLine("skin join: chain dispatches", mark), join = lastLine("skin join: source=", mark);
        const unsigned long long frames = lt::number(chain, "over "), dispatches = lt::number(chain, "dispatches "), multi = lt::number(chain, "two or more in "), late = lt::number(chain, "late ");
        const std::string hist = join.find("history [") == std::string::npos ? std::string() : join.substr(join.find("history ["));
        h.check(frames >= 10 && dispatches == 2 * frames && multi == frames && late == 0,
                "L12.l the chain line: every frame of the window had two dispatches and one join (dispatches = 2 x frames, two or more in every frame, none late)");
        h.check(join.find("source=hook") != std::string::npos && lt::number(join, "hook/t0 disagreements ") == 0 && lt::number(hist, "gap ") == 0 && lt::number(join, "no history ") == 0,
                "L12.m ...and the join line: the hook's one list is judged against the union of the tables (no disagreement), no gap, no frame without history");
        if (!(frames >= 10 && dispatches == 2 * frames && multi == frames && late == 0)) std::fprintf(stderr, "  chain line: %s\n", chain.c_str());
        if (join.find("source=hook") == std::string::npos || lt::number(join, "hook/t0 disagreements ") != 0) std::fprintf(stderr, "  join line: %s\n", join.c_str());
    }
    f.useHookList = false;
    lifecycle_fake::g_hookSnap = nullptr;
    lifecycle_fake::g_hookArmed = false;
    doubled();
    f.lateSecond = true;
    for (int i = 0; i < 6; ++i) f.frame(true, [&] { f.read(0); }, false, false);
    f.frame(true, [&] { f.read(0); }, true, false);
    for (int i = 0; i < 10; ++i) f.frame(true, [&] { f.read(0); }, false, false);
    f.frame(true, [&] { f.read(0); }, true, false);
    {
        const std::string chain = lastLine("skin join: chain dispatches", mark);
        const unsigned long long frames = lt::number(chain, "over "), dispatches = lt::number(chain, "dispatches "), multi = lt::number(chain, "two or more in "), late = lt::number(chain, "late ");
        h.check(frames >= 8 && dispatches == 2 * frames && multi == 0 && late == frames,
                "L12.n the chain line when every second dispatch is late: two dispatches over one join a frame, none counted as part of the frame, every one counted late");
        if (!(frames >= 8 && dispatches == 2 * frames && multi == 0 && late == frames)) std::fprintf(stderr, "  chain line: %s\n", chain.c_str());
    }
    single();
    f.frame([&] {});

    // L13: the on-foot source (F2 on foot, the VR first-person panel). The source pass draws the same skinned pair into the source's own targets (a depth the
    // screen shader names, no eye RTV). The engine makes the source's own target 7 beside its slot target, clears it at the frame's first skinned draw, runs the
    // join and the pose table for it, and hands it to the screen shader and the world route as EngineVelocityViews::skin: only while a skinned draw wrote it this
    // frame, never in the eyes' views (their target 7 is the compose's engineVelocitySkinView), and never in the flat profile (it has no target 7).
    lifecycle_fake::g_hookSnap = nullptr;
    f.useHookList = false;
    lifecycle_fake::g_hookArmed = false;
    f.staleSlot = -1; f.drawEntry = 0; f.readStale = false; f.secondStream = false; f.overdrawPlain = false;
    single();
    {
        sct::Rng rng(31);
        sct::Vertex scratch[3];
        sct::makeVertices(rng, 3, scratch);
        f.state = sct::makeState(rng, 3, f.base);
    }
    settle();
    g.makeSource(64, 40);
    const size_t mark13 = lt::g_log.size();
    Fixture::SourceEye first13, steady13;
    f.sourceFrame([&] { first13 = f.readSource(); });
    h.check(!first13.views && !first13.given, "L13.a the first source frame has no previous scene constants: the screen shader is given no views, so no target 7");
    f.sourceFrame([&] { steady13 = f.readSource(); });
    const Judged st13 = judge(steady13, zero);
    h.check(steady13.views && steady13.given && st13.drawn > 20 && st13.valid == st13.drawn && st13.worst == 0.0 && st13.outside == 0.0,
            "L13.b a steady source frame: the screen shader's views carry the source's target 7, valid 1 at every drawn pixel, E exactly zero, zero outside the drawn pixels");
    h.check(lt::logged("skin join: on-foot source target 7 created 64x40 R16G16B16A16_FLOAT", mark13),
            "L13.c the log says the source's target 7 was created, at the source depth's size");
    f.sourceFrame([&] {});
    {
        const sct::State was = f.state;
        for (int c = 0; c < 3; ++c) f.state.pos[c] += (c == 0 ? 0.15f : c == 1 ? -0.1f : 0.05f);
        Fixture::SourceEye moved;
        f.sourceFrame([&] { moved = f.readSource(); });
        const float expect[3] = {100.0f * (was.pos[0] - f.state.pos[0]), 100.0f * (was.pos[1] - f.state.pos[1]), 100.0f * (was.pos[2] - f.state.pos[2])};
        const Judged a = judge(moved, expect);
        if (a.worst > 0.1) std::fprintf(stderr, "  L13.d: worst deviation %.4f cm from (%.2f %.2f %.2f)\n", a.worst, expect[0], expect[1], expect[2]);
        h.check(moved.given && a.drawn > 20 && a.valid == a.drawn && a.worst <= 0.1 && std::fabs(expect[0]) > 10.0,
                "L13.d a moving character seen on foot: E = 100 x (previous - current position) in centimetres at every drawn pixel of the source, valid 1");
    }
    f.sourceFrame([&] {});   // steady again
    // a frame whose source pass draws only a rigid record: the views come, with no target 7 (a skinned draw wrote none: a view of nothing would read as masked)
    {
        Fixture::SourceEye rigid;
        f.sourceRigidFrame([&] { rigid = f.readSource(); });
        h.check(rigid.views && !rigid.given, "L13.e a source frame with only a rigid pool draw: the screen shader gets its views but no target 7");
    }
    // a smaller triangle after it: target 7 was cleared at this frame's first skinned draw, so the pixels the larger one wrote two frames ago are zero again
    {
        f.setTriangleScale(0.5f);
        Fixture::SourceEye smaller;
        f.sourceFrame([&] { smaller = f.readSource(); });
        f.setTriangleScale(1.0f);
        const Judged a = judge(smaller, zero);
        h.check(smaller.given && a.drawn > 5 && a.drawn < st13.drawn && a.valid == a.drawn && a.outside == 0.0,
                "L13.f a smaller triangle seen on foot: its pixels are valid and the pixels the larger one wrote are zero again (target 7 is cleared at each frame's first skinned draw)");
    }
    f.sourceFrame([&] {});
    // the character's job table changes shape: no history for that frame (valid 0, E exactly zero: the consumers keep no history there), then it has it again
    f.setWorld(4);
    {
        sct::Rng rng(31);
        sct::Vertex scratch[3];
        sct::makeVertices(rng, 4, scratch);
        f.state = sct::makeState(rng, 4, f.base);
    }
    {
        Fixture::SourceEye changed, again;
        f.sourceFrame([&] { changed = f.readSource(); });
        const Judged a = judge(changed, zero);
        h.check(changed.given && a.drawn > 20 && a.valid == 0 && a.worst == 0.0,
                "L13.g a source frame whose job table changed shape has no history: valid 0 and E exactly zero at every drawn pixel (the consumers read it as no history, never as garbage)");
        f.sourceFrame([&] {});
        f.sourceFrame([&] { again = f.readSource(); });
        const Judged b = judge(again, zero);
        h.check(again.given && b.drawn > 20 && b.valid == b.drawn && b.worst == 0.0, "L13.h ...and two frames on the character has history again");
    }
    single();
    {
        sct::Rng rng(31);
        sct::Vertex scratch[3];
        sct::makeVertices(rng, 3, scratch);
        f.state = sct::makeState(rng, 3, f.base);
    }
    settle();
    // the eyes' views never carry the source's kind of target 7: their own is the compose's engineVelocitySkinView
    {
        edvr::EngineVelocityViews ev{};
        Fixture::Eye e13;
        f.frame([&] { e13 = f.read(0); f.g.views(0, &ev); });
        h.check(e13.given && ev.slots && ev.skin == nullptr, "L13.i the eyes' EngineVelocityViews carry no target 7 (the compose has its own view of it)");
        lt::release(ev);
    }
    // the flat profile has no target 7: skinned draws in it, on foot or in an eye pass, create none, the views carry none and the log says nothing of one
    {
        const auto previous = edvr::g_runtimeProfile;
        edvr::g_runtimeProfile = edvr::RuntimeProfile::Flat;
        const size_t flatMark = lt::g_log.size();
        Fixture::SourceEye flatSource;
        bool flatViews = false;
        for (int i = 0; i < 4; ++i) f.sourceFrame(true, [&] { flatSource = f.readSource(); flatViews = flatViews || flatSource.views; }, false, true);
        const Seen flatSeen = f.seen;
        Fixture::Eye flatEye;
        for (int i = 0; i < 4; ++i) f.frame([&] { flatEye = f.read(0); });
        edvr::g_runtimeProfile = previous;
        h.check(!flatSource.given && !flatEye.given, "L13.j in the flat profile neither the source's views nor an eye give a target 7");
        h.check(!lt::logged("target 7 created", flatMark), "L13.k in the flat profile no target 7 is created, for the source or an eye");
        h.check(!flatSeen.rt7 && !flatSeen.vsPatched && !flatSeen.srv[0] && !flatSeen.srv[1] && !flatSeen.srv[2],
                "L13.l in the flat profile a skinned family's draw is the game's own: no target 7 bound, no patched vertex shader, none of the three skin views");
        if (lt::logged("target 7 created", flatMark)) std::fprintf(stderr, "  flat profile made a target 7\n");
        (void)flatViews;
    }
    single();
    {
        sct::Rng rng(31);
        sct::Vertex scratch[3];
        sct::makeVertices(rng, 3, scratch);
        f.state = sct::makeState(rng, 3, f.base);
    }
    for (int i = 0; i < 3; ++i) f.frame([&] {});

    // L14: the GPU census slots the second skin fills (F16: gpu_census.h FrameSkinSourceClear..FrameSkinPose). The census stub counts the Begins per section, so
    // this reads which slots the engine path fills and which stay empty: each is begun by the work it prices and by nothing else, and the probe of the join's
    // 3-table clear, when the census times it, runs on scratch tables and changes nothing the join wrote.
    {
        using CS = edvr::GpuCensusSection;
        const auto begun = [](CS s) { return lifecycle_fake::g_censusBegins[static_cast<unsigned>(s)]; };
        struct Seen5 { unsigned src, eye, join, probe, pose; };
        const auto take = [&] { return Seen5{begun(CS::FrameSkinSourceClear), begun(CS::FrameSkinEyeClear), begun(CS::FrameSkinJoin), begun(CS::FrameSkinJoinClearProbe), begun(CS::FrameSkinPose)}; };
        single();
        {
            sct::Rng rng(31);
            sct::Vertex scratch[3];
            sct::makeVertices(rng, 3, scratch);
            f.state = sct::makeState(rng, 3, f.base);
        }
        settle();
        // a frame with no pool draw and no chain dispatch does none of the second skin's work: nothing is begun
        Seen5 a = take();
        g.beginFrame();
        g.endFrame();
        Seen5 b = take();
        h.check(b.src == a.src && b.eye == a.eye && b.join == a.join && b.probe == a.probe && b.pose == a.pose,
                "L14.a a frame with no pool draw and no chain dispatch begins none of the second skin's census slots");
        // an eye frame with the skinned pair in both eyes: each eye's target 7 clear, the join once, the pose table once, the probe's Begin with the join; never the source's clear
        a = take();
        f.frame([&] {});
        b = take();
        h.check(b.eye - a.eye == 2 && b.join - a.join == 1 && b.probe - a.probe == 1 && b.pose - a.pose == 1 && b.src == a.src,
                "L14.b an eye frame begins the eye clear once for each eye, and the join, the probe's slot and the pose table once; the source's clear not at all");
        if (!(b.eye - a.eye == 2 && b.join - a.join == 1 && b.probe - a.probe == 1 && b.pose - a.pose == 1 && b.src == a.src))
            std::fprintf(stderr, "  L14.b: eye %u join %u probe %u pose %u src %u\n", b.eye - a.eye, b.join - a.join, b.probe - a.probe, b.pose - a.pose, b.src - a.src);
        // a source frame with the skinned pair: the source's clear once, no eye clear
        a = take();
        f.sourceFrame([&] {});
        f.sourceFrame([&] {});
        b = take();
        h.check(b.src - a.src == 2 && b.eye == a.eye && b.join - a.join == 2,
                "L14.c two source frames begin the source's target 7 clear once each, the join once each, and no eye's clear");
        if (!(b.src - a.src == 2 && b.eye == a.eye && b.join - a.join == 2))
            std::fprintf(stderr, "  L14.c: src %u eye %u join %u\n", b.src - a.src, b.eye - a.eye, b.join - a.join);
        // a source frame that draws only a rigid record: no skinned draw, so the source's clear is not begun
        a = take();
        f.sourceRigidFrame([&] {});
        b = take();
        h.check(b.src == a.src && b.eye == a.eye,
                "L14.d a source frame with only a rigid pool draw begins neither the source's clear nor an eye's");
        // the census times the probe (the stub answers "timed"): the production probe dispatch runs on scratch tables, and what the join wrote is exactly what it was
        lifecycle_fake::g_censusTimeProbe = true;
        Fixture::Eye probed;
        for (int i = 0; i < 3; ++i) f.frame([&] {});
        f.frame([&] { probed = f.read(0); });
        lifecycle_fake::g_censusTimeProbe = false;
        const Judged pj = judge(probed, zero);
        h.check(probed.given && pj.drawn > 20 && pj.valid == pj.drawn && pj.worst == 0.0,
                "L14.e with the census timing the join-clear probe, the join's table is untouched: a steady character still has history, valid 1 and E exactly zero");
        // and without the census asking, a frame does no probe work at all: the same frames, the same result
        Fixture::Eye plain;
        f.frame([&] { plain = f.read(0); });
        const Judged pp = judge(plain, zero);
        h.check(plain.given && pp.drawn == pj.drawn && pp.valid == pp.drawn && pp.worst == 0.0, "L14.f the same frame with the probe off gives the same result");
    }
    single();
    {
        sct::Rng rng(31);
        sct::Vertex scratch[3];
        sct::makeVertices(rng, 3, scratch);
        f.state = sct::makeState(rng, 3, f.base);
    }
    for (int i = 0; i < 3; ++i) f.frame([&] {});

    // L9: a small previous palette buffer, no list: the job at a row near the end of the 64-row buffer
    lifecycle_fake::g_hookSnap = nullptr;
    f.useHookList = false;
    lifecycle_fake::g_hookArmed = false;
    {
        sjw::World w;
        w.push_back(sjw::Ent{901, 0xA11CE, 0, {{78, 37}}});
        w.push_back(sjw::Ent{902, 0xA11CE, 0, {{79, 3}}});
        f.built = sjw::build(w, 1);
        f.bones = 3;
        f.base = f.built.jobs[1].dst;   // 38: rows 38..40
        sct::Rng rng(31);
        sct::Vertex scratch[3];
        sct::makeVertices(rng, 3, scratch);
        f.state = sct::makeState(rng, 3, f.base);
    }
    for (int i = 0; i < 3; ++i) f.frame([&] {});
    Fixture::Eye fits0;
    f.frame([&] { fits0 = f.read(0); });
    h.check(judge(fits0, zero).valid == judge(fits0, zero).drawn && judge(fits0, zero).drawn > 20, "L9.a a job whose previous rows lie inside the previous palette buffer has history with no list at all");
    {
        // the same job, but the buffers are only 40 rows: row 40 (the job's third) is past their end
        f.paletteRows = 40;
        for (int i = 0; i < 2; ++i) {
            f.palette[i] = f.structured(nullptr, 48, f.paletteRows, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS);
            f.paletteSrv[i].Reset(); f.paletteUav[i].Reset();
            h.device->CreateShaderResourceView(f.palette[i].Get(), nullptr, &f.paletteSrv[i]);
            h.device->CreateUnorderedAccessView(f.palette[i].Get(), nullptr, &f.paletteUav[i]);
        }
        // (the character's third bone reads past the current buffer too -- zeros -- so the picture is a little smaller; the point of the case is the join)
        for (int i = 0; i < 3; ++i) f.frame([&] {});
        Fixture::Eye small0;
        f.frame([&] { small0 = f.read(0); });
        const Judged a = judge(small0, zero);
        if (!(a.drawn > 20 && a.valid == 0)) std::fprintf(stderr, "  L9.b: drawn %u valid %u invalid %u worst %.3f\n", a.drawn, a.valid, a.invalid, a.worst);
        h.check(a.drawn > 20 && a.valid == 0, "L9.b a job whose previous rows run past the previous palette buffer has no history, with no list");
    }
    std::printf("  skin lifecycle: %u frames through the engine, E exact in both eyes, the hook's list and the prefix, the guard on a small buffer\n", f.count);
    edvr::engineVelocityShutdown();
    h.context->ClearState();
    lifecycle_fake::g_hookSnap = nullptr;
    lifecycle_fake::g_hookArmed = false;
}

}  // namespace skin_lifecycle_tests
