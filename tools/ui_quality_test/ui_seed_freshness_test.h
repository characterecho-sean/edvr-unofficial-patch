// Included after the existing production Seeder/DS fixture helpers.
void testSeedWriterRule() {
    UiDsEffect stencil; stencil.stencilWrite = true;
    UiDsEffect depth; depth.depthWrite = true;
    UiDsEffect mixed = depth; mixed.stencilWrite = true;
    check(!uiLayerSeedWriterInvalidates(stencil, 0, true, false),
          "unmodified depth-only seed survives a stencil-only game writer");
    check(uiLayerSeedWriterInvalidates(stencil, 0, true, true),
          "any private depth interaction in frame retains legacy refresh");
    check(uiLayerSeedWriterInvalidates(stencil, 4, true, false) &&
          uiLayerSeedWriterInvalidates(stencil, 255, true, false),
          "requested bits and specified-reference full-mask seeds keep legacy refresh");
    check(uiLayerSeedWriterInvalidates(stencil, 0, false, false),
          "unseeded or failed depth seed cannot qualify for preservation");
    check(uiLayerSeedWriterInvalidates(depth, 0, true, false) &&
          uiLayerSeedWriterInvalidates(mixed, 0, true, false),
          "all depth and mixed writers retain legacy invalidation");
    check(!uiLayerSeedWriterInvalidates(UiDsEffect{}, 255, true, true),
          "read-only and KEEP no-writer effects remain no invalidation");
    // The write mask decides what a stencil-only writer can touch: D3D11 masks every stencil write by StencilWriteMask.
    check(UiDsEffect{}.stencilWriteMask == 0xFF, "an effect built by hand writes every stencil bit");
    D3D11_DEPTH_STENCIL_DESC bitDesc{};
    bitDesc.StencilEnable = TRUE; bitDesc.StencilReadMask = 0xFF; bitDesc.StencilWriteMask = 0x04;
    bitDesc.FrontFace = {D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_REPLACE,
                         D3D11_COMPARISON_ALWAYS};
    bitDesc.BackFace = bitDesc.FrontFace;
    const UiDsEffect fromDesc = uiLayerDsEffect(uiLayerDsStateFrom(&bitDesc, 0), true);
    check(fromDesc.stencilWrite && !fromDesc.depthWrite && fromDesc.stencilWriteMask == 0x04,
          "the effect of a depth-off REPLACE state carries its StencilWriteMask");
    UiDsEffect bit4 = stencil; bit4.stencilWriteMask = 0x04;
    UiDsEffect bit4Depth = bit4; bit4Depth.depthWrite = true;
    check(!uiLayerSeedWriterInvalidates(bit4, 0x02, true, false),
          "a stencil-only writer whose mask misses the seed's bits keeps a depth-and-bits seed");
    check(uiLayerSeedWriterInvalidates(bit4, 0x06, true, false),
          "the same writer against a seed holding a bit it writes invalidates");
    check(uiLayerSeedWriterInvalidates(bit4Depth, 0x02, true, false),
          "a mask-disjoint writer that also writes depth invalidates");
    check(uiLayerSeedWriterInvalidates(bit4, 0x02, true, true),
          "a mask-disjoint writer under the private depth guard retains legacy refresh");
    check(!uiLayerSeedWriterInvalidates(bit4, 0x02, false, false),
          "a mask-disjoint writer keeps a bits-only seed that holds no depth");
    check(uiLayerSeedWriterInvalidates(bit4, 0, false, false),
          "a seed holding neither depth nor bits is never kept");
    check(uiLayerSeedWriterInvalidates(bit4, 0xFF, true, false),
          "a full-mask seed (the specified-reference seed) overlaps every write mask");
    check(!uiLayerSeedWriterInvalidates(bit4, 0, true, false),
          "a depth-only seed is kept by a narrow-mask stencil writer too");
    UiLayerPrivateDepthGuard guard;
    check(guard.active(0) && !guard.active(71), "sequence zero/bootstrap remains conservative");
    guard.note(71, true);
    check(guard.active(71) && !guard.active(72), "private interaction guards whole current sequence only");
    guard.noteReplay(71, 72, true);
    check(guard.active(72), "late replay guards current sequence without advancing cached freshness");
    guard.noteReplay(74, 72, true);
    check(guard.active(72) && guard.active(74), "future captured replay conservatively guards older current caches");
    guard.note(71, true);
    check(guard.sequence == 74 && guard.active(73) && !guard.active(75),
          "older private write cannot regress watermark; next monotonically newer frame recovers");
    guard.note(75, false);
    check(!guard.active(75), "depth-disabled or write-zero draw cannot extend guard");
    guard.reset();
    check(!guard.active(72) && guard.active(0), "shutdown resets guard and keeps bootstrap conservative");
}

using CensusCopyFn = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, ID3D11Resource*);
CensusCopyFn g_freshCopy = nullptr;
unsigned g_freshCopies = 0;
void STDMETHODCALLTYPE freshnessCopy(ID3D11DeviceContext* ctx, ID3D11Resource* dst, ID3D11Resource* src) {
    ++g_freshCopies; g_freshCopy(ctx, dst, src);
}
struct FreshCopySpy {
    ID3D11DeviceContext* ctx;
    void** original;
    std::array<void*, 115> table{};
    bool installed = false;
    explicit FreshCopySpy(ID3D11DeviceContext* c) : ctx(c), original(*reinterpret_cast<void***>(c)) {
        std::copy(original, original + table.size(), table.begin());
        g_freshCopy = reinterpret_cast<CensusCopyFn>(original[47]);
        table[47] = reinterpret_cast<void*>(&freshnessCopy);
        DWORD old = 0;
        if (VirtualProtect(ctx, sizeof(void*), PAGE_READWRITE, &old)) {
            *reinterpret_cast<void***>(ctx) = table.data();
            DWORD ignored = 0; VirtualProtect(ctx, sizeof(void*), old, &ignored); installed = true;
        }
    }
    ~FreshCopySpy() {
        if (!installed) return;
        DWORD old = 0;
        if (VirtualProtect(ctx, sizeof(void*), PAGE_READWRITE, &old)) {
            *reinterpret_cast<void***>(ctx) = original;
            DWORD ignored = 0; VirtualProtect(ctx, sizeof(void*), old, &ignored);
        }
    }
};

struct FreshWorld {
    struct Cache {
        Ds layer;
        Tex colour;
        const void* source = nullptr;
        uint64_t seq = 0;
        uint32_t seededW = 0, seededH = 0;
        uint8_t mask = 0;
        bool depth = false;
    };
    Gpu& gpu;
    edvr_layer_seed::Seeder seeder;
    ComPtr<ID3D11DeviceContext> deferred;
    std::array<Ds, 2> games, copies;
    std::array<Cache, 4> caches; // two layers for each eye
    uint64_t seq = 71;
    UiLayerPrivateDepthGuard privateDepthGuard;
    unsigned seeds = 0, seedAttempts = 0, seedCopies = 0, seedDraws = 0, seedClears = 0;
    unsigned gameWriterCommands = 0, preserved = 0;
    float jx = .23f, jy = -.31f;
    bool optimized = false, failNextSeed = false;

    FreshWorld(Gpu& g, bool opt) : gpu(g), optimized(opt) {
        seeder.init(g.dev.Get());
        check(SUCCEEDED(g.dev->CreateDeferredContext(0, &deferred)), "freshness replay deferred context");
        for (unsigned i = 0; i < 2; ++i) {
            games[i] = makeDs(g, 12, 8, true); copies[i] = makeDs(g, 12, 8, true);
            check(games[i].dsv && copies[i].stencil, "freshness real game/source-copy resources");
            g.ctx->ClearDepthStencilView(games[i].dsv.Get(), 3, .75f, 0);
            D3D11_DEPTH_STENCIL_DESC desc{};
            desc.DepthEnable = TRUE; desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
            desc.DepthFunc = D3D11_COMPARISON_ALWAYS;
            auto state = dsState(desc);
            const float rect[4] = {-.8f + float(i) * .15f, -.8f, .8f, .8f};
            draw(games[i].dsv.Get(), nullptr, state.Get(), rect, 12, 8, 0, 0);
        }
        for (Cache& c : caches) resize(c, 15, 10);
    }
    ComPtr<ID3D11DepthStencilState> dsState(const D3D11_DEPTH_STENCIL_DESC& d) {
        ComPtr<ID3D11DepthStencilState> s;
        check(SUCCEEDED(gpu.dev->CreateDepthStencilState(&d, &s)), "freshness fixture depth/stencil state");
        return s;
    }
    void resize(Cache& c, uint32_t w, uint32_t h) {
        gpu.ctx->OMSetRenderTargets(0, nullptr, nullptr);
        c.layer = makeDs(gpu, w, h, false); c.colour = makeTex(gpu, w, h, false);
        check(c.layer.dsv && c.colour.rtv, "freshness resized private depth and colour targets");
        gpu.ctx->ClearDepthStencilView(c.layer.dsv.Get(), 3, .875f, 90);
        // Production target recreation sets render sequence zero. Retain old
        // descriptive masks only until the next successful seed, as it does.
        c.seq = 0;
    }
    void draw(ID3D11DepthStencilView* dst, ID3D11RenderTargetView* colour,
              ID3D11DepthStencilState* state, const float rect[4], uint32_t w, uint32_t h,
              float ndcX, float ndcY) {
        const float white[4] = {1, 1, 1, 1};
        const D3D11_VIEWPORT vp{0, 0, float(w), float(h), 0, 1};
        quadDs(gpu, colour, dst, state, rect, white, ndcX, ndcY, vp, nullptr);
        gpu.ctx->OMSetRenderTargets(0, nullptr, nullptr);
    }
    bool seed(unsigned cache, unsigned game, uint8_t mask, bool depth) {
        ++seedAttempts;
        if (failNextSeed) { failNextSeed = false; return false; }
        Cache& c = caches[cache];
        {
            FreshCopySpy spy(gpu.ctx.Get()); check(spy.installed, "real CopyResource observation installed");
            g_freshCopies = 0;
            gpu.ctx->CopyResource(copies[game].tex.Get(), games[game].tex.Get());
            check(g_freshCopies == 1, "each production-shaped seed submits exactly one full source copy");
            seedCopies += g_freshCopies;
        }
        {
            CensusCommandSpy spy(deferred.Get()); check(spy.installed, "real seed Draw/clear observation installed");
            g_censusDraws = g_censusClears = 0;
            seeder.seed(deferred.Get(), copies[game].depth.Get(), copies[game].stencil.Get(), c.layer.dsv.Get(),
                        12, 8, c.layer.w, c.layer.h, jx, jy, mask, depth);
            seedDraws += g_censusDraws; seedClears += g_censusClears;
        }
        ComPtr<ID3D11CommandList> list;
        check(SUCCEEDED(deferred->FinishCommandList(FALSE, &list)) && list, "real seed replay command list finished");
        if (!list) return false;
        gpu.ctx->ExecuteCommandList(list.Get(), TRUE);
        const bool full = seeder.usesSpecifiedStencilRef() && (depth || mask);
        c.seq = seq; c.source = games[game].tex.Get();
        c.mask = full ? 255 : mask; c.depth = depth || full;
        c.seededW = c.layer.w; c.seededH = c.layer.h; ++seeds;
        return true;
    }
    bool consume(unsigned cache, unsigned game, uint8_t requestedMask = 0) {
        Cache& c = caches[cache];
        const bool current = c.seq == seq && c.source == games[game].tex.Get() &&
            c.seededW == c.layer.w && c.seededH == c.layer.h;
        const bool shortBits = (requestedMask & ~c.mask) != 0;
        if (!current || shortBits || !c.depth) {
            const uint8_t want = uint8_t(requestedMask | (current ? c.mask : 0));
            if (!seed(cache, game, want, true)) return false;
        }
        D3D11_DEPTH_STENCIL_DESC d{};
        d.DepthEnable = TRUE; d.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        d.DepthFunc = D3D11_COMPARISON_GREATER_EQUAL;
        if (requestedMask) {
            d.StencilEnable = TRUE; d.StencilReadMask = requestedMask; d.StencilWriteMask = 0;
            d.FrontFace = {D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP,
                           D3D11_COMPARISON_EQUAL}; d.BackFace = d.FrontFace;
        }
        auto state = dsState(d);
        const float black[4] = {0, 0, 0, 0}, full[4] = {-1.2f, -1.2f, 1.2f, 1.2f};
        gpu.ctx->ClearRenderTargetView(c.colour.rtv.Get(), black);
        draw(c.layer.dsv.Get(), c.colour.rtv.Get(), state.Get(), full, c.layer.w, c.layer.h, 0, 0);
        return true;
    }
    void gameWriter(unsigned game, const D3D11_DEPTH_STENCIL_DESC& d, const float rect[4],
                    uint32_t viewFlags = 0, bool zeroWork = false) {
        auto state = dsState(d);
        const UiDsState description = uiLayerDsStateFrom(&d, viewFlags);
        const UiDsEffect effect = uiLayerDsEffect(description, true);
        for (Cache& c : caches) {
            if (c.seq != seq || c.source != games[game].tex.Get()) continue;
            const bool dirty = optimized ? uiLayerSeedWriterInvalidates(effect, c.mask, c.depth,
                privateDepthGuard.active(seq)) : effect.writes();
            if (dirty) c.seq = 0; else if (effect.writes()) ++preserved;
        }
        D3D11_DEPTH_STENCIL_VIEW_DESC vd{}; games[game].dsv->GetDesc(&vd); vd.Flags = viewFlags;
        ComPtr<ID3D11DepthStencilView> view;
        check(SUCCEEDED(gpu.dev->CreateDepthStencilView(games[game].tex.Get(), &vd, &view)), "game writer view flags");
        CensusCommandSpy spy(gpu.ctx.Get()); check(spy.installed, "real game writer command observation installed");
        g_censusDraws = 0;
        if (!zeroWork) draw(view.Get(), nullptr, state.Get(), rect, 12, 8, 0, 0);
        else {
            // Install the same pipeline first on a target which cannot alter
            // this source, then submit the actual zero-vertex source command.
            draw(nullptr, nullptr, state.Get(), rect, 12, 8, 0, 0);
            g_censusDraws = 0;
            gpu.ctx->OMSetRenderTargets(0, nullptr, view.Get());
            gpu.ctx->OMSetDepthStencilState(state.Get(), 4);
            gpu.ctx->Draw(0, 0);
            gpu.ctx->OMSetRenderTargets(0, nullptr, nullptr);
        }
        check(g_censusDraws == 1, "source writer always forwards its original command exactly once");
        gameWriterCommands += g_censusDraws;
    }
    void clear(unsigned game, UINT flags) {
        // All clear invalidation deliberately remains the existing rule.
        for (Cache& c : caches) if (c.seq && c.source == games[game].tex.Get()) c.seq = 0;
        gpu.ctx->ClearDepthStencilView(games[game].dsv.Get(), flags, .25f, 60);
    }
    void privateWriter(unsigned cache, unsigned game, bool depth, bool readOnlyOriginal = false,
                       uint64_t capturedSequence = 0) {
        Cache& c = caches[cache];
        D3D11_DEPTH_STENCIL_DESC d{};
        d.DepthEnable = TRUE; d.DepthFunc = depth ? D3D11_COMPARISON_ALWAYS : D3D11_COMPARISON_GREATER_EQUAL;
        d.DepthWriteMask = depth ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
        if (!depth || readOnlyOriginal) {
            d.StencilEnable = TRUE; d.StencilReadMask = 0; d.StencilWriteMask = 4;
            d.FrontFace = {D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_REPLACE,
                           D3D11_COMPARISON_ALWAYS}; d.BackFace = d.FrontFace;
        }
        auto state = dsState(d);
        const float rect[4] = {-.97f, -.97f, .51f, .43f};
        // Exactly the production guard: mark before a potential private/raw
        // depth write, and do not reset it after another cache's fresh seed.
        const UiDsState raw = uiLayerDsStateFrom(&d, readOnlyOriginal ? D3D11_DSV_READ_ONLY_DEPTH : 0);
        const bool potential = raw.depthEnable && raw.depthWriteAll;
        privateDepthGuard.note(seq, potential);
        draw(c.layer.dsv.Get(), nullptr, state.Get(), rect, c.layer.w, c.layer.h, 0, 0);
        // Raw replay deliberately bypasses the game-writer freshness hook.
        D3D11_DEPTH_STENCIL_VIEW_DESC vd{}; games[game].dsv->GetDesc(&vd);
        vd.Flags = readOnlyOriginal ? D3D11_DSV_READ_ONLY_DEPTH : 0;
        ComPtr<ID3D11DepthStencilView> originalView;
        check(SUCCEEDED(gpu.dev->CreateDepthStencilView(games[game].tex.Get(), &vd, &originalView)),
              "raw replay retains original read-only view flags");
        privateDepthGuard.noteReplay(capturedSequence ? capturedSequence : seq, seq, potential);
        draw(originalView.Get(), nullptr, state.Get(), rect, 12, 8,
             2 * jx / 12, -2 * jy / 8);
    }
};

D3D11_DEPTH_STENCIL_DESC freshnessStencilWriter() {
    D3D11_DEPTH_STENCIL_DESC d{};
    d.DepthEnable = TRUE; d.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    d.DepthFunc = D3D11_COMPARISON_GREATER_EQUAL;
    d.StencilEnable = TRUE; d.StencilReadMask = 0; d.StencilWriteMask = 4;
    d.FrontFace = {D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP,
                   D3D11_STENCIL_OP_REPLACE, D3D11_COMPARISON_ALWAYS}; d.BackFace = d.FrontFace;
    return d;
}
void compareFreshWorlds(FreshWorld& old, FreshWorld& opt, unsigned cache, unsigned game, uint8_t validMask = 0) {
    check(readBack(old.gpu, old.caches[cache].colour) == readBack(opt.gpu, opt.caches[cache].colour),
          "legacy and optimized consumer colour is byte-exact");
    const auto a = readBackDs(old.gpu, old.caches[cache].layer), b = readBackDs(opt.gpu, opt.caches[cache].layer);
    bool equal = a.size() == b.size();
    for (size_t i = 0; equal && i < a.size(); i += 4)
        equal = std::memcmp(&a[i], &b[i], 3) == 0 && ((a[i + 3] ^ b[i + 3]) & validMask) == 0;
    check(equal, "legacy and optimized consumer depth and all claimed stencil bits are byte-exact");
    check(readBackDs(old.gpu, old.games[game]) == readBackDs(opt.gpu, opt.games[game]),
          "all original game depth/stencil mutation bytes are unchanged");
}

void testReadOnlyPrivateDepthProof(Gpu& g) {
    FreshWorld legacy(g, false), maskedMarker(g, true), guarded(g, true);
    check(legacy.consume(1, 0) && maskedMarker.consume(1, 0) && guarded.consume(1, 0),
          "read-only-view private writer proof primes copied depth");
    const auto gameBefore = readBackDs(g, guarded.games[0]);
    const auto privateBefore = readBackDs(g, guarded.caches[1].layer);
    legacy.privateWriter(1, 0, true, true);
    maskedMarker.privateWriter(1, 0, true, true);
    guarded.privateWriter(1, 0, true, true);
    const auto gameAfter = readBackDs(g, guarded.games[0]);
    const auto privateAfter = readBackDs(g, guarded.caches[1].layer);
    bool gameDepthEqual = true, privateDepthChanged = false;
    for (size_t p = 0; p < gameBefore.size(); p += 4)
        gameDepthEqual = gameDepthEqual && std::memcmp(&gameBefore[p], &gameAfter[p], 3) == 0;
    for (size_t p = 0; p < privateBefore.size(); p += 4)
        privateDepthChanged = privateDepthChanged || std::memcmp(&privateBefore[p], &privateAfter[p], 3) != 0;
    check(gameDepthEqual && privateDepthChanged,
          "actual original read-only view preserves game depth while writable private binding changes depth");
    auto state = freshnessStencilWriter();
    state.DepthFunc = D3D11_COMPARISON_ALWAYS; state.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    const auto raw = uiLayerDsStateFrom(&state, D3D11_DSV_READ_ONLY_DEPTH);
    const auto effect = uiLayerDsEffect(raw, true);
    check(!effect.tests() && !effect.depthWrite && effect.stencilWrite && raw.depthEnable && raw.depthWriteAll,
          "admitted no-tests/read-only state masks original depth effect but retains private raw depth potential");
    // Emulate the rejected marker that used original effect.depthWrite only.
    maskedMarker.privateDepthGuard.reset();
    const float rect[4] = {-.7f, -.6f, -.1f, .6f};
    const auto writer = freshnessStencilWriter();
    legacy.gameWriter(0, writer, rect); maskedMarker.gameWriter(0, writer, rect); guarded.gameWriter(0, writer, rect);
    check(legacy.consume(1, 0) && maskedMarker.consume(1, 0) && guarded.consume(1, 0), "read-only proof later depth consumers complete");
    check(readBack(g, legacy.caches[1].colour) != readBack(g, maskedMarker.caches[1].colour),
          "marker masked by original read-only flag causes actual later colour regression");
    compareFreshWorlds(legacy, guarded, 1, 0);
}

// A depth-off stencil writer under a given write mask (ALWAYS / REPLACE; the harness's reference is 4, so REPLACE writes
// 4 & mask: a mask of 0x04 sets bit 0x04, a mask of 0x02 clears bit 0x02). depthWrite adds a depth write (quad z = 0).
D3D11_DEPTH_STENCIL_DESC disjointStencilWriter(uint8_t writeMask, bool depthWrite = false) {
    D3D11_DEPTH_STENCIL_DESC d{};
    d.DepthEnable = depthWrite ? TRUE : FALSE;
    d.DepthWriteMask = depthWrite ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
    d.DepthFunc = D3D11_COMPARISON_ALWAYS;
    d.StencilEnable = TRUE; d.StencilReadMask = 0xFF; d.StencilWriteMask = writeMask;
    d.FrontFace = {D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_REPLACE,
                   D3D11_COMPARISON_ALWAYS}; d.BackFace = d.FrontFace;
    return d;
}
// The game's stencil before any seed: bit 0x02 set everywhere except a window (left of x = 0.5, the whole height) where a
// stencil-only game draw cleared it. A layer test on bit 0x02 is lit only inside the window, and only where the game's
// depth is 0 (the constructor's quad, columns 1..10 and rows 1..6 of the 12 x 8 buffer). No seed exists yet, so the draw
// invalidates nothing.
void disjointPrime(FreshWorld& w, unsigned game) {
    w.gpu.ctx->ClearDepthStencilView(w.games[game].dsv.Get(), D3D11_CLEAR_STENCIL, 0.0f, 0x02);
    const float window[4] = {-.99f, -.99f, .5f, .99f};
    w.gameWriter(game, disjointStencilWriter(0x02), window);
}
unsigned litPixels(const std::vector<uint8_t>& rgba) {
    unsigned n = 0;
    for (size_t i = 3; i < rgba.size(); i += 4) n += rgba[i] != 0;
    return n;
}

// A stencil-only game writer whose StencilWriteMask misses every bit a seed holds leaves the layer's copy what a fresh
// seed would copy, so the optimized world skips the re-seed and still draws exactly what the legacy world (every write
// re-seeds) draws. A writer that overlaps the held bits, writes depth, or meets the private-depth guard does not: the
// optimized world re-seeds like legacy, and a world that is never told of the write (the stale seed) draws something else.
void testBitDisjointSeedReplay(Gpu& g) {
    const float lateral[4] = {-.1f, -.99f, .99f, .99f};   // right half of the buffer, beyond the clear window's edge
    const float everywhere[4] = {-1.1f, -1.1f, 1.1f, 1.1f};
    {
        FreshWorld old(g, false), opt(g, true);
        // A seed that holds every stencil bit (the specified-reference seed, AMD/Intel) overlaps any write mask.
        const bool fullSeed = opt.seeder.usesSpecifiedStencilRef();
        disjointPrime(old, 0); disjointPrime(opt, 0);
        check(old.consume(0, 0, 0x02) && opt.consume(0, 0, 0x02), "disjoint writer: bit 0x02 seeds complete");
        compareFreshWorlds(old, opt, 0, 0, 0x02);
        const auto lit0 = readBack(g, opt.caches[0].colour);
        check(litPixels(lit0) > 0 && litPixels(lit0) < unsigned(lit0.size() / 4),
              "the bit 0x02 test is lit in the clear window only (the rig discriminates)");
        const auto gameBefore = readBackDs(g, opt.games[0]);
        old.gameWriter(0, disjointStencilWriter(0x04), lateral);
        opt.gameWriter(0, disjointStencilWriter(0x04), lateral);
        const auto gameAfter = readBackDs(g, opt.games[0]);
        bool depthEqual = true, stencilChanged = false, bit2Equal = true;
        for (size_t p = 0; p < gameBefore.size(); p += 4) {
            depthEqual = depthEqual && std::memcmp(&gameBefore[p], &gameAfter[p], 3) == 0;
            stencilChanged = stencilChanged || gameBefore[p + 3] != gameAfter[p + 3];
            bit2Equal = bit2Equal && ((gameBefore[p + 3] ^ gameAfter[p + 3]) & 0x02) == 0;
        }
        check(depthEqual && stencilChanged && bit2Equal,
              "the actual stencil 0x04 writer changes stencil bit 0x04 only and leaves every depth byte");
        check(old.consume(0, 0, 0x02) && opt.consume(0, 0, 0x02), "bit 0x02 consumers after the 0x04 writer complete");
        compareFreshWorlds(old, opt, 0, 0, 0x02);
        if (!fullSeed) {
            check(old.seeds == 2 && opt.seeds == 1 && old.preserved == 0 && opt.preserved == 1,
                  "the 0x04 writer re-seeds the legacy world and leaves the optimized seed in place");
            check(old.seedCopies == 2 && opt.seedCopies == 1 && opt.seedDraws * 2 == old.seedDraws &&
                  opt.seedClears * 2 == old.seedClears,
                  "the skipped re-seed is one fewer full copy, draw set and clear");
        } else {
            check(opt.seeds == old.seeds && opt.preserved == 0,
                  "a full-mask seed overlaps the 0x04 writer: the optimized world re-seeds like legacy");
        }
        // Growth to the written bit: the seed is short of 0x04 in both worlds, so both copy the game's current bits.
        check(old.consume(0, 0, 0x04) && opt.consume(0, 0, 0x04), "bit 0x04 consumers after the writer complete");
        compareFreshWorlds(old, opt, 0, 0, 0x06);
        const auto lit4 = readBack(g, opt.caches[0].colour);
        check(litPixels(lit4) > 0, "the bit 0x04 test sees the writer's new bit");
        check(readBack(g, old.caches[0].colour) == lit4, "bit 0x04 consumer colour matches the legacy world's");
        if (!fullSeed)
            check(old.seeds == 3 && opt.seeds == 2, "the growth re-seed picks up the writer's bit in both worlds");
        else
            check(opt.seeds == old.seeds, "full-mask seeds grow no further");
    }
    {
        // A writer that overlaps the held bit (mask 0x06 clears bit 0x02 on the right) is seen by the layer's next test.
        FreshWorld old(g, false), opt(g, true), unseen(g, true);
        for (FreshWorld* w : {&old, &opt, &unseen}) {
            disjointPrime(*w, 0);
            check(w->consume(0, 0, 0x02), "overlap: bit 0x02 seed completes");
        }
        old.gameWriter(0, disjointStencilWriter(0x06), lateral);
        opt.gameWriter(0, disjointStencilWriter(0x06), lateral);
        // The same draw with the freshness bookkeeping bypassed: the stale seed an overlapping rule would leave.
        unseen.draw(unseen.games[0].dsv.Get(), nullptr, unseen.dsState(disjointStencilWriter(0x06)).Get(), lateral,
                    12, 8, 0, 0);
        check(old.consume(0, 0, 0x02) && opt.consume(0, 0, 0x02) && unseen.consume(0, 0, 0x02),
              "overlap: consumers after the 0x06 writer complete");
        compareFreshWorlds(old, opt, 0, 0, 0x02);
        check(opt.seeds == old.seeds && opt.preserved == 0 && opt.seeds == 2,
              "an overlapping writer re-seeds the optimized world exactly as legacy");
        check(unseen.seeds == 1 && readBack(g, unseen.caches[0].colour) != readBack(g, old.caches[0].colour),
              "the stale seed an unseen overlapping writer leaves draws a different image (the overlap is real)");
    }
    {
        // A mask-disjoint writer that also writes depth: the copied depth is stale where it wrote.
        FreshWorld old(g, false), opt(g, true), unseen(g, true);
        for (FreshWorld* w : {&old, &opt, &unseen}) {
            disjointPrime(*w, 0);
            check(w->consume(0, 0, 0x02), "depth writer: bit 0x02 seed completes");
        }
        const auto depthWriter = disjointStencilWriter(0x04, true);
        old.gameWriter(0, depthWriter, everywhere);
        opt.gameWriter(0, depthWriter, everywhere);
        unseen.draw(unseen.games[0].dsv.Get(), nullptr, unseen.dsState(depthWriter).Get(), everywhere, 12, 8, 0, 0);
        check(old.consume(0, 0, 0x02) && opt.consume(0, 0, 0x02) && unseen.consume(0, 0, 0x02),
              "depth writer: consumers after the writer complete");
        compareFreshWorlds(old, opt, 0, 0, 0x02);
        check(opt.seeds == old.seeds && opt.preserved == 0 && opt.seeds == 2,
              "a mask-disjoint writer that writes depth re-seeds the optimized world as legacy");
        check(unseen.seeds == 1 && readBack(g, unseen.caches[0].colour) != readBack(g, old.caches[0].colour),
              "the stale depth an unseen depth writer leaves draws a different image (the depth write is real)");
    }
    {
        // A private depth write earlier in the frame: the guard keeps the legacy refresh across both layers.
        FreshWorld old(g, false), opt(g, true);
        disjointPrime(old, 0); disjointPrime(opt, 0);
        for (unsigned c = 0; c < 2; ++c)
            check(old.consume(c, 0, 0x02) && opt.consume(c, 0, 0x02), "guard: bit 0x02 seeds complete");
        old.privateWriter(1, 0, true); opt.privateWriter(1, 0, true);
        old.gameWriter(0, disjointStencilWriter(0x04), lateral);
        opt.gameWriter(0, disjointStencilWriter(0x04), lateral);
        for (unsigned c = 0; c < 2; ++c) {
            check(old.consume(c, 0, 0x02) && opt.consume(c, 0, 0x02), "guard: consumers after the writer complete");
            compareFreshWorlds(old, opt, c, 0, 0x02);
        }
        check(opt.seeds == old.seeds && opt.preserved == 0,
              "the private depth guard retains the legacy refresh for a mask-disjoint writer");
    }
}

void testSeedFreshnessGpu(Gpu& g) {
    testReadOnlyPrivateDepthProof(g);
    testBitDisjointSeedReplay(g);
    const float writes[3][4] = {{-.7f, -.6f, -.1f, .6f}, {-.1f, -.6f, .4f, .6f}, {.4f, -.6f, .7f, .6f}};
    const auto observed = freshnessStencilWriter();
    FreshWorld old(g, false), opt(g, true);
    check(old.consume(0, 0) && opt.consume(0, 0), "poisoned baseline depth-only seeds complete");
    compareFreshWorlds(old, opt, 0, 0);
    for (unsigned i = 0; i < 3; ++i) {
        const auto before = readBackDs(g, opt.games[0]);
        old.gameWriter(0, observed, writes[i]); opt.gameWriter(0, observed, writes[i]);
        const auto after = readBackDs(g, opt.games[0]);
        bool depthEqual = true, stencilChanged = false;
        for (size_t p = 0; p < before.size(); p += 4) {
            depthEqual = depthEqual && std::memcmp(&before[p], &after[p], 3) == 0;
            stencilChanged = stencilChanged || before[p + 3] != after[p + 3];
        }
        check(depthEqual && stencilChanged, "each actual observed GEQUAL stencil writer changes stencil and preserves sourced depth exactly");
        check(old.consume(0, 0) && opt.consume(0, 0), "interleaved depth consumers complete");
        compareFreshWorlds(old, opt, 0, 0);
    }
    const unsigned wanted = opt.seeder.usesSpecifiedStencilRef() ? 4 : 1;
    check(old.seeds == 4 && opt.seeds == wanted && old.seedCopies == 4 && opt.seedCopies == wanted &&
          old.seedDraws == 4 && opt.seedDraws == wanted && old.seedClears == 4 && opt.seedClears == wanted,
          "observed three-writer route removes exactly three copies/draws/clears only on depth-only fallback");
    check(old.gameWriterCommands == 3 && opt.gameWriterCommands == 3,
          "optimization changes no original source draw command count");
    // Growth reads bit4: a fresh current-source copy must precede its test.
    check(old.consume(0, 0, 4) && opt.consume(0, 0, 4), "later stencil-read growth re-seeds");
    compareFreshWorlds(old, opt, 0, 0, 4);
    const unsigned prior = opt.seeds;
    opt.gameWriter(0, observed, writes[0]); old.gameWriter(0, observed, writes[0]);
    check(old.consume(0, 0, 4) && opt.consume(0, 0, 4) && opt.seeds == prior + 1,
          "nonzero claimed stencil mask retains full legacy invalidation");
    compareFreshWorlds(old, opt, 0, 0, 4);

    for (unsigned scenario = 0; scenario < 14; ++scenario) {
        FreshWorld baseline(g, false), guarded(g, true);
        // Both eyes and both layers share or independently own source data.
        for (unsigned c = 0; c < 4; ++c)
            check(baseline.consume(c, c / 2) && guarded.consume(c, c / 2), "multi-layer/eye initial seeds complete");
        auto d = observed;
        uint32_t viewFlags = 0; bool zeroWork = false;
        if (scenario == 0) d.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL; // mixed
        if (scenario == 1) { d.StencilEnable = FALSE; d.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL; } // depth
        if (scenario == 2) { d.FrontFace.StencilPassOp = D3D11_STENCIL_OP_KEEP; d.BackFace = d.FrontFace; } // KEEP
        if (scenario == 3) { d.FrontFace.StencilPassOp = D3D11_STENCIL_OP_KEEP; d.BackFace = d.FrontFace; viewFlags = D3D11_DSV_READ_ONLY_STENCIL; }
        if (scenario == 4) zeroWork = true;
        if (scenario == 5 || scenario == 6) {
            // 5: own finer-grid private depth; 6: another layer's source gets
            // raw depth replay, while this cache itself had no private write.
            baseline.privateWriter(scenario == 5 ? 0 : 1, 0, true);
            guarded.privateWriter(scenario == 5 ? 0 : 1, 0, true);
        }
        if (scenario == 7) { // private stencil writes do not poison copied depth
            baseline.privateWriter(0, 0, false); guarded.privateWriter(0, 0, false);
        }
        if (scenario == 8) { // prior private depth frame must not disable future frames
            baseline.privateWriter(1, 0, true); guarded.privateWriter(1, 0, true);
            ++baseline.seq; ++guarded.seq;
            for (unsigned c = 0; c < 4; ++c)
                check(baseline.consume(c, c / 2) && guarded.consume(c, c / 2), "new native sequence refreshes all caches");
        }
        if (scenario == 9) { // source swap and size change after an old seed
            baseline.resize(baseline.caches[0], 18, 12); guarded.resize(guarded.caches[0], 18, 12);
            check(baseline.consume(0, 1) && guarded.consume(0, 1), "new source identity and target size force fresh seed");
            compareFreshWorlds(baseline, guarded, 0, 1);
        }
        if (scenario == 10) { // failed refresh establishes no fresh sequence
            ++baseline.seq; ++guarded.seq; baseline.failNextSeed = guarded.failNextSeed = true;
            check(!baseline.consume(0, 0) && !guarded.consume(0, 0), "failed seed keeps original unavailable outcome");
        }
        if (scenario >= 11) {
            const uint64_t capturedOld = scenario == 12 ? baseline.seq - 1 : scenario == 13 ? baseline.seq + 1 : 0;
            const uint64_t capturedNew = scenario == 12 ? guarded.seq - 1 : scenario == 13 ? guarded.seq + 1 : 0;
            baseline.privateWriter(1, 0, true, true, capturedOld);
            guarded.privateWriter(1, 0, true, true, capturedNew);
        }
        const unsigned start = guarded.seeds;
        baseline.gameWriter(0, d, writes[0], viewFlags, zeroWork);
        guarded.gameWriter(0, d, writes[0], viewFlags, zeroWork);
        // A private depth write on eye0 also keeps eye1's legacy behavior,
        // even though its independent source has not changed depth.
        baseline.gameWriter(1, d, writes[1], viewFlags, zeroWork);
        guarded.gameWriter(1, d, writes[1], viewFlags, zeroWork);
        for (unsigned c = 0; c < 4; ++c) {
            const unsigned source = scenario == 9 && c == 0 ? 1 : c / 2;
            check(baseline.consume(c, source) && guarded.consume(c, source), "guarded multi-cache consumers complete");
            compareFreshWorlds(baseline, guarded, c, source);
        }
        if (scenario == 5 || scenario == 6 || scenario >= 11)
            check(guarded.seeds == start + 4 && guarded.seeds == baseline.seeds,
                  "one private depth interaction retains legacy refresh across both layers and both eyes");
        check(baseline.gameWriterCommands == guarded.gameWriterCommands,
              "all fallback cases preserve game source mutation command counts");
        if (scenario == 7) {
            check(baseline.consume(0, 0, 4) && guarded.consume(0, 0, 4), "growth after private stencil writes refreshes current game bits");
            compareFreshWorlds(baseline, guarded, 0, 0, 4);
        }
    }
    for (UINT flags : {0u, 1u, 2u, 3u}) {
        FreshWorld baseline(g, false), guarded(g, true);
        check(baseline.consume(0, 0) && guarded.consume(0, 0), "clear-case seeds complete");
        baseline.clear(0, flags); guarded.clear(0, flags);
        check(baseline.consume(0, 0) && guarded.consume(0, 0), "all clear flags retain legacy refresh including zero flags");
        compareFreshWorlds(baseline, guarded, 0, 0);
        check(baseline.seeds == 2 && guarded.seeds == 2, "no clear shortcut introduced");
    }
}
