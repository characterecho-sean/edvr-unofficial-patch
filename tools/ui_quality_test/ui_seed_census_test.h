// Included after the existing Gpu/Ds fixture: exercises the production CPU
// observer and the production Seeder, with no DLL or game dependency.
void testSeedCensusCpu() {
    UiSeedCensus c;
    const void* a = &c;
    const void* b = &g_checks;
    c.configure(false);
    auto p = c.plan(0, true, 10, a, 9, 7, true, true, false, 0, true, false, true);
    c.seed(0, true, 10, a, 9, 7, p, true);
    check(!c.seedEvents && c.observedDisabled, "seed census off observes no events");
    c.configure(true);
    c.seed(0, true, 10, a, 9, 7, p, true);
    check(c.seedEvents == 1 && c.matches(0, true, a), "diagnostics observes successful seed source");
    UiDsState ds;
    ds.depthEnable = ds.depthWriteAll = true;
    UiSeedKey k = uiSeedDrawKey(ds, 3, 1, 0, 123, 456, 4, 45);
    c.event(0, true, 10, k, true);
    c.finishDraw(true, false, false, true);
    c.event(0, true, 10, k, false);
    c.finishDraw(true, false, false, true);
    p = c.plan(0, true, 10, a, 9, 7, true, false, false, 0, true, false, true);
    check(p.reason == UiSeedStaleDraw && p.passes == 1,
          "stale draw reason retained after render sequence zero; depth-only one pass");
    check(c.keys[1].events == 2 && c.keys[1].invalidated == 1 && c.keys[1].afterStale == 1,
          "first invalidation and later intervening writers distinguishable");
    c.event(0, true, 10, uiSeedClearKey(2, .25f, 165, 0, 45, true), false);
    p = c.plan(0, true, 10, a, 9, 7, true, false, false, 0, true, false, true);
    check(p.reason == (UiSeedStaleDraw | UiSeedStaleClear), "joint stale episode retains draw and clear");
    c.seed(0, true, 10, a, 9, 7, p, true);
    // Accepted HDR rendering and its raw write-back do not call event: the
    // source stays fresh, just as production's raw owner bypass requires.
    p = c.plan(0, true, 10, a, 9, 7, false, false, false, 0, true, false, true);
    check(p.reason == 0, "accepted HDR raw write-back does not create an observed invalidation");
    p = c.plan(0, true, 11, b, 13, 5, true, true, true, 0xA5, true, false, true);
    check((p.reason & (UiSeedNewFrame | UiSeedSourceChanged | UiSeedSizeChanged)) ==
          (UiSeedNewFrame | UiSeedSourceChanged | UiSeedSizeChanged) && p.passes == 5,
          "source and size swaps and missing planes jointly recorded; bit passes exact");
    c.seed(0, true, 11, b, 13, 5, p, true);
    check(c.timelines[0].active && c.timelines[0].seq == 11, "later whole seeded frame capture starts");
    c.event(0, true, 11, k, true); c.finishDraw(false, true, false, false);
    c.door(0, 11);
    check(c.timelines[0].complete && c.timelines[0].n == 2 &&
          !c.timelines[0].events[1].key.outcomeKnown,
          "seed-to-door trace closes and uncertain substitution is explicit");
    UiSeedCensus boundary;
    boundary.configure(true);
    for (uint32_t eye = 0; eye < 2; ++eye) {
        boundary.seed(eye, true, 50, a, 9, 7, p, true);
        boundary.seed(eye, true, 51, a, 9, 7, p, true);
    }
    boundary.event(0, true, 51, k, true);
    // A seed within this draw must follow its decision-order invalidation,
    // although the original/substitution outcome is known only on exit.
    boundary.seed(0, true, 51, a, 9, 7, p, true);
    boundary.finishDraw(true, false, true, true);
    check(boundary.timelines[0].events[1].key.kind == 1 &&
          boundary.timelines[0].events[2].key.kind == 3 &&
          boundary.timelines[0].events[1].key.redirected,
          "same-draw invalidation precedes reseed; deferred outcome fills original event position");
    boundary.door(0, 51);
    boundary.event(0, true, 51, k, true); boundary.finishDraw(true, false, false, true);
    boundary.event(1, true, 51, k, true); boundary.finishDraw(true, false, false, true);
    check(boundary.timelines[0].n == 3 && boundary.timelines[1].n == 2 &&
          !boundary.timelines[1].complete, "one eye door cannot close the other or admit post-door events");
    boundary.seed(1, true, 52, b, 9, 7, p, true);
    boundary.door(1, 52);
    check(!boundary.timelines[1].complete && boundary.matches(1, true, b) &&
          !boundary.matches(1, true, a), "missing earlier eye door cannot become complete on next frame/source");
    boundary.configure(false); boundary.configure(true);
    check(!boundary.matches(0, true, a) && !boundary.timelines[1].active,
          "toggle ends incomplete capture and resets known sources");
    for (uint32_t i = 0; i < 30; ++i) {
        k.count = 100 + i; c.event(0, true, 11, k, false); c.finishDraw(true, false, false, true);
    }
    check(c.n == 16 && c.overflow > 0, "explicit normalized aggregate bounded at sixteen keys");
    k.count = 999; c.event(0, true, 11, k, true); c.finishDraw(true, false, false, true);
    check(c.overflowInvalidated == 1, "overflow explicitly counts omitted invalidators");
    UiDsState noOp;
    noOp.depthEnable = noOp.depthWriteAll = true;
    noOp.readOnlyDepth = true;
    check(uiSeedDrawKey(noOp, 4, 1, 0, 1, 2, 4, 45).planes == 0,
          "read-only depth suppresses writer classification even with ALL state");
    noOp.depthEnable = false; noOp.stencilEnable = true;
    noOp.front.pass = noOp.back.pass = 3; noOp.readOnlyStencil = true;
    check(uiSeedDrawKey(noOp, 4, 1, 0, 1, 2, 4, 45).planes == 0,
          "read-only stencil suppresses writer classification");
    noOp.readOnlyStencil = false; noOp.stencilPlane = false;
    check(uiSeedDrawKey(noOp, 4, 1, 0, 1, 2, 4, 45).planes == 0,
          "stencil-less DSV has no stencil writer plane");
    c.nextWindow();
    check(c.n == 0 && !c.timelines[0].seq && c.matches(0, true, b),
          "window clears output while retaining diagnostic seed source");
    c.configure(false); c.configure(true);
    check(!c.matches(0, true, b) && c.observedDisabled && c.observedEnabled,
          "off-on reset cannot imply unobserved intervening events were absent");
    p = c.plan(1, false, 20, a, 4, 4, true, false, false, 255, false, true, true);
    check(p.passes == 1, "specified stencil uses one draw for all requested bits");
    c.seed(1, false, 20, a, 4, 4, p, false);
    check(!c.matches(1, false, a), "failed seed never establishes a diagnostic source");
    c.seed(1, false, 20, a, 4, 4, p, true);
    c.seed(1, false, 21, a, 4, 4, p, true);
    for (uint32_t i = 0; i < 100; ++i) {
        c.event(1, false, 21, k, false); c.finishDraw(true, false, false, true);
    }
    c.door(1, 21);
    check(c.timelines[1].n == 64 && c.timelines[1].overflow == 37 && c.timelines[1].complete,
          "timeline cap reports omitted events without exceeding storage");
    unsigned lines = 0;
    c.report([&](const char*) { ++lines; });
    check(lines <= 147, "summary output has fixed line bound");
}

using CensusDrawFn = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT);
using CensusClearFn = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11DepthStencilView*, UINT, FLOAT, UINT8);
CensusDrawFn g_censusDraw = nullptr;
CensusClearFn g_censusClear = nullptr;
unsigned g_censusDraws = 0, g_censusClears = 0;
void STDMETHODCALLTYPE censusDraw(ID3D11DeviceContext* c, UINT n, UINT start) {
    ++g_censusDraws; g_censusDraw(c, n, start);
}
void STDMETHODCALLTYPE censusClear(ID3D11DeviceContext* c, ID3D11DepthStencilView* v,
                                  UINT f, FLOAT d, UINT8 s) {
    ++g_censusClears; g_censusClear(c, v, f, d, s);
}
struct CensusCommandSpy {
    ID3D11DeviceContext* ctx;
    void** original;
    std::array<void*, 115> table{};
    bool installed = false;
    explicit CensusCommandSpy(ID3D11DeviceContext* c) : ctx(c), original(*reinterpret_cast<void***>(c)) {
        std::copy(original, original + table.size(), table.begin());
        g_censusDraw = reinterpret_cast<CensusDrawFn>(original[13]);
        g_censusClear = reinterpret_cast<CensusClearFn>(original[53]);
        table[13] = reinterpret_cast<void*>(&censusDraw); table[53] = reinterpret_cast<void*>(&censusClear);
        DWORD old = 0;
        if (VirtualProtect(ctx, sizeof(void*), PAGE_READWRITE, &old)) {
            *reinterpret_cast<void***>(ctx) = table.data();
            DWORD ignored = 0; VirtualProtect(ctx, sizeof(void*), old, &ignored); installed = true;
        }
    }
    ~CensusCommandSpy() {
        if (!installed) return;
        DWORD old = 0;
        if (VirtualProtect(ctx, sizeof(void*), PAGE_READWRITE, &old)) {
            *reinterpret_cast<void***>(ctx) = original;
            DWORD ignored = 0; VirtualProtect(ctx, sizeof(void*), old, &ignored);
        }
    }
};

void testSeedCensusGpu(Gpu& g) {
    Ds src = makeDs(g, 8, 6, true), dst = makeDs(g, 8, 6, false);
    check(src.stencil && dst.dsv, "poisoned census depth-stencil fixture views");
    if (!src.stencil || !dst.dsv) return;
    edvr_layer_seed::Seeder seeder;
    try { seeder.init(g.dev.Get()); } catch (...) { check(false, "census production Seeder init"); return; }
    ComPtr<ID3D11DeviceContext> deferred;
    check(SUCCEEDED(g.dev->CreateDeferredContext(0, &deferred)), "census deferred seed context");
    if (!deferred) return;
    const D3D11_VIEWPORT vp{0, 0, 8, 6, 0, 1};
    const float rect[4] = {-1.2f, -1.2f, 1.2f, 1.2f}, white[4] = {1, 1, 1, 1};
    // Poison both planes, then check actual full-screen draw behavior against
    // the observer's conservative and reachable classifications.
    for (uint32_t scenario = 0; scenario < 15; ++scenario) {
        g.ctx->ClearDepthStencilView(src.dsv.Get(), 3, .75f, 165);
        const auto before = readBackDs(g, src);
        D3D11_DEPTH_STENCIL_DESC d{};
        d.DepthEnable = TRUE; d.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        d.DepthFunc = D3D11_COMPARISON_ALWAYS;
        d.StencilReadMask = d.StencilWriteMask = 255;
        d.FrontFace = {D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_COMPARISON_ALWAYS};
        d.BackFace = d.FrontFace;
        uint32_t count = 4, instances = 1, viewFlags = 0;
        if (scenario == 1) { d.DepthEnable = FALSE; d.StencilEnable = TRUE; d.FrontFace.StencilPassOp = D3D11_STENCIL_OP_REPLACE; d.BackFace = d.FrontFace; }
        if (scenario == 2) d.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        if (scenario == 3) d.DepthFunc = D3D11_COMPARISON_NEVER;
        if (scenario == 4) { d.DepthEnable = FALSE; d.StencilEnable = TRUE; d.FrontFace.StencilFailOp = D3D11_STENCIL_OP_REPLACE; d.BackFace = d.FrontFace; }
        if (scenario == 5) { d.StencilEnable = TRUE; d.FrontFace.StencilDepthFailOp = D3D11_STENCIL_OP_REPLACE; d.BackFace = d.FrontFace; d.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO; }
        if (scenario == 6) { viewFlags = D3D11_DSV_READ_ONLY_DEPTH; d.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO; }
        if (scenario == 7) count = 0;
        if (scenario == 8) instances = 0;
        if (scenario == 9) { d.DepthEnable = FALSE; d.StencilEnable = TRUE; d.StencilWriteMask = 0; d.FrontFace.StencilPassOp = D3D11_STENCIL_OP_REPLACE; d.BackFace = d.FrontFace; }
        if (scenario == 10) { d.DepthEnable = FALSE; d.StencilEnable = TRUE; viewFlags = D3D11_DSV_READ_ONLY_STENCIL; }
        if (scenario == 11) { d.DepthEnable = FALSE; d.StencilEnable = TRUE; }
        if (scenario == 12) { d.DepthEnable = FALSE; d.StencilEnable = TRUE; d.FrontFace.StencilFunc = D3D11_COMPARISON_NEVER; d.FrontFace.StencilPassOp = D3D11_STENCIL_OP_REPLACE; d.BackFace = d.FrontFace; }
        if (scenario == 13) { d.DepthEnable = FALSE; d.StencilEnable = TRUE; d.FrontFace.StencilDepthFailOp = D3D11_STENCIL_OP_REPLACE; d.BackFace = d.FrontFace; }
        if (scenario == 14) { d.StencilEnable = TRUE; d.FrontFace.StencilPassOp = D3D11_STENCIL_OP_REPLACE; d.BackFace = d.FrontFace; }
        ComPtr<ID3D11DepthStencilState> state;
        check(SUCCEEDED(g.dev->CreateDepthStencilState(&d, &state)), "census fixture DS state");
        D3D11_DEPTH_STENCIL_VIEW_DESC vd{};
        src.dsv->GetDesc(&vd); vd.Flags = viewFlags;
        ComPtr<ID3D11DepthStencilView> view;
        check(SUCCEEDED(g.dev->CreateDepthStencilView(src.tex.Get(), &vd, &view)), "census fixture DSV flags");
        UiDsState s = uiLayerDsStateFrom(&d, viewFlags);
        UiSeedKey k = uiSeedDrawKey(s, count, instances, 0, 1, 2, 4, uint32_t(vd.Format));
        check(k.ds[14] == uint8_t((viewFlags & 1) != 0) && k.ds[15] == uint8_t((viewFlags & 2) != 0), "DSV read-only metadata retained");
        if (count && instances) quadDs(g, nullptr, view.Get(), state.Get(), rect, white, 0, 0, vp, nullptr);
        else { // Set the same shader/bindings, then observe the actual zero-work call.
            quadDs(g, nullptr, view.Get(), state.Get(), rect, white, 0, 0, vp, nullptr);
            g.ctx->ClearDepthStencilView(src.dsv.Get(), 3, .75f, 165);
            g.ctx->OMSetRenderTargets(0, nullptr, view.Get());
            g.ctx->OMSetDepthStencilState(state.Get(), 4);
            g.ctx->DrawInstanced(count, instances, 0, 0);
        }
        g.ctx->OMSetRenderTargets(0, nullptr, nullptr);
        const auto after = readBackDs(g, src);
        bool depthChanged = false, stencilChanged = false;
        for (size_t i = 0; i < before.size(); i += 4) {
            depthChanged = depthChanged || std::memcmp(&before[i], &after[i], 3) != 0;
            stencilChanged = stencilChanged || before[i + 3] != after[i + 3];
        }
        check(k.reachable == ((depthChanged ? 1 : 0) | (stencilChanged ? 2 : 0)),
              "recorded reachable planes match exact observed depth/stencil changes in poisoned writer fixture");
        check((after != before) == (scenario < 2 || scenario == 14), "poisoned GPU plane changes only reachable nonzero writer");
        check(k.reachable == (scenario == 0 ? 1 : scenario == 1 ? 2 : scenario == 14 ? 3 : 0),
              "CPU reachable plane upper bound matches these known GPU cases");
        if (scenario == 3 || scenario == 4 || scenario == 7 || scenario == 8)
            check(k.planes != 0, "conservative no-op writer remains represented without changing freshness");
    }
    for (uint32_t flags : {0u, 1u, 2u, 3u}) {
        g.ctx->ClearDepthStencilView(src.dsv.Get(), 3, .75f, 165);
        const auto before = readBackDs(g, src);
        g.ctx->ClearDepthStencilView(src.dsv.Get(), flags, .25f, 60);
        const auto after = readBackDs(g, src);
        const auto k = uiSeedClearKey(flags, .25f, 60, 0, 45, true);
        bool depthChanged = false, stencilChanged = false;
        for (size_t i = 0; i < before.size(); i += 4) {
            depthChanged = depthChanged || std::memcmp(&before[i], &after[i], 3) != 0;
            stencilChanged = stencilChanged || before[i + 3] != after[i + 3];
        }
        check(k.planes == ((depthChanged ? 1 : 0) | (stencilChanged ? 2 : 0)), "clear flags exactly identify poisoned plane changes");
        check(k.value == 0x3E800000u && k.ref == 60, "clear depth bits and stencil value exact");
    }
    for (uint8_t mask : {uint8_t(0), uint8_t(4), uint8_t(165), uint8_t(255)}) {
        g.ctx->ClearDepthStencilView(src.dsv.Get(), 3, .25f, 165);
        g.ctx->ClearDepthStencilView(dst.dsv.Get(), 3, .875f, 90);
        UiSeedCensus c; c.configure(true);
        auto p = c.plan(0, true, 1, src.tex.Get(), 8, 6, true, true, true,
                        mask, true, seeder.usesSpecifiedStencilRef(), true);
        {
            CensusCommandSpy spy(deferred.Get());
            check(spy.installed, "production seed command observation installed");
            g_censusDraws = g_censusClears = 0;
            seeder.seed(deferred.Get(), src.depth.Get(), src.stencil.Get(), dst.dsv.Get(),
                        8, 6, 8, 6, 0, 0, mask, true);
            check(g_censusDraws == p.passes && g_censusClears == 1,
                  "reported seed pass count observes exact production Draw and stencil clear count");
        }
        ComPtr<ID3D11CommandList> list;
        check(SUCCEEDED(deferred->FinishCommandList(FALSE, &list)) && list, "production seed commands finish");
        if (!list) continue;
        g.ctx->ExecuteCommandList(list.Get(), TRUE);
        const auto got = readBackDs(g, dst), expected = readBackDs(g, src);
        const uint8_t want = seeder.usesSpecifiedStencilRef() ? 165 : uint8_t(165 & mask);
        for (size_t i = 0; i < got.size(); i += 4) {
            check(std::memcmp(&got[i], &expected[i], 3) == 0 && got[i + 3] == want,
                  "observed seed plans overwrite poisoned output with exact production depth and requested stencil");
        }
    }
}

// A private UI depth write at the layer's finer grid can differ from its raw
// game write-back resampled by a later seed. Prove whether the proposed
// stencil-only shortcut needs to retain the legacy refresh in this case.
void testPrivateDepthRefreshProof(Gpu& g) {
    Ds game = makeDs(g, 12, 8, true), legacy = makeDs(g, 15, 10, false), held = makeDs(g, 15, 10, false);
    Tex oldColour = makeTex(g, 15, 10, false), heldColour = makeTex(g, 15, 10, false);
    edvr_layer_seed::Seeder seeder;
    try { seeder.init(g.dev.Get()); } catch (...) { check(false, "private depth proof Seeder init"); return; }
    const float jx = .23f, jy = -.31f;
    auto seed = [&](Ds& dst) {
        g.ctx->OMSetRenderTargets(0, nullptr, nullptr);
        seeder.seed(g.ctx.Get(), game.depth.Get(), game.stencil.Get(), dst.dsv.Get(),
                    12, 8, 15, 10, jx, jy, 0, true);
    };
    g.ctx->ClearDepthStencilView(game.dsv.Get(), 3, .75f, 165);
    seed(legacy); seed(held);
    D3D11_DEPTH_STENCIL_DESC desc{};
    desc.DepthEnable = TRUE; desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    desc.DepthFunc = D3D11_COMPARISON_ALWAYS;
    ComPtr<ID3D11DepthStencilState> depthWriter;
    check(SUCCEEDED(g.dev->CreateDepthStencilState(&desc, &depthWriter)), "private depth proof writer state");
    const float rect[4] = {-.37f, -.5f, .51f, .43f}, full[4] = {-1.2f, -1.2f, 1.2f, 1.2f};
    const float white[4] = {1, 1, 1, 1}, black[4] = {0, 0, 0, 0};
    const D3D11_VIEWPORT gameVp{0, 0, 12, 8, 0, 1}, layerVp{0, 0, 15, 10, 0, 1};
    quadDs(g, nullptr, legacy.dsv.Get(), depthWriter.Get(), rect, white, 0, 0, layerVp, nullptr);
    quadDs(g, nullptr, held.dsv.Get(), depthWriter.Get(), rect, white, 0, 0, layerVp, nullptr);
    // The production raw write-back uses the game's original viewport/jitter.
    quadDs(g, nullptr, game.dsv.Get(), depthWriter.Get(), rect, white,
           2 * jx / 12, -2 * jy / 8, gameVp, nullptr);
    const auto gameBefore = readBackDs(g, game);
    const auto stencilWriter = stencilState(g, true);
    quadDs(g, nullptr, game.dsv.Get(), stencilWriter.Get(), full, white, 0, 0, gameVp, nullptr);
    const auto gameAfter = readBackDs(g, game);
    bool unchanged = true;
    for (size_t i = 0; i < gameBefore.size(); i += 4)
        unchanged = unchanged && std::memcmp(&gameBefore[i], &gameAfter[i], 3) == 0;
    check(unchanged, "actual game stencil-only write leaves sourced depth byte-exact after raw replay");
    seed(legacy);
    const auto a = readBackDs(g, legacy), b = readBackDs(g, held);
    bool differs = false;
    for (size_t i = 0; i < a.size(); i += 4) differs = differs || std::memcmp(&a[i], &b[i], 3) != 0;
    check(differs, "private finer depth and later game-depth resampling can differ despite stencil-only intervening writer");
    desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO; desc.DepthFunc = D3D11_COMPARISON_GREATER_EQUAL;
    ComPtr<ID3D11DepthStencilState> tester;
    check(SUCCEEDED(g.dev->CreateDepthStencilState(&desc, &tester)), "private depth proof consumer state");
    g.ctx->ClearRenderTargetView(oldColour.rtv.Get(), black);
    g.ctx->ClearRenderTargetView(heldColour.rtv.Get(), black);
    quadDs(g, oldColour.rtv.Get(), legacy.dsv.Get(), tester.Get(), full, white, 0, 0, layerVp, nullptr);
    quadDs(g, heldColour.rtv.Get(), held.dsv.Get(), tester.Get(), full, white, 0, 0, layerVp, nullptr);
    check(readBack(g, oldColour) != readBack(g, heldColour),
          "omitting refresh after accepted private depth write can change later consumer colour");
}
