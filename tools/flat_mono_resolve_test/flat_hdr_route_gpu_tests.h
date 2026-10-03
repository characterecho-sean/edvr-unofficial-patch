// The HDR route's resolver half on WARP (docs/design-flat-temporal-aa-2026-09-23.md, section 81): the game's R11G11B10F
// scene target goes in, the resolve goes back INTO it through a pixel shader, and nothing else of the game's state moves.
// The backends are the rig's stubs (they inspect their real GPU inputs, the flag they were handed and the formats of the
// textures they name); the shaders, the private input copy, the fp16 outputs and the requantisation are the shipped ones.
//
// What is pinned: the backend is told "HDR" and handed an R11G11B10F input and an fp16 output; a reset frame comes back
// as the jittered image resampled on the unjittered grid, requantised to within one ulp of the format (half an ulp
// where a device rounds to nearest; WARP truncates toward zero, which costs up to one, and the rig prints which); a frame the
// backend resolves comes back as the backend's output where the history is trusted and as the input where it is not;
// EDVR's TAA accumulates in c/(1+max3(c)) space (a hot pixel is tamed where linear blending would carry it) and never
// overshoots the input range or makes a NaN; every refusal leaves the game's texture bit for bit what it was; the game's
// pipeline state is restored on every exit; and the LDR route after an HDR frame is what it was.
#pragma once

namespace hdrgpu {
// R11G11B10_FLOAT by hand: no sign, 5 exponent bits (bias 15), 6 mantissa bits for red and green and 5 for blue.
inline double top(int mant) { return (2.0 - std::ldexp(1.0, -mant)) * 32768.0; }
inline uint32_t encode(double v, int mant) {
    const uint32_t full = (1u << mant);
    if (!(v > 0)) return 0;
    if (v >= top(mant)) return (30u << mant) | (full - 1);
    int e = 0; std::frexp(v, &e);
    int exponent = e - 1;
    if (exponent < -14) {   // denormal: mant * 2^(-14 - mant)
        const uint32_t q = uint32_t(std::floor(v / std::ldexp(1.0, -14 - mant) + 0.5));
        return q >= full ? full : q;
    }
    uint32_t q = uint32_t(std::floor((v / std::ldexp(1.0, exponent) - 1.0) * full + 0.5));
    if (q == full) { q = 0; ++exponent; }
    if (exponent > 15) return (30u << mant) | (full - 1);
    return (uint32_t(exponent + 15) << mant) | q;
}
inline double decode(uint32_t bits, int mant) {
    const uint32_t full = (1u << mant);
    const uint32_t exponent = bits >> mant, q = bits & (full - 1);
    if (exponent == 0) return std::ldexp(double(q), -14 - mant);
    return std::ldexp(1.0 + double(q) / full, int(exponent) - 15);
}
inline uint32_t pack(double r, double g, double b) { return encode(r, 6) | (encode(g, 6) << 11) | (encode(b, 5) << 22); }
inline void unpack(uint32_t p, double (&out)[3]) { out[0] = decode(p & 0x7FF, 6); out[1] = decode((p >> 11) & 0x7FF, 6); out[2] = decode(p >> 22, 5); }
// The gap to the next value the format holds above v.
inline double ulp(double v, int mant) {
    int e = 0; std::frexp(v > 0 ? v : 1e-9, &e);
    int exponent = e - 1; if (exponent < -14) exponent = -14;
    return std::ldexp(1.0, exponent - mant);
}
inline int mantissa(int channel) { return channel == 2 ? 5 : 6; }
// A texel as three doubles, after one trip through the format.
inline void representable(double r, double g, double b, double (&out)[3]) { unpack(pack(r, g, b), out); }
}  // namespace hdrgpu

inline void hdrRouteGpuTests(ID3D11Device* device, ID3D11DeviceContext* context) {
    using namespace hdrgpu;
    using edvr::FlatMonoResolveFrame;
    using edvr::FlatMonoResolveMode;
    const UINT w = 16, h = 16;

    // The engine inputs, as the copy route's fixture builds them: no engine slot anywhere (the camera term moves pixels),
    // a pool with one empty record, scene constants for a still camera.
    std::vector<float> z(w * h, .01f), slots(w * h * 2);
    for (size_t i = 0; i < slots.size(); i += 2) { slots[i] = -1; slots[i + 1] = .01f; }
    auto depth = texture(device, w, h, DXGI_FORMAT_R32_FLOAT, D3D11_BIND_SHADER_RESOURCE, z.data(), w * 4);
    auto slotTexture = texture(device, w, h, DXGI_FORMAT_R32G32_FLOAT, D3D11_BIND_SHADER_RESOURCE, slots.data(), w * 8);
    auto depthView = view(device, depth.Get()), slotView = view(device, slotTexture.Get());
    uint32_t record[84]{};
    D3D11_BUFFER_DESC bd{}; bd.ByteWidth = sizeof(record); bd.StructureByteStride = 336;
    bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_SHADER_RESOURCE; bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    D3D11_SUBRESOURCE_DATA initial{}; initial.pSysMem = record;
    ComPtr<ID3D11Buffer> pool;
    check(SUCCEEDED(device->CreateBuffer(&bd, &initial, pool.GetAddressOf())), "HDR route: pool buffer");
    auto poolView = view(device, pool.Get());
    constexpr uint32_t kStamp = 77;
    float scene[277][4]{}; float cam[6][4]; camera(cam); std::memcpy(scene + 270, cam, sizeof(cam));
    { const uint32_t stamp = kStamp; std::memcpy(&scene[276][0], &stamp, 4); }
    bd = {}; bd.ByteWidth = sizeof(scene); bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    initial.pSysMem = scene;
    ComPtr<ID3D11Buffer> now, old;
    check(SUCCEEDED(device->CreateBuffer(&bd, &initial, now.GetAddressOf())) &&
          SUCCEEDED(device->CreateBuffer(&bd, &initial, old.GetAddressOf())), "HDR route: engine camera buffers");

    // The game's pipeline around the call: a render target, a pixel and a vertex shader resource, a compute constant
    // buffer and a viewport, all of which the resolver must hand back untouched.
    // (The shader resource is over ANOTHER texture: one resource bound as both would have the runtime unbind one of them.)
    auto other = texture(device, 32, 32, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_RENDER_TARGET);
    auto sampled = texture(device, 32, 32, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE);
    ComPtr<ID3D11RenderTargetView> otherRtv; ComPtr<ID3D11ShaderResourceView> otherSrv = view(device, sampled.Get());
    check(SUCCEEDED(device->CreateRenderTargetView(other.Get(), nullptr, otherRtv.GetAddressOf())), "HDR route: original RTV");
    D3D11_VIEWPORT viewport{3, 4, 19, 21, .2f, .8f};
    auto bindOriginal = [&] {
        ID3D11RenderTargetView* rt = otherRtv.Get(); context->OMSetRenderTargets(1, &rt, nullptr);
        ID3D11ShaderResourceView* srv = otherSrv.Get(); context->PSSetShaderResources(0, 1, &srv); context->VSSetShaderResources(3, 1, &srv);
        ID3D11Buffer* cb = old.Get(); context->CSSetConstantBuffers(4, 1, &cb); context->RSSetViewports(1, &viewport);
    };
    auto restored = [&] {
        ComPtr<ID3D11RenderTargetView> rt; ComPtr<ID3D11ShaderResourceView> ps, vs; ComPtr<ID3D11Buffer> cb;
        context->OMGetRenderTargets(1, rt.GetAddressOf(), nullptr); context->PSGetShaderResources(0, 1, ps.GetAddressOf());
        context->VSGetShaderResources(3, 1, vs.GetAddressOf()); context->CSGetConstantBuffers(4, 1, cb.GetAddressOf());
        UINT count = 1; D3D11_VIEWPORT current{}; context->RSGetViewports(&count, &current);
        return rt.Get() == otherRtv.Get() && ps.Get() == otherSrv.Get() && vs.Get() == otherSrv.Get() && cb.Get() == old.Get() &&
               count == 1 && !std::memcmp(&current, &viewport, sizeof(viewport));
    };

    // The game's HDR scene target: R11G11B10F, shader and render-target bound, at the render size.
    auto makeH = [&](UINT binds = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET) {
        return texture(device, w, h, DXGI_FORMAT_R11G11B10_FLOAT, binds);
    };
    auto hTexture = makeH();
    auto hSrv = view(device, hTexture.Get());
    // One frame of input: what the game rendered, already at representable values.
    std::vector<uint32_t> texels(w * h);
    std::vector<double> inR(w * h), inG(w * h), inB(w * h);
    auto fill = [&](auto colorOf) {
        for (UINT y = 0; y < h; ++y)
            for (UINT x = 0; x < w; ++x) {
                double c[3]; colorOf(x, y, c);
                double r[3]; representable(c[0], c[1], c[2], r);
                inR[y * w + x] = r[0]; inG[y * w + x] = r[1]; inB[y * w + x] = r[2];
                texels[y * w + x] = pack(r[0], r[1], r[2]);
            }
        context->UpdateSubresource(hTexture.Get(), 0, nullptr, texels.data(), w * 4, 0);
    };
    auto readH = [&](std::vector<uint32_t>& out) {
        std::vector<unsigned char> bytes;
        if (!readWhole(context, hTexture.Get(), bytes, 4)) return false;
        out.resize(w * h);
        std::memcpy(out.data(), bytes.data(), bytes.size());
        return true;
    };
    auto hHash = [&] {
        std::vector<unsigned char> bytes;
        return readWhole(context, hTexture.Get(), bytes, 4) ? fnv1a(bytes.data(), bytes.size()) : uint64_t(0);
    };
    // The jittered input resampled on the unjittered grid, in doubles: what a reset frame must give back.
    auto bilinear = [&](const std::vector<double>& channel, double fx, double fy) {
        const double tx = std::min<double>(std::max<double>(fx, 0.0), w - 1.0), ty = std::min<double>(std::max<double>(fy, 0.0), h - 1.0);
        const int x0 = int(std::floor(tx)), y0 = int(std::floor(ty));
        const int x1 = std::min<int>(x0 + 1, w - 1), y1 = std::min<int>(y0 + 1, h - 1);
        const double ax = tx - x0, ay = ty - y0;
        return (channel[y0 * w + x0] * (1 - ax) + channel[y0 * w + x1] * ax) * (1 - ay) +
               (channel[y1 * w + x0] * (1 - ax) + channel[y1 * w + x1] * ax) * ay;
    };
    // Every texel of H against `expected(x, y, channel)`, to within one unit in the format's last place plus the filter's
    // own slack (one ulp is what either rounding mode of the conversion to R11G11B10F can cost: nearest costs half, truncation
    // nearly one, and what a device does is its own); the worst miss, in ulps, is returned, with how many texels fell short
    // of the expected value (truncation) and how many went past it (rounding up).
    auto worstMiss = [&](auto expected, double slack, int* badTexels, int* below = nullptr, int* above = nullptr) {
        std::vector<uint32_t> got;
        double worst = 0; int bad = 0, lo = 0, hi = 0;
        if (!readH(got)) { if (badTexels) *badTexels = -1; return 1e9; }
        for (UINT y = 0; y < h; ++y)
            for (UINT x = 0; x < w; ++x) {
                double g[3]; unpack(got[y * w + x], g);
                for (int c = 0; c < 3; ++c) {
                    const double want = expected(x, y, c), u = ulp(want, mantissa(c));
                    const double miss = std::abs(g[c] - want) / u;
                    if (std::abs(g[c] - want) > 1.0 * u + slack) ++bad;
                    if (g[c] < want - 1e-9 * u) ++lo; else if (g[c] > want + 1e-9 * u) ++hi;
                    worst = std::max(worst, miss);
                }
            }
        if (badTexels) *badTexels = bad;
        if (below) *below = lo;
        if (above) *above = hi;
        return worst;
    };

    FlatMonoResolveFrame f{};
    f.color = hSrv.Get(); f.depth = depthView.Get(); f.renderWidth = w; f.renderHeight = h; f.outputWidth = w; f.outputHeight = h;
    f.deltaMs = 16; camera(f.camera); camera(f.previousCamera);
    f.engine = {slotView.Get(), poolView.Get(), now.Get(), old.Get()};
    f.mode = FlatMonoResolveMode::Dlss; f.frame = 100; f.hdr = true;
    auto run = [&](bool wanted, const char** why = nullptr) {
        bindOriginal();
        ComPtr<ID3D11ShaderResourceView> out; const char* reason = nullptr;
        const bool ok = edvr::flatMonoResolve(device, context, f, out.GetAddressOf(), &reason);
        if (ok != wanted) std::printf("info: HDR resolver reason %s\n", reason ? reason : "none");
        check(ok == wanted, "HDR route: resolver result");
        check(restored(), "HDR route: the game's whole pipeline restored on every exit");
        check(out == nullptr, "HDR route: no output view is returned (the result is in the game's target)");
        if (why) *why = reason;
        return ok;
    };

    // ---- 1. a reset frame: the jittered input resampled on the unjittered grid, requantised --------------------------
    // A 2-D ramp in all three channels, HDR-ranged: the bilinear resample is exact for it, so the only error is the
    // format's conversion (half an ulp rounding to nearest, up to one truncating) and the filter's fixed-point weights.
    fill([&](UINT x, UINT y, double (&c)[3]) { c[0] = 100 + 50.0 * x; c[1] = 10.0 * (y + 1); c[2] = 40.0 + 30.0 * x + 20.0 * y; });
    f.jitterX = f.previousJitterX = expectedJx = .25f; f.jitterY = f.previousJitterY = expectedJy = -.375f; f.reset = true;
    const auto before = edvr::flatMonoResolveStats();
    backendCalls = 0; observedHdr = false;
    bool ok = run(true);
    check(ok && backendCalls == 1 && observedHdr, "HDR route: the backend is called once and told it is HDR");
    check(observedColourFormat == DXGI_FORMAT_R11G11B10_FLOAT && observedOutFormat == DXGI_FORMAT_R16G16B16A16_FLOAT,
          "HDR route: the backend is handed an R11G11B10F input and an R16G16B16A16F output");
    int bad = 0, below = 0, above = 0;
    double worst = worstMiss([&](UINT x, UINT y, int c) {
        const std::vector<double>& ch = c == 0 ? inR : c == 1 ? inG : inB;
        return bilinear(ch, x + f.jitterX, y + f.jitterY);
    }, 0.5, &bad, &below, &above);
    check(bad == 0, "HDR route: a reset frame is the input resampled at the jitter, to within an ulp of the format and the filter's slack");
    std::printf("flat mono resolve: HDR reset frame: worst miss %.3f ulp of R11G11B10F over %u texels x 3 channels; %d below the exact "
                "resample, %d above (this device %s)\n", worst, w * h, below, above,
                worst <= 0.5001 ? "rounds to nearest" : (above == 0 ? "truncates toward zero" : "rounds both ways"));
    check(edvr::flatMonoResolveStats().hdrResolves == before.hdrResolves + 1, "HDR route: the resolve is counted");

    // ---- 2. a frame the backend resolves: its output where history is trusted, the input where it is not -------------
    fill([&](UINT x, UINT y, double (&c)[3]) { c[0] = 100 + 50.0 * x; c[1] = 10.0 * (y + 1); c[2] = 40.0 + 30.0 * x + 20.0 * y; });
    f.reset = false; ++f.frame; f.camera[5][0] = .3125f;   // a translation: one render pixel of motion, current to previous
    f.jitterX = f.previousJitterX = expectedJx = 0; f.jitterY = f.previousJitterY = expectedJy = 0;
    ok = run(true);
    check(ok && !backendReset && std::abs(observedMotion - 1) < .001, "HDR route: a continuing frame reaches the backend with the camera's motion");
    {
        std::vector<uint32_t> got; readH(got);
        double mid[3], edge[3];
        unpack(got[8 * w + 8], mid); unpack(got[8 * w + 15], edge);
        check(std::abs(mid[0]) < 1e-6 && std::abs(mid[1] - 1) < 1e-3 && std::abs(mid[2]) < 1e-6,
              "HDR route: where history is trusted the backend's output (green, fp16) is what goes into the game's target");
        check(std::abs(edge[0] - inR[8 * w + 15]) <= .55 * ulp(inR[8 * w + 15], 6) &&
              std::abs(edge[1] - inG[8 * w + 15]) <= .55 * ulp(inG[8 * w + 15], 6),
              "HDR route: where the reprojection left the frame the input goes through, unchanged");
    }
    f.camera[5][0] = 0;

    // ---- 3. FSR receives the flag too ------------------------------------------------------------------------------
    fill([&](UINT x, UINT, double (&c)[3]) { c[0] = 64 + 8.0 * x; c[1] = 32; c[2] = 16; });
    f.mode = FlatMonoResolveMode::Fsr; f.reset = true; ++f.frame; observedHdr = false; infiniteSeen = false;
    ok = run(true);
    check(ok && observedHdr && infiniteSeen && observedColourFormat == DXGI_FORMAT_R11G11B10_FLOAT && observedOutFormat == DXGI_FORMAT_R16G16B16A16_FLOAT,
          "HDR route: FSR is told HDR with an R11G11B10F input and an fp16 output, and keeps its explicit infinite depth");
    f.mode = FlatMonoResolveMode::Dlaa; f.reset = true; ++f.frame; observedHdr = false;
    ok = run(true);
    check(ok && observedHdr, "HDR route: DLAA is told HDR too (R = D)");

    // The backend sees only the clean world image while the final HDR target
    // takes the live game colour at covered source texels. The shader's four
    // bilinear source taps apply coverage at a nonzero phase, for SDK and TAA.
    {
        const uint32_t cleanPixel=pack(20,30,40);
        std::vector<uint32_t> cleanTexels(w*h,cleanPixel);
        auto cleanTexture=texture(device,w,h,DXGI_FORMAT_R11G11B10_FLOAT,
            D3D11_BIND_SHADER_RESOURCE,cleanTexels.data(),w*4);
        auto cleanView=view(device,cleanTexture.Get());
        std::vector<unsigned char> coverage(w*h,0);
        coverage[8*w+8]=255;
        auto coverageTexture=texture(device,w,h,DXGI_FORMAT_R8_UNORM,
            D3D11_BIND_SHADER_RESOURCE,coverage.data(),w);
        auto coverageView=view(device,coverageTexture.Get());
        check(cleanView && coverageView,"HDR overlay: clean and private coverage views create");
        if(cleanView && coverageView) {
            f.cleanColor=cleanView.Get();f.overlayCoverage=coverageView.Get();
            f.jitterX=f.previousJitterX=expectedJx=.25f;
            f.jitterY=f.previousJitterY=expectedJy=-.25f;
            std::memcpy(f.previousCamera,f.camera,sizeof(f.camera));
            auto live=[&] { fill([&](UINT x,UINT y,double (&c)[3]) {
                c[0]=20;c[1]=30;c[2]=40;
                if(x==8 && y==8) {c[0]=200;c[1]=10;c[2]=10;}
            }); };
            auto composited=[&](const char* route) {
                std::vector<uint32_t> got;
                if(!readH(got)) {check(false,route);return;}
                double marked[3]{},outside[3]{};
                unpack(got[8*w+8],marked);unpack(got[2*w+2],outside);
                bool fourTaps=true;
                for(UINT y=8;y<=9;++y)for(UINT x=7;x<=8;++x) {
                    double pixel[3]{};unpack(got[y*w+x],pixel);
                    const double raw=bilinear(inR,x+f.jitterX,y+f.jitterY);
                    fourTaps&=std::abs(pixel[0]-raw)<=1.5*ulp(raw,6) && pixel[0]>20;
                }
                check(fourTaps && marked[0]>100,
                      "HDR overlay: all four nonzero-phase bilinear neighbors use aligned live colour");
                if(f.mode==FlatMonoResolveMode::Taa)
                    check(std::abs(outside[0]-20)<=ulp(20,6) && std::abs(outside[1]-30)<=ulp(30,6),
                          "HDR overlay: TAA world outside coverage came from clean input");
                else
                    check(std::abs(outside[0])<1e-6 && std::abs(outside[1]-1)<.01,
                          "HDR overlay: SDK world outside coverage came from backend result");
            };
            for(auto mode:{FlatMonoResolveMode::Dlaa,FlatMonoResolveMode::Fsr}) {
                f.mode=mode;f.reset=false;++f.frame;live();run(true); // prime a new SDK mode's history
                ++f.frame;live();observedBackendPixels=false;
                backendCalls=0;
                ok=run(true);
                check(ok && !backendReset && backendCalls==1 && observedBackendPixels &&
                      observedBackendCenter==cleanPixel && observedBackendOutside==cleanPixel,
                      "HDR overlay: both SDKs receive clean world pixels, never live overlay pixels");
                composited("HDR overlay SDK readback");
            }
            std::vector<unsigned char> none(w*h,0);
            auto noCoverageTexture=texture(device,w,h,DXGI_FORMAT_R8_UNORM,
                D3D11_BIND_SHADER_RESOURCE,none.data(),w);
            auto noCoverageView=view(device,noCoverageTexture.Get());
            check(noCoverageView!=nullptr,"HDR overlay: zero coverage control view creates");
            if(noCoverageView) {
                f.mode=FlatMonoResolveMode::Taa;f.reset=true;++f.frame;live();
                f.overlayCoverage=noCoverageView.Get();
                ok=run(true);
                std::vector<uint32_t> zeroMaskOutput;readH(zeroMaskOutput);
                double zeroCenter[3]{};
                if(zeroMaskOutput.size()==w*h)unpack(zeroMaskOutput[8*w+8],zeroCenter);
                check(ok && zeroMaskOutput.size()==w*h &&
                      std::abs(zeroCenter[0]-20)<=ulp(20,6),
                      "HDR overlay: TAA receives clean HDR, proven by zero-mask live-hot-pixel control");
            }
            f.overlayCoverage=coverageView.Get();
            f.mode=FlatMonoResolveMode::Taa;f.reset=true;++f.frame;live();
            ok=run(true);
            check(ok,"HDR overlay: internal TAA accepts clean/live split");
            composited("HDR overlay TAA readback");

            // The next unmarked frame does not inherit a stale coverage mask.
            f.cleanColor=nullptr;f.overlayCoverage=nullptr;
            f.mode=FlatMonoResolveMode::Dlaa;f.reset=false;++f.frame;
            fill([&](UINT,UINT,double (&c)[3]) {c[0]=20;c[1]=30;c[2]=40;});
            run(true); // the previous TAA mode forces a reset of the SDK slot
            ++f.frame;
            fill([&](UINT,UINT,double (&c)[3]) {c[0]=20;c[1]=30;c[2]=40;});
            ok=run(true);
            std::vector<uint32_t> next;readH(next);
            double center[3]{};if(next.size()==w*h)unpack(next[8*w+8],center);
            check(ok && !backendReset && next.size()==w*h && center[0]<1e-6 && std::abs(center[1]-1)<.01,
                  "HDR overlay: next unmarked frame uses backend world result at former overlay pixel");

            // Supplying only half of the clean/raw contract must fail before
            // the backend, preserving the actual game H bytes.
            f.cleanColor=cleanView.Get();f.overlayCoverage=nullptr;++f.frame;live();
            const auto liveHash=hHash();const int calls=backendCalls;
            ok=run(false);
            check(!ok && backendCalls==calls && hHash()==liveHash,
                  "HDR overlay: missing coverage refuses without modifying game H or calling backend");
            f.cleanColor=nullptr;f.overlayCoverage=nullptr;
        }
    }

    // ---- 4. EDVR's TAA: bounded-space accumulation -----------------------------------------------------------------
    // A still camera and a still scene, jitter zero: a hot pixel appears on the second frame. In c/(1+max3(c)) space the
    // history (100) and the new value (10000) are a hair apart, the 3x3 box holds the history, and the blend at .9
    // leaves the hot pixel near 111; linear blending would leave it near 1090.
    f.mode = FlatMonoResolveMode::Taa; f.jitterX = f.jitterY = f.previousJitterX = f.previousJitterY = 0; expectedJx = expectedJy = 0;
    fill([&](UINT, UINT, double (&c)[3]) { c[0] = c[1] = c[2] = 100; });
    f.reset = true; ++f.frame; ok = run(true);
    {
        const int badStill = [&] { int b = 0; worstMiss([&](UINT, UINT, int) { return 100.0; }, 0.0, &b); return b; }();
        check(ok && badStill == 0, "HDR TAA: a reset frame is the input through the format (a flat 100)");
    }
    fill([&](UINT x, UINT y, double (&c)[3]) { c[0] = c[1] = c[2] = (x == 8 && y == 8) ? 10000 : 100; });
    f.reset = false; ++f.frame; ok = run(true);
    {
        const double cur = 10000.0 / 10001.0, hist = 100.0 / 101.0;
        const double mixed = cur + (hist - cur) * .9;
        const double want = mixed / (1 - mixed);
        std::vector<uint32_t> got; readH(got);
        double hot[3], adjacent[3], distant[3];
        unpack(got[8 * w + 8], hot); unpack(got[8 * w + 9], adjacent); unpack(got[2 * w + 2], distant);
        std::printf("flat mono resolve: HDR TAA hot pixel %.2f (compressed-space blend expects %.2f; a linear blend would give 1090), neighbour %.2f, far %.2f\n",
                    hot[0], want, adjacent[0], distant[0]);
        check(std::abs(hot[0] - want) <= .02 * want && std::abs(hot[1] - want) <= .02 * want && std::abs(hot[2] - want) <= .03 * want,
              "HDR TAA: the hot pixel is blended in c/(1+max3(c)) space, history weighted .9: near 111, not the linear 1090");
        check(std::abs(adjacent[0] - 100) <= 1 && std::abs(distant[0] - 100) <= 1, "HDR TAA: the pixels around it stay at 100");
    }
    // A jitter walk over a still firefly: every frame finite, nothing below 0 or above the brightest input, and the
    // texels far from it untouched. No ringing.
    {
        const double fire = 61440;   // representable: 1.875 * 2^15
        static const float walk[8][2] = {{.25f, -.25f}, {-.25f, .25f}, {.125f, .375f}, {-.375f, -.125f},
                                          {.375f, .125f}, {-.125f, -.375f}, {.0f, .25f}, {.25f, .0f}};
        f.reset = true;
        bool finite = true, bounded = true, calm = true;
        for (int frame = 0; frame < 8; ++frame) {
            fill([&](UINT x, UINT y, double (&c)[3]) { c[0] = c[1] = c[2] = (x == 8 && y == 8) ? fire : 0.25; });
            f.jitterX = expectedJx = walk[frame][0]; f.jitterY = expectedJy = walk[frame][1];
            f.previousJitterX = frame ? walk[frame - 1][0] : f.jitterX; f.previousJitterY = frame ? walk[frame - 1][1] : f.jitterY;
            ++f.frame; ok = run(true); f.reset = false;
            std::vector<uint32_t> got; readH(got);
            for (UINT y = 0; y < h; ++y)
                for (UINT x = 0; x < w; ++x) {
                    double g[3]; unpack(got[y * w + x], g);
                    for (int c = 0; c < 3; ++c) {
                        finite = finite && std::isfinite(g[c]);
                        bounded = bounded && g[c] >= 0 && g[c] <= fire * (1 + 1e-3) && (x == 8 && y == 8 ? true : g[c] <= fire * .5 * (1.02));
                        if (std::abs(double(x) - 8) >= 3 || std::abs(double(y) - 8) >= 3) calm = calm && std::abs(g[c] - 0.25) <= 0.25 * 0.016;
                    }
                }
        }
        check(finite, "HDR TAA: a firefly under a jitter walk never makes a NaN or an infinity");
        check(bounded, "HDR TAA: a firefly never leaves an output above its own brightness, or a neighbour above half of it");
        check(calm, "HDR TAA: texels three or more away from a firefly stay at their own value in every frame");
    }
    // A flat field under a jitter walk: still flat, to within the format's rounding and the round trip of the bounded space.
    {
        bool flat = true;
        f.reset = true;
        for (int frame = 0; frame < 6; ++frame) {
            fill([&](UINT, UINT, double (&c)[3]) { c[0] = 1024; c[1] = 512; c[2] = 1536; });
            f.jitterX = expectedJx = (frame & 1) ? .25f : -.25f; f.jitterY = expectedJy = (frame & 2) ? .125f : -.125f;
            f.previousJitterX = -f.jitterX; f.previousJitterY = -f.jitterY;
            ++f.frame; ok = run(true); f.reset = false;
            std::vector<uint32_t> got; readH(got);
            for (UINT i = 0; i < w * h; ++i) {
                double g[3]; unpack(got[i], g);
                // Within an ulp of the format: the round trip through the bounded space is exact to a part in 10^4, and
                // what is left is the conversion into R11G11B10F (nearest costs half an ulp, truncation up to one).
                flat = flat && std::abs(g[0] - 1024) <= ulp(1024, 6) && std::abs(g[1] - 512) <= ulp(512, 6) &&
                       std::abs(g[2] - 1536) <= ulp(1536, 5);
            }
        }
        check(flat, "HDR TAA: a flat HDR field stays flat through the bounded space and back, frame after frame");
    }

    // ---- 5. a backend that refuses, and the spatial recovery into the game's target --------------------------------
    f.mode = FlatMonoResolveMode::Dlss; f.reset = false; ++f.frame;
    fill([&](UINT x, UINT y, double (&c)[3]) { c[0] = 100 + 50.0 * x; c[1] = 10.0 * (y + 1); c[2] = 40.0 + 30.0 * x + 20.0 * y; });
    f.jitterX = f.previousJitterX = expectedJx = .5f; f.jitterY = f.previousJitterY = expectedJy = 0;
    const uint64_t untouched = hHash();
    backendFail = true;
    const char* why = nullptr;
    ok = run(false, &why);
    backendFail = false;
    check(!ok && hHash() == untouched, "HDR route: a backend refusal leaves the game's target bit for bit what it was");
    bindOriginal();
    ComPtr<ID3D11ShaderResourceView> none; const char* fallbackReason = nullptr;
    const auto beforeSpatial = edvr::flatMonoResolveStats();
    const bool recovered = edvr::flatMonoResolveSpatialFallback(device, context, f, none.GetAddressOf(), &fallbackReason);
    check(recovered && none == nullptr && restored(), "HDR route: the spatial recovery writes into the game's target, returns no view and restores the pipeline");
    check(edvr::flatMonoResolveStats().hdrSpatial == beforeSpatial.hdrSpatial + 1, "HDR route: the spatial recovery is counted");
    int badSpatial = 0;
    worstMiss([&](UINT x, UINT y, int c) {
        const std::vector<double>& ch = c == 0 ? inR : c == 1 ? inG : inB;
        return bilinear(ch, x + f.jitterX, y + f.jitterY);
    }, 0.5, &badSpatial);
    check(badSpatial == 0, "HDR route: the spatial recovery is the jittered input resampled on the unjittered grid");

    // ---- 6. refusals that happen before anything is written ---------------------------------------------------------
    {
        fill([&](UINT x, UINT, double (&c)[3]) { c[0] = 10.0 * x + 1; c[1] = 2; c[2] = 3; });
        const uint64_t base = hHash();
        // R < D: upscaling keeps the copy route (decision (c)).
        FlatMonoResolveFrame up = f; up.outputWidth = 32; up.outputHeight = 32; up.reset = true; ++up.frame;
        bindOriginal(); ComPtr<ID3D11ShaderResourceView> out; const char* reason = nullptr;
        check(!edvr::flatMonoResolve(device, context, up, out.GetAddressOf(), &reason) && reason &&
              !std::strcmp(reason, "flat-resolve-hdr-requires-render-size-evaluation") && restored() && hHash() == base,
              "HDR route: a render below the output declines, naming why, with the target and the pipeline untouched");
        // EDVR's TAA above D evaluates on the display grid: not E = R.
        FlatMonoResolveFrame taaDown = f; taaDown.mode = FlatMonoResolveMode::Taa; taaDown.outputWidth = 8; taaDown.outputHeight = 8;
        bindOriginal(); reason = nullptr;
        check(!edvr::flatMonoResolve(device, context, taaDown, out.GetAddressOf(), &reason) && reason &&
              !std::strcmp(reason, "flat-resolve-hdr-requires-render-size-evaluation") && restored() && hHash() == base,
              "HDR route: EDVR's TAA at R > D (display-grid evaluation) stays on the copy route");
        // An LDR view over an LDR texture is not H.
        auto ldr = texture(device, w, h, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET);
        auto ldrView = view(device, ldr.Get());
        FlatMonoResolveFrame wrong = f; wrong.color = ldrView.Get();
        bindOriginal(); reason = nullptr;
        check(!edvr::flatMonoResolve(device, context, wrong, out.GetAddressOf(), &reason) && reason &&
              !std::strcmp(reason, "flat-resolve-input-view-mismatch") && restored(),
              "HDR route: an R8G8B8A8 view is not the game's HDR target");
        // An R11G11B10F texture the game cannot render into cannot take the result.
        auto noRt = makeH(D3D11_BIND_SHADER_RESOURCE);
        auto noRtView = view(device, noRt.Get());
        FlatMonoResolveFrame readOnly = f; readOnly.color = noRtView.Get();
        bindOriginal(); reason = nullptr;
        check(!edvr::flatMonoResolve(device, context, readOnly, out.GetAddressOf(), &reason) && reason &&
              !std::strcmp(reason, "flat-resolve-input-view-mismatch") && restored(),
              "HDR route: a texture with no render-target bind cannot take the result");
        // The same gates in the spatial recovery.
        bindOriginal(); reason = nullptr;
        check(!edvr::flatMonoResolveSpatialFallback(device, context, up, out.GetAddressOf(), &reason) && hHash() == base && restored(),
              "HDR route: the spatial recovery refuses a render below the output too");
    }

    // ---- 7. the preflight knows the route ---------------------------------------------------------------------------
    {
        edvr::FlatMonoResolvePreflight planned{};
        planned.renderWidth = w; planned.renderHeight = h; planned.outputWidth = w; planned.outputHeight = h;
        planned.mode = FlatMonoResolveMode::Dlss; planned.hdr = true;
        planned.colorViewFormat = DXGI_FORMAT_R11G11B10_FLOAT; planned.depthViewFormat = DXGI_FORMAT_R32_FLOAT;
        auto ready = edvr::flatMonoResolvePreflight(device, context, planned);
        check(ready.readyForRasterJitter() && ready.spatialFallbackReady && ready.backendAvailable,
              "HDR route: a valid HDR plan preflights ready, its pixel-shader fallback included");
        auto low = planned; low.outputWidth = 32; low.outputHeight = 32;
        auto refused = edvr::flatMonoResolvePreflight(device, context, low);
        check(refused.status == edvr::FlatMonoResolvePreflightStatus::InvalidMetadata && refused.reason &&
              !std::strcmp(refused.reason, "flat-preflight-hdr-requires-render-size-evaluation"),
              "HDR route: a plan with the render below the output is refused by name");
        auto taaDown = planned; taaDown.mode = FlatMonoResolveMode::Taa; taaDown.renderWidth = 32; taaDown.renderHeight = 32;
        refused = edvr::flatMonoResolvePreflight(device, context, taaDown);
        check(refused.status == edvr::FlatMonoResolvePreflightStatus::InvalidMetadata,
              "HDR route: a TAA plan that evaluates at the display size above it is refused");
        auto ldrFormat = planned; ldrFormat.colorViewFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
        refused = edvr::flatMonoResolvePreflight(device, context, ldrFormat);
        check(refused.status == edvr::FlatMonoResolvePreflightStatus::InvalidMetadata && refused.reason &&
              !std::strcmp(refused.reason, "flat-preflight-unsupported-source-format"),
              "HDR route: an LDR colour view in an HDR plan is refused");
        auto hdrFormatLdr = planned; hdrFormatLdr.hdr = false;
        refused = edvr::flatMonoResolvePreflight(device, context, hdrFormatLdr);
        check(refused.status == edvr::FlatMonoResolvePreflightStatus::InvalidMetadata,
              "the copy route's plan still refuses an R11G11B10F colour view");
    }

    // ---- 8. the copy route after the HDR route is the copy route -----------------------------------------------------
    {
        std::vector<uint32_t> red(w * h, 0xff0000ff);
        auto color = texture(device, w, h, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE, red.data(), w * 4);
        auto colorView = view(device, color.Get());
        FlatMonoResolveFrame ldr{};
        ldr.color = colorView.Get(); ldr.depth = depthView.Get(); ldr.renderWidth = w; ldr.renderHeight = h;
        ldr.outputWidth = w; ldr.outputHeight = h; ldr.deltaMs = 16; camera(ldr.camera); camera(ldr.previousCamera);
        ldr.engine = {slotView.Get(), poolView.Get(), now.Get(), old.Get()};
        ldr.mode = FlatMonoResolveMode::Dlss; ldr.frame = f.frame + 1000; ldr.reset = true;
        expectedJx = expectedJy = 0; observedHdr = true;
        bindOriginal(); ComPtr<ID3D11ShaderResourceView> out; const char* reason = nullptr;
        const bool good = edvr::flatMonoResolve(device, context, ldr, out.GetAddressOf(), &reason);
        uint32_t px = 0;
        if (good && out) {
            ComPtr<ID3D11Resource> r; out->GetResource(r.GetAddressOf()); ComPtr<ID3D11Texture2D> t; r.As(&t);
            readPixel(context, t.Get(), &px, 4, 8, 8);
        }
        check(good && out && restored() && !observedHdr && px == 0xff0000ff,
              "after the HDR route, the copy route resolves an LDR frame as before: output view, LDR backend flag, the input's pixels");
    }

    // ---- 9. the crash-safe breadcrumbs around every step (src\d3d11\flat_hdr_crumbs.h) ---------------------------------------
    // What the route writes to edvr_breadcrumbs.txt so that a session that ends inside the treatment names the step, read back
    // from the rig's breadcrumb() while the real resolver runs on WARP. The runtime's own calls (the admission, the reach, the
    // Present's end) are driven by hand, in the order the runtime makes them; what no rig can run (treatHdr, the real Present,
    // the two SDKs) is pinned as source by flat_temporal_test. Pinned here: every begin has its end on a normal frame, the
    // steps come in the order they run, each creation says what it makes, a refusal and a state restore are written, a call that is
    // not the route's writes nothing, and after the third frame to reach the resolver the resolver writes nothing at all.
    {
        namespace ct = hdr_crumb_trail;
        // Everything in this block but (g) is the DXMT case: the crumbs' device gate (flat_hdr_crumbs.h, THE GATE) is open, which the
        // runtime does from the markers and WARP's device would not answer. (g) shuts it, as it is on every Windows device.
        edvr::hdrCrumbEnable(true);
        const auto inOrder = [&](const std::vector<ct::Crumb>& trail, std::initializer_list<std::pair<const char*, const char*>> steps) {
            int at = -1;
            for (const auto& step : steps) {
                const int i = ct::find(trail, step.first, step.second, static_cast<size_t>(at + 1));
                if (i < 0) { std::printf("  (crumb trail: no '%s %s' after index %d in: %s)\n", step.first, step.second, at, ct::outline(trail).c_str()); return false; }
                at = i;
            }
            return true;
        };
        // One frame as the runtime drives it around a call into the resolver: admitted, reached, the call, the Present's end.
        const auto routeFrame = [&](const char* backend, auto&& resolverCall) {
            edvr::hdrCrumbAdmit(f.frame, backend);
            edvr::hdrCrumbReach(f.frame, backend, "resolve");
            resolverCall();
            { edvr::HdrCrumbFrameEnd end(0); }
        };
        const auto restart = [&] { edvr::flatMonoResolveReset(); edvr::hdrCrumbReset(); crumbLines.clear(); };
        f.mode = FlatMonoResolveMode::Dlss; f.hdr = true; f.jitterX = f.jitterY = f.previousJitterX = f.previousJitterY = 0; expectedJx = expectedJy = 0;
        fill([&](UINT x, UINT y, double (&c)[3]) { c[0] = 64 + 8.0 * x; c[1] = 32 + y; c[2] = 16; });

        // (a) the preflight at the Present of the frame the route admitted: on a renderer that has made nothing, it writes
        // what it creates, the texture crumbs saying what each is, and ends; the backend's first ask is bracketed.
        restart();
        edvr::hdrCrumbAdmit(f.frame, "dlss");
        {
            edvr::FlatMonoResolvePreflight planned{};
            planned.renderWidth = w; planned.renderHeight = h; planned.outputWidth = w; planned.outputHeight = h;
            planned.mode = FlatMonoResolveMode::Dlss; planned.hdr = true;
            planned.colorViewFormat = DXGI_FORMAT_R11G11B10_FLOAT; planned.depthViewFormat = DXGI_FORMAT_R32_FLOAT;
            edvr::HdrCrumbFrameEnd end(0);
            const auto ready = edvr::flatMonoResolvePreflight(device, context, planned);
            check(ready.readyForRasterJitter(), "breadcrumbs: the HDR preflight the crumbs bracket is the one that was always ready");
        }
        {
            const auto trail = ct::trail(crumbLines);
            std::string gap;
            check(ct::balanced(trail, &gap), "breadcrumbs: the preflight's crumbs balance");
            check(inOrder(trail, {{"admitted", ""}, {"frame-end", "begin"}, {"preflight", "begin"}, {"create-context-state", "begin"},
                                  {"create-context-state", "end"}, {"create-compute-shaders", "begin"}, {"create-compute-shaders", "end"},
                                  {"create-constants-sampler", "begin"}, {"create-constants-sampler", "end"}, {"create-hdr-vs", "begin"},
                                  {"create-hdr-vs", "end"}, {"create-hdr-ps-finish", "begin"}, {"create-hdr-ps-finish", "end"},
                                  {"create-hdr-ps-spatial", "begin"}, {"create-hdr-ps-spatial", "end"}, {"create-texture", "begin"},
                                  {"create-texture", "end"}, {"backend-available", "begin"}, {"backend-available", "end"},
                                  {"preflight", "end"}, {"frame-end", "end"}}),
                  "breadcrumbs: a cold preflight writes its creations in the order it makes them, the SDK's first ask after them, all inside its own pair");
            check(ct::count(trail, "create-texture", "begin") == 5 && ct::count(trail, "create-texture", "end") == 5 &&
                      ct::count(trail, "create-rtv", "begin") == 0,
                  "breadcrumbs: the DLSS route's five private images are each crumbed, and the preflight makes no target view");
            const int pre = ct::find(trail, "preflight", "begin");
            check(pre >= 0 && trail[pre].detail == "plan=16x16->16x16 mode=dlss", "breadcrumbs: the preflight names its plan");
            const int pe = ct::find(trail, "preflight", "end");
            check(pe >= 0 && ct::has(trail[pe].detail, "status=0 ready=1 reason=ready-backend-feature-creation-deferred"),
                  "breadcrumbs: the preflight's end carries its verdict");
            // Each texture: role, format, size before; every result after.
            struct Want { const char* role; const char* fmt; const char* uavResult; };
            static const Want wants[5] = {{"color", "R11G11B10_FLOAT(26)", "0x8000000A"}, {"depth0", "R32_FLOAT(41)", "0x00000000"},
                                          {"motion", "R16G16_FLOAT(34)", "0x00000000"}, {"rejection", "R8_UNORM(61)", "0x00000000"},
                                          {"output0", "R16G16B16A16_FLOAT(10)", "0x00000000"}};
            bool textures = true; int at = -1;
            for (const auto& want : wants) {
                const int b = ct::find(trail, "create-texture", "begin", static_cast<size_t>(at + 1));
                const int e = ct::find(trail, "create-texture", "end", static_cast<size_t>(at + 1));
                std::string begin = b >= 0 ? trail[b].detail : "", end = e >= 0 ? trail[e].detail : "";
                textures = textures && b >= 0 && e == b + 1 && begin == std::string("role=") + want.role + " fmt=" + want.fmt + " size=16x16 uav=" + (std::strcmp(want.role, "color") ? "1" : "0") &&
                           ct::has(end, "hr=0x00000000 srv=0x00000000") && ct::has(end, (std::string("uav=") + want.uavResult).c_str());
                at = e;
            }
            check(textures, "breadcrumbs: each image says its role, format and size before it is made, and its three results after (E_PENDING where a call was not reached)");
            check(ct::count(trail, "backend-available", "begin") == 1 && trail[ct::find(trail, "backend-available", "end")].detail == "ok=1 reason=none",
                  "breadcrumbs: the SDK's first availability ask is bracketed, with its answer");
            check(!edvr::hdrCrumbLive(), "breadcrumbs: the frame's end closes the gate");
        }
        const size_t coldPreflightCrumbs = crumbLines.size();

        // (b) three frames that reach the resolver, then nothing: from a warm renderer (what the preflight left) the first frame
        // makes only its target view, and the creations are not written again. The preflight's frame was not one of the three
        // (it never reached the resolver), so the count starts here without a new renderer. The 5 s line's step counts do not
        // stop with the crumbs: all five frames are counted, at every step of the resolver.
        edvr::hdrCrumbReset();
        crumbLines.clear();
        size_t sizes[5] = {};
        const auto steps0 = edvr::flatMonoResolveStats();
        for (int n = 0; n < 5; ++n) {
            f.reset = n == 0; ++f.frame;
            routeFrame("dlss", [&] { run(true); });
            sizes[n] = crumbLines.size();
        }
        {
            const auto steps1 = edvr::flatMonoResolveStats();
            check(steps1.hdrCaptured - steps0.hdrCaptured == 5 && steps1.hdrCopied - steps0.hdrCopied == 5 && steps1.hdrPrepped - steps0.hdrPrepped == 5 &&
                      steps1.hdrBackend - steps0.hdrBackend == 5 && steps1.hdrFinished - steps0.hdrFinished == 5 &&
                      steps1.hdrRestored - steps0.hdrRestored == 5,
                  "breadcrumbs: the step counts of the 5 s line count every frame at every step, the two after the crumbs' third frame included");
        }
        {
            const auto trail = ct::trail(crumbLines);
            std::string gap;
            check(ct::balanced(trail, &gap), "breadcrumbs: three frames through the real resolver leave every begin ended");
            check(ct::count(trail, "reached", "") == 3 && sizes[3] == sizes[2] && sizes[4] == sizes[2] && !edvr::hdrCrumbLive(),
                  "breadcrumbs: the third frame to reach the resolver is the last that writes; the fourth and fifth write nothing");
            bool slotsOk = true;
            for (const auto& c : trail) slotsOk = slotsOk && c.slot >= 1 && c.slot <= 3;
            check(slotsOk, "breadcrumbs: no crumb is numbered past 3/3");
            // The first frame on a warm renderer: the route's target view is made and the creations already made are not written again.
            const int first = ct::find(trail, "admitted", "");
            const int second = ct::find(trail, "admitted", "", static_cast<size_t>(first + 1));
            const std::vector<ct::Crumb> one(trail.begin() + first, trail.begin() + second);
            check(inOrder(one, {{"admitted", ""}, {"reached", ""}, {"create-rtv", "begin"}, {"create-rtv", "end"}, {"capture-state", "begin"},
                                {"capture-state", "end"}, {"copy-h", "begin"}, {"copy-h", "end"}, {"prep", "begin"}, {"prep", "end"},
                                {"backend", "begin"}, {"backend", "end"}, {"finish-bind", "begin"}, {"finish-bind", "end"},
                                {"finish-draw", "begin"}, {"finish-draw", "end"}, {"restore-state", "begin"}, {"restore-state", "end"},
                                {"frame-end", "begin"}, {"frame-end", "end"}}) &&
                      ct::count(one, "create-texture", "begin") == 0 && ct::count(one, "create-compute-shaders", "begin") == 0,
                  "breadcrumbs: a first frame writes capture, copy, prep, backend, finish-bind, finish-draw and restore, in that order, between its reach and its frame end");
            const int rtv = ct::find(one, "create-rtv", "begin");
            check(rtv >= 0 && one[rtv].detail == "over H fmt=R11G11B10_FLOAT(26) size=16x16" && one[rtv + 1].detail == "hr=0x00000000",
                  "breadcrumbs: the target view over H says what it is over and its result");
            const int copy = ct::find(one, "copy-h", "begin"), prep = ct::find(one, "prep", "begin"), backend = ct::find(one, "backend", "begin"),
                      bind = ct::find(one, "finish-bind", "begin"), draw = ct::find(one, "finish-draw", "begin");
            check(copy >= 0 && one[copy].detail == "fmt=R11G11B10_FLOAT(26) size=16x16" && one[prep].detail == "groups=2x2" &&
                      one[backend].detail == "mode=dlss in=16x16 out=16x16 reset=1" && one[backend + 1].step == "backend" && one[backend + 1].detail == "ok=1 reason=none" &&
                      one[bind].detail == "ps=finish target=16x16 views=8" && one[draw].detail == "ps=finish vertices=3",
                  "breadcrumbs: each step names what it does before it does it, and the backend says how it ended");
            bool one1 = true;
            for (const auto& c : one) one1 = one1 && c.slot == 1;
            check(one1, "breadcrumbs: every crumb of the first frame reads 1/3");
            check(ct::find(trail, "capture-state", "end") < ct::find(trail, "restore-state", "begin") && ct::has(trail[ct::find(trail, "finish-draw", "begin")].detail, "ps=finish"),
                  "breadcrumbs: the game's state is out before the draw and back after it");
        }

        // (c) a cold resolve with no preflight before it (the first HDR frame can be the first call): the creations are written inside it.
        restart();
        f.reset = true; ++f.frame;
        routeFrame("dlss", [&] { run(true); });
        {
            const auto trail = ct::trail(crumbLines);
            std::string gap;
            check(ct::balanced(trail, &gap), "breadcrumbs: a cold resolve's crumbs balance");
            check(inOrder(trail, {{"admitted", ""}, {"reached", ""}, {"create-context-state", "begin"}, {"create-compute-shaders", "begin"},
                                  {"create-constants-sampler", "begin"}, {"create-hdr-vs", "begin"}, {"create-hdr-ps-spatial", "end"},
                                  {"create-texture", "begin"}, {"create-rtv", "begin"}, {"capture-state", "begin"}, {"copy-h", "begin"},
                                  {"prep", "begin"}, {"backend", "begin"}, {"finish-bind", "begin"}, {"finish-draw", "begin"},
                                  {"restore-state", "end"}, {"frame-end", "end"}}),
                  "breadcrumbs: with no preflight before it the resolve writes every creation, then capture, copy, prep, backend, finish and restore");
            check(ct::count(trail, "create-texture", "begin") == 5, "breadcrumbs: the five images are made inside the cold resolve");
        }
        const size_t coldFrameCrumbs = crumbLines.size();

        // (d) a backend that refuses: the refusal is written, and the game's state is still put back (restore-state after the backend's end).
        // The step counts say where the frame stopped: the game's state out, H copied, prep run, the backend refused, nothing drawn, the state back.
        restart();
        f.reset = true; ++f.frame;
        backendFail = true;
        const auto refuse0 = edvr::flatMonoResolveStats();
        routeFrame("dlss", [&] { run(false); });
        const auto refuse1 = edvr::flatMonoResolveStats();
        backendFail = false;
        {
            const auto trail = ct::trail(crumbLines);
            std::string gap;
            const int backend = ct::find(trail, "backend", "end");
            check(ct::balanced(trail, &gap) && backend >= 0 && trail[backend].detail == "ok=0 reason=injected-backend-refusal" &&
                      ct::find(trail, "restore-state", "end") > backend && ct::count(trail, "finish-draw", "begin") == 0,
                  "breadcrumbs: a refused backend ends its step with the reason, draws nothing, and the state is restored");
            check(refuse1.hdrCaptured - refuse0.hdrCaptured == 1 && refuse1.hdrCopied - refuse0.hdrCopied == 1 && refuse1.hdrPrepped - refuse0.hdrPrepped == 1 &&
                      refuse1.hdrBackend - refuse0.hdrBackend == 0 && refuse1.hdrFinished - refuse0.hdrFinished == 0 &&
                      refuse1.hdrRestored - refuse0.hdrRestored == 1,
                  "breadcrumbs: a refused backend's frame is counted at capture, copy, prep and restore, and not at the backend or the draw");
        }

        // (e) the spatial recovery writes its own steps: capture, copy, the finish through the spatial shader, restore.
        restart();
        f.reset = true; ++f.frame;
        edvr::hdrCrumbAdmit(f.frame, "dlss");
        edvr::hdrCrumbReach(f.frame, "dlss", "spatial-recovery");
        const auto spatial0 = edvr::flatMonoResolveStats();
        {
            bindOriginal();
            ComPtr<ID3D11ShaderResourceView> noView; const char* recoverWhy = nullptr;
            check(edvr::flatMonoResolveSpatialFallback(device, context, f, noView.GetAddressOf(), &recoverWhy) && restored(), "breadcrumbs: the recovery the crumbs bracket still recovers");
        }
        const auto spatial1 = edvr::flatMonoResolveStats();
        { edvr::HdrCrumbFrameEnd end(0); }
        check(spatial1.hdrCaptured - spatial0.hdrCaptured == 1 && spatial1.hdrCopied - spatial0.hdrCopied == 1 && spatial1.hdrPrepped - spatial0.hdrPrepped == 0 &&
                  spatial1.hdrBackend - spatial0.hdrBackend == 0 && spatial1.hdrFinished - spatial0.hdrFinished == 1 &&
                  spatial1.hdrRestored - spatial0.hdrRestored == 1,
              "breadcrumbs: the spatial recovery is counted at capture, copy, draw and restore, and not at prep or the backend");
        {
            const auto trail = ct::trail(crumbLines);
            std::string gap;
            check(ct::balanced(trail, &gap) &&
                      inOrder(trail, {{"reached", ""}, {"capture-state", "begin"}, {"capture-state", "end"}, {"copy-h", "begin"}, {"copy-h", "end"},
                                      {"finish-bind", "begin"}, {"finish-bind", "end"}, {"finish-draw", "begin"}, {"finish-draw", "end"},
                                      {"restore-state", "begin"}, {"restore-state", "end"}, {"frame-end", "end"}}) &&
                      ct::count(trail, "prep", "begin") == 0 && ct::count(trail, "backend", "begin") == 0 &&
                      ct::has(trail[ct::find(trail, "finish-bind", "begin")].detail, "ps=spatial"),
                  "breadcrumbs: the spatial recovery writes capture, copy, finish-bind, finish-draw and restore, through the spatial pixel shader, and no prep or backend");
        }

        // (f) a call that is not the route's writes nothing, even inside a frame the route has admitted and reached: the copy route's
        // resolve and its recovery (hdr false) in the same frame.
        restart();
        edvr::hdrCrumbAdmit(f.frame, "dlss");
        edvr::hdrCrumbReach(f.frame, "dlss", "resolve");
        const auto copyRoute0 = edvr::flatMonoResolveStats();
        {
            std::vector<uint32_t> red(w * h, 0xff0000ff);
            auto color = texture(device, w, h, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE, red.data(), w * 4);
            auto colorView = view(device, color.Get());
            FlatMonoResolveFrame ldr{};
            ldr.color = colorView.Get(); ldr.depth = depthView.Get(); ldr.renderWidth = w; ldr.renderHeight = h;
            ldr.outputWidth = w; ldr.outputHeight = h; ldr.deltaMs = 16; camera(ldr.camera); camera(ldr.previousCamera);
            ldr.engine = {slotView.Get(), poolView.Get(), now.Get(), old.Get()};
            ldr.mode = FlatMonoResolveMode::Dlss; ldr.frame = f.frame + 2000; ldr.reset = true;
            expectedJx = expectedJy = 0;
            bindOriginal(); ComPtr<ID3D11ShaderResourceView> out; const char* reason = nullptr;
            const bool good = edvr::flatMonoResolve(device, context, ldr, out.GetAddressOf(), &reason);
            ComPtr<ID3D11ShaderResourceView> noView; const char* recoverWhy = nullptr;
            const bool recoveredLdr = edvr::flatMonoResolveSpatialFallback(device, context, ldr, noView.GetAddressOf(), &recoverWhy);
            check(good && recoveredLdr, "breadcrumbs: the copy route's resolve and recovery still run while the route's frame is live");
        }
        {
            const auto trail = ct::trail(crumbLines);
            check(trail.size() == 2 && trail[0].step == "admitted" && trail[1].step == "reached",
                  "breadcrumbs: the copy route's resolve and its recovery write nothing under the route's name, even in a frame the route admitted");
            const auto copyRoute1 = edvr::flatMonoResolveStats();
            check(copyRoute1.hdrCaptured == copyRoute0.hdrCaptured && copyRoute1.hdrCopied == copyRoute0.hdrCopied && copyRoute1.hdrPrepped == copyRoute0.hdrPrepped &&
                      copyRoute1.hdrBackend == copyRoute0.hdrBackend && copyRoute1.hdrFinished == copyRoute0.hdrFinished &&
                      copyRoute1.hdrRestored == copyRoute0.hdrRestored,
                  "breadcrumbs: the copy route's resolve and its recovery are not counted as the HDR route's steps");
        }
        { edvr::HdrCrumbFrameEnd end(0); }

        // (g) THE DEVICE GATE: the same route with the gate shut, as the runtime leaves it on every device the markers do not call DXMT.
        // A cold preflight, three frames that reach the resolver, a refused backend and a spatial recovery write not one crumb, the armed
        // line included, and the 5 s line's step counts count the frames all the same (they are plain counters, read into a log line).
        // The same again with the explicit capture FORCED (advanced.flat_context_isolation=capture), which is what a tester on Windows
        // would do: the capture runs, and the gate, which is the detection's alone, stays shut.
        for (const bool forcedCapture : {false, true}) {
            restart();
            edvr::hdrCrumbEnable(false);
            edvr::flatMonoResolveSetIsolation(forcedCapture ? edvr::FlatContextIsolation::Capture : edvr::FlatContextIsolation::Auto);
            edvr::flatMonoResolveReset();
            const char* what = forcedCapture ? "breadcrumbs, gate shut, capture forced" : "breadcrumbs, gate shut";
            char name[200];
            const auto say = [&](const char* text) { std::snprintf(name, sizeof(name), "%s: %s", what, text); return name; };
            check(!edvr::hdrCrumbArmed("auto") && crumbLines.empty(), say("the armed line is not written"));
            {
                edvr::FlatMonoResolvePreflight planned{};
                planned.renderWidth = w; planned.renderHeight = h; planned.outputWidth = w; planned.outputHeight = h;
                planned.mode = FlatMonoResolveMode::Dlss; planned.hdr = true;
                planned.colorViewFormat = DXGI_FORMAT_R11G11B10_FLOAT; planned.depthViewFormat = DXGI_FORMAT_R32_FLOAT;
                edvr::hdrCrumbAdmit(f.frame, "dlss");
                edvr::HdrCrumbFrameEnd end(0);
                check(edvr::flatMonoResolvePreflight(device, context, planned).readyForRasterJitter(), say("the preflight is the one that was always ready"));
            }
            const auto gate0 = edvr::flatMonoResolveStats();
            for (int n = 0; n < 3; ++n) {
                f.reset = n == 0; ++f.frame;
                routeFrame("dlss", [&] { run(true); });
            }
            const auto gate1 = edvr::flatMonoResolveStats();
            backendFail = true;
            f.reset = true; ++f.frame;
            routeFrame("dlss", [&] { run(false); });
            backendFail = false;
            edvr::hdrCrumbAdmit(f.frame, "dlss");
            edvr::hdrCrumbReach(f.frame, "dlss", "spatial-recovery");
            {
                bindOriginal();
                ComPtr<ID3D11ShaderResourceView> noView; const char* recoverWhy = nullptr;
                check(edvr::flatMonoResolveSpatialFallback(device, context, f, noView.GetAddressOf(), &recoverWhy) && restored(), say("the recovery still recovers"));
            }
            { edvr::HdrCrumbFrameEnd end(0); }
            check(crumbLines.empty() && edvr::g_hdrCrumbs.written.load() == 0 && !edvr::hdrCrumbLive() && edvr::g_hdrCrumbs.reached == 0 && !edvr::g_hdrCrumbs.spent,
                  say("a preflight, three resolves, a refused backend and a recovery write no crumb at all and start no budget"));
            check(gate1.hdrCaptured - gate0.hdrCaptured == 3 && gate1.hdrCopied - gate0.hdrCopied == 3 && gate1.hdrPrepped - gate0.hdrPrepped == 3 &&
                      gate1.hdrBackend - gate0.hdrBackend == 3 && gate1.hdrFinished - gate0.hdrFinished == 3 && gate1.hdrRestored - gate0.hdrRestored == 3,
                  say("the 5 s line's step counts count all three frames at every step, as they do with the gate open"));
            const auto gate2 = edvr::flatMonoResolveStats();
            check(forcedCapture ? (gate2.isolationCaptures > gate0.isolationCaptures && gate2.isolation && !std::strcmp(gate2.isolation, "capture"))
                                : (gate2.isolationSwaps > gate0.isolationSwaps && gate2.isolation && !std::strcmp(gate2.isolation, "swap")),
                  say(forcedCapture ? "the explicit capture ran (it is forced) and wrote no crumb: the gate is the markers', not the key's"
                                    : "the swap ran, as on every Windows device"));
        }
        edvr::flatMonoResolveSetIsolation(edvr::FlatContextIsolation::Auto);
        edvr::hdrCrumbEnable(true);   // (the gate is open again for the printout below, which the cold-resolve count came from)
        edvr::hdrCrumbReset();
        // What it comes to, for the cap (flat_hdr_crumbs.h, WHAT IT COSTS): the real thing adds the depth view (2), the SDK's own
        // create and evaluate (2 and 2), before-present (2) and the Present (2) that only the game's process can write.
        std::printf("flat mono resolve: HDR breadcrumbs: a cold preflight writes %zu crumbs, a first frame on a warm renderer %zu, a steady frame %zu "
                    "(resolver, admission and frame end; the game's process adds 6 more a frame), a cold resolve %zu; the budget is %u and a frame "
                    "past the third writes none\n", coldPreflightCrumbs, sizes[0], sizes[2] - sizes[1], coldFrameCrumbs,
                    static_cast<unsigned>(edvr::kHdrCrumbCap));
        // The rig's default for whoever runs next: the gate shut, as on a Windows device.
        edvr::hdrCrumbEnable(false);
    }
}
