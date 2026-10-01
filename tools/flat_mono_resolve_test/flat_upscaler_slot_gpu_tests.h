// The third upscaler slot (docs/design-flat-temporal-aa-2026-09-23.md section 82; dlaa.h, kUpscalerSlots): the VR world route calls
// the resolver from the VR draw path, where the two eyes already own upscaler slots 0 and 1, so it passes FlatMonoResolveFrame::slot
// = 2 and both engines keep one feature (one size key, one history) per slot. What is pinned here is the resolver's half, on the
// rig's stub backends, which record the slot they were handed: every slot 0..2 reaches each backend (NVIDIA's two modes, FSR) as
// itself, on the copy route and on the HDR route the world route uses; the default is slot 0, which is what the flat profile has
// always passed; a slot no engine has (3 and beyond) refuses the frame by name BEFORE anything is made or written; and a refusal
// does not keep a later valid frame from resolving. The engines' own halves are pinned where their engines can run: fsr3_engine_test
// (real contexts on WARP: each slot's context and history is its own) and dlaa_mode_test (the slot rules, and the source scan that
// holds dlaa.cpp's arrays to them, since NGX needs an NVIDIA GPU).
#pragma once

inline void upscalerSlotGpuTests(ID3D11Device* device, ID3D11DeviceContext* context) {
    using edvr::FlatMonoResolveFrame;
    using edvr::FlatMonoResolveMode;
    const UINT w = ResolveFixture::w, h = ResolveFixture::h;
    ResolveFixture fx(device, context);
    std::vector<uint32_t> red(w * h, 0xff0000ff);
    std::vector<float> z(w * h, .01f);
    auto color = texture(device, w, h, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE, red.data(), w * 4);
    auto depth = texture(device, w, h, DXGI_FORMAT_R32_FLOAT, D3D11_BIND_SHADER_RESOURCE, z.data(), w * 4);
    auto colorView = view(device, color.Get()), depthView = view(device, depth.Get());

    FlatMonoResolveFrame f{};
    f.color = colorView.Get(); f.depth = depthView.Get();
    f.renderWidth = w; f.renderHeight = h; f.outputWidth = w; f.outputHeight = h;
    f.deltaMs = 16; camera(f.camera); camera(f.previousCamera); fx.engine(f);
    f.mode = FlatMonoResolveMode::Dlss; f.frame = 9000; f.reset = true;
    expectedJx = expectedJy = 0;
    check(f.slot == 0, "upscaler slots: a frame that never names a slot is slot 0, the flat profile's and eye 0's");
    check(edvr::kUpscalerSlots == 3 && edvr::kUpscalerEyeSlots == 2,
          "upscaler slots: two eyes and the VR world route's third");

    const char* reasonOut = nullptr;
    auto run = [&](const FlatMonoResolveFrame& frame, bool wanted) {
        fx.bindOriginal();
        ComPtr<ID3D11ShaderResourceView> out; reasonOut = nullptr;
        const bool ok = edvr::flatMonoResolve(device, context, frame, out.GetAddressOf(), &reasonOut);
        if (ok != wanted) std::printf("info: upscaler-slot resolver reason %s\n", reasonOut ? reasonOut : "none");
        check(ok == wanted, "upscaler slots: resolver result");
        check(fx.restored(), "upscaler slots: the game's whole pipeline restored on every exit");
        check(ok ? (frame.hdr ? out == nullptr : out != nullptr) : out == nullptr,
              "upscaler slots: an output view exactly when the copy route resolved");
        return ok;
    };

    // ---- 1. every slot reaches every backend as itself ---------------------------------------------------------------
    edvr::flatMonoResolveReset();
    static const struct { FlatMonoResolveMode mode; const char* name; } backends[] = {
        {FlatMonoResolveMode::Dlss, "DLSS"}, {FlatMonoResolveMode::Dlaa, "DLAA"}, {FlatMonoResolveMode::Fsr, "FSR"}};
    for (const auto& b : backends) {
        for (uint32_t slot = 0; slot < edvr::kUpscalerSlots; ++slot) {
            FlatMonoResolveFrame frame = f; frame.mode = b.mode; frame.slot = slot; frame.frame = ++f.frame; frame.reset = false;
            const size_t calls = backendSlots.size();
            const bool ok = run(frame, true);
            char what[160];
            std::snprintf(what, sizeof(what), "upscaler slots: a %s frame on slot %u reaches the backend as slot %u, once", b.name, slot, slot);
            check(ok && backendSlots.size() == calls + 1 && backendSlots.back() == int(slot), what);
        }
    }
    // The default is slot 0: a frame built before the field existed passes what every caller always passed.
    {
        FlatMonoResolveFrame frame = f; frame.frame = ++f.frame; frame.reset = false;
        check(run(frame, true) && backendSlots.back() == 0, "upscaler slots: an untouched frame reaches the backend as slot 0");
    }
    // The slots interleave: the resolver hands each call its own, in order (the engines keep one history per slot).
    {
        static const uint32_t order[] = {2, 0, 2, 1, 2, 2, 0};
        const size_t first = backendSlots.size();
        bool all = true;
        for (uint32_t slot : order) {
            FlatMonoResolveFrame frame = f; frame.slot = slot; frame.frame = ++f.frame; frame.reset = false;
            all = run(frame, true) && all;
        }
        bool inOrder = all && backendSlots.size() == first + sizeof(order) / sizeof(order[0]);
        for (size_t i = 0; inOrder && i < sizeof(order) / sizeof(order[0]); ++i) inOrder = backendSlots[first + i] == int(order[i]);
        check(inOrder, "upscaler slots: interleaved calls each reach the backend with their own slot, in call order");
    }

    // ---- 2. a slot no engine has refuses the frame, by name, before anything is made or written ------------------------
    static const uint32_t bad[] = {edvr::kUpscalerSlots, edvr::kUpscalerSlots + 1, 7, 0x80000000u, 0xffffffffu};
    for (uint32_t slot : bad) {
        edvr::flatMonoResolveReset();   // nothing made, so "nothing made" is a count that can stay put
        const auto before = edvr::flatMonoResolveStats();
        const int calls = backendCalls; const size_t slots = backendSlots.size();
        FlatMonoResolveFrame frame = f; frame.slot = slot; frame.frame = ++f.frame; frame.reset = true;
        const bool ok = run(frame, false);
        const auto after = edvr::flatMonoResolveStats();
        char what[160];
        std::snprintf(what, sizeof(what), "upscaler slots: slot %u refuses the frame, naming flat-resolve-invalid-slot", slot);
        check(!ok && reasonOut && !std::strcmp(reasonOut, "flat-resolve-invalid-slot"), what);
        std::snprintf(what, sizeof(what), "upscaler slots: slot %u refuses before any backend call, renderer initialisation or allocation", slot);
        check(backendCalls == calls && backendSlots.size() == slots && after.initializations == before.initializations &&
                  after.allocations == before.allocations, what);
    }
    // The refusal leaves nothing behind that keeps a valid frame from resolving, on any slot.
    for (uint32_t slot = 0; slot < edvr::kUpscalerSlots; ++slot) {
        FlatMonoResolveFrame frame = f; frame.slot = slot; frame.frame = ++f.frame; frame.reset = false;
        check(run(frame, true) && backendSlots.back() == int(slot), "upscaler slots: a valid frame after a refusal resolves on its own slot");
    }

    // ---- 3. the HDR route the world route uses: slot 2 reaches the backend, and a refused slot leaves the game's target bit for bit ---
    {
        auto hTexture = texture(device, w, h, DXGI_FORMAT_R11G11B10_FLOAT, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET);
        auto hSrv = view(device, hTexture.Get());
        std::vector<uint32_t> texels(w * h);
        for (size_t i = 0; i < texels.size(); ++i) texels[i] = 0x2a8a5000u + uint32_t(i) * 0x00010441u;   // any bits: what matters is they stay
        context->UpdateSubresource(hTexture.Get(), 0, nullptr, texels.data(), w * 4, 0);
        auto hHash = [&] {
            std::vector<unsigned char> bytes;
            return readWhole(context, hTexture.Get(), bytes, 4) ? fnv1a(bytes.data(), bytes.size()) : uint64_t(0);
        };
        FlatMonoResolveFrame hdr = f; hdr.color = hSrv.Get(); hdr.hdr = true; hdr.slot = edvr::kUpscalerSlots - 1; hdr.reset = true;
        hdr.frame = ++f.frame; observedHdr = false;
        edvr::flatMonoResolveReset();
        check(run(hdr, true) && observedHdr && backendSlots.back() == int(edvr::kUpscalerSlots - 1),
              "upscaler slots: an HDR-route frame on the VR world's slot reaches the backend as slot 2, told it is HDR");
        context->UpdateSubresource(hTexture.Get(), 0, nullptr, texels.data(), w * 4, 0);
        const uint64_t untouched = hHash();
        FlatMonoResolveFrame refused = hdr; refused.slot = edvr::kUpscalerSlots; refused.frame = ++f.frame;
        const int calls = backendCalls;
        const bool refusedOk = run(refused, false);
        check(!refusedOk && reasonOut && !std::strcmp(reasonOut, "flat-resolve-invalid-slot") &&
                  backendCalls == calls && hHash() == untouched,
              "upscaler slots: an HDR-route frame on a slot no engine has refuses and leaves the game's HDR target bit for bit what it was");
    }
    edvr::flatMonoResolveReset();
}
