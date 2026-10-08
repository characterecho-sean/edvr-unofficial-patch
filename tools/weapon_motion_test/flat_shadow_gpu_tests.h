#pragma once
// THE SHADOW OF THE SIBLING MODEL, ON A DEVICE (design doc section 104, the 10-07 settlement walk). The two compute shaders and their dispatcher
// (src\d3d11\flat_foreground_shadow_gpu.h, shaders in flat_foreground_motion_shader.h) are run on WARP (or hardware) over synthetic buffers, with no
// vertex shader, no adapter and no raster, and every draw's record is held to the CPU form of the model (flat_foreground_shadow.h, whose cases and
// counters tools\flat_temporal_test\flat_shadow_model_tests.h pins): kinds, donors and gate bits exactly, the numbers within float precision.
//
// A scene is a list of draws in screen space: a vertex at pixel P moving by m has Now at P and Before0 at P + m (ndc x = 2 P.x / W - 1, ndc y =
// 1 - 2 P.y / H, z = 0.5 for every vertex so that each triangle has one z, w the view depth); a draw that matched has the same identity words in
// Identity and PreviousIdentity0 (a prior count of one); a receiver has no prior (prior count zero, null views) or a prior of another identity.
// The same description feeds the model, so the two cannot drift apart.
//
// Where the shader and the model differ, and nothing reads the difference: a matched draw with no vertex (kind none) still carries the donors'
// gates in the shader's record and none in the model's; a draw with no donor carries spread -2e30 in the shader's (the empty accumulator's
// extent) and 0 in the model's. flatShadowAccumulate files neither.
#include <array>
#include <cstring>
#include <string>
#include <vector>
#include "../flat_temporal_test/flat_shadow_model_tests.h"

namespace flatshadow_gpu {

using Microsoft::WRL::ComPtr;
using edvr::ShadowDrawRef;
using edvr::ShadowKind;
using edvr::ShadowRecord;
using edvr::ShadowVertex;

// The slots of the compute stage the dispatcher saves and restores (cs_stage_save.h).
inline constexpr unsigned srvs = edvr::CsStageSave::kSrvs, uavs = edvr::CsStageSave::kUavs, cbs = edvr::CsStageSave::kCbs;

// One draw of a scene.
struct Draw {
    uint32_t x = flatshadow_test::kIdentityX, y = flatshadow_test::kIdentityY;   // identity.x and identity.y with byte 30 left out
    bool prior = true;           // a prior is bound (prior count one)
    bool matched = true;         // the prior has this draw's identity (otherwise another's)
    bool badPrior = false;       // the prior's positions cannot be used: the map refuses every vertex, though the identity matched
    bool invalidNow = false;     // the draw's own positions are not valid: the map refuses every vertex
    bool unreadable = false;     // Identity.z == 0
    bool notAuthentic = false;   // the instance index carries a flag bit
    unsigned byte30 = 0, priorByte30 = 0;   // identity.y bits 16-23, a per-instance parameter the match ignores
    std::vector<ShadowVertex> verts;       // positions (u, v, w), and the motion for a draw with a prior of its identity
    // Triangles (three vertices each) the map refuses one by one, by what is wrong with them: 1 a motion over the shader's limit (300 px), 2 a prior with
    // a negative w, 3 a prior position that is not a number, 4 a current z that differs within the triangle. Empty: none.
    std::vector<uint8_t> bad;
    bool badTriangle(unsigned vertex) const { return vertex / 3 < bad.size() && bad[vertex / 3] != 0; }
};

// What the model is told of a draw.
inline ShadowDrawRef modelOf(const Draw& d) {
    ShadowDrawRef r;
    r.identityX = d.x; r.identityY = d.y; r.identityReadable = !d.unreadable;
    r.matched = d.prior && d.matched && !d.unreadable && !d.notAuthentic;
    // The extent is every valid current position (a vertex of a triangle the map refuses still has one); the motion is those the map takes.
    if (!d.invalidNow) for (const ShadowVertex& v : d.verts) r.hull.push_back({v.u, v.v, v.w});
    if (r.matched && !d.badPrior && !d.invalidNow)
        for (unsigned i = 0; i < d.verts.size(); ++i) if (!d.badTriangle(i)) r.vertices.push_back(d.verts[i]);
    return r;
}

// One draw's resources.
struct Resident {
    ComPtr<ID3D11Buffer> buffers[5];
    ComPtr<ID3D11ShaderResourceView> now, before, identity, priorIdentity, instance;
    unsigned vertices = 0;
};

inline ComPtr<ID3D11ShaderResourceView> typedView(ID3D11Device* dev, const void* data, unsigned elements, DXGI_FORMAT format, ComPtr<ID3D11Buffer>& buffer) {
    D3D11_BUFFER_DESC d{};
    d.ByteWidth = elements * 4 * (format == DXGI_FORMAT_R32_UINT ? 1 : 4); d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sd{data, 0, 0};
    hr(dev->CreateBuffer(&d, &sd, &buffer));
    D3D11_SHADER_RESOURCE_VIEW_DESC v{};
    v.Format = format; v.ViewDimension = D3D11_SRV_DIMENSION_BUFFER; v.Buffer.NumElements = elements;
    ComPtr<ID3D11ShaderResourceView> view;
    hr(dev->CreateShaderResourceView(buffer.Get(), &v, &view));
    return view;
}
inline ComPtr<ID3D11ShaderResourceView> structuredView(ID3D11Device* dev, const void* data, unsigned stride, ComPtr<ID3D11Buffer>& buffer) {
    D3D11_BUFFER_DESC d{};
    d.ByteWidth = stride; d.BindFlags = D3D11_BIND_SHADER_RESOURCE; d.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED; d.StructureByteStride = stride;
    D3D11_SUBRESOURCE_DATA sd{data, 0, 0};
    hr(dev->CreateBuffer(&d, &sd, &buffer));
    D3D11_SHADER_RESOURCE_VIEW_DESC v{};
    v.Format = DXGI_FORMAT_UNKNOWN; v.ViewDimension = D3D11_SRV_DIMENSION_BUFFER; v.Buffer.NumElements = 1;
    ComPtr<ID3D11ShaderResourceView> view;
    hr(dev->CreateShaderResourceView(buffer.Get(), &v, &view));
    return view;
}

inline Resident makeResident(ID3D11Device* dev, const Draw& d, unsigned index) {
    using flatshadow_test::kW;
    using flatshadow_test::kH;
    Resident r;
    const unsigned n = static_cast<unsigned>(d.verts.size());
    check(n > 0 && n % 3 == 0, "shadow GPU scene: a draw has a whole number of triangles");
    r.vertices = n;
    std::vector<float> now(n * 4), before(n * 4);
    for (unsigned i = 0; i < n; ++i) {
        const ShadowVertex& v = d.verts[i];
        const float w = v.w;
        const bool withMotion = d.prior && d.matched && !d.badPrior;
        const float mx = withMotion ? v.mx : 0.f, my = withMotion ? v.my : 0.f;
        float* a = &now[i * 4];
        float* b = &before[i * 4];
        a[0] = v.u * w; a[1] = -v.v * w; a[2] = 0.5f; a[3] = w;
        b[0] = (v.u + 2.f * mx / static_cast<float>(kW)) * w; b[1] = -(v.v + 2.f * my / static_cast<float>(kH)) * w; b[2] = 0.5f; b[3] = w;
        if (d.invalidNow) { a[2] = -1.f; a[3] = -1.f; }          // not a valid position: the map refuses the draw
        if (d.badPrior) { b[2] = -1.f; }                          // not a usable prior
        if (d.badTriangle(i)) {
            switch (d.bad[i / 3]) {
            case 1: b[0] = (v.u + 2.f * 300.f / static_cast<float>(kW)) * w; b[1] = -v.v * w; break;
            case 2: b[0] = -b[0]; b[1] = -b[1]; b[3] = -w; break;   // the same ndc position, the other sign of w: only the w test refuses it
            case 3: b[0] = std::nanf(""); break;
            default: if (i % 3 == 1) a[2] = 0.25f; break;
            }
        }
    }
    r.now = typedView(dev, now.data(), n, DXGI_FORMAT_R32G32B32A32_FLOAT, r.buffers[0]);
    if (d.prior) r.before = typedView(dev, before.data(), n, DXGI_FORMAT_R32G32B32A32_FLOAT, r.buffers[1]);
    const uint32_t identity[4] = {d.x, d.y | (d.byte30 << 16), d.unreadable ? 0u : 1u, 0};
    r.identity = structuredView(dev, identity, 16, r.buffers[2]);
    if (d.prior) {
        const uint32_t prior[4] = {d.matched ? d.x : d.x ^ 0x1234u, d.y | (d.priorByte30 << 16), 1u, 0};
        r.priorIdentity = structuredView(dev, prior, 16, r.buffers[3]);
    }
    const uint32_t slot = 5 + index, raw = slot | (d.notAuthentic ? 0x80000000u : 0u);
    r.instance = typedView(dev, &raw, 1, DXGI_FORMAT_R32_UINT, r.buffers[4]);
    return r;
}

// The words of one frame.
struct Frame {
    std::vector<Resident> residents;
    std::vector<edvr::FlatForegroundMotion::Settings> settings;
    std::vector<edvr::FlatForegroundShadow::DrawInput> inputs;
};
inline Frame makeFrame(ID3D11Device* dev, const std::vector<Draw>& draws) {
    using flatshadow_test::kW;
    using flatshadow_test::kH;
    Frame f;
    f.residents.reserve(draws.size());
    f.settings.resize(draws.size());
    f.inputs.resize(draws.size());
    for (unsigned i = 0; i < draws.size(); ++i) f.residents.push_back(makeResident(dev, draws[i], i));
    for (unsigned i = 0; i < draws.size(); ++i) {
        edvr::FlatForegroundMotion::Settings& s = f.settings[i];
        s.extentPhase[0] = static_cast<float>(kW); s.extentPhase[1] = static_cast<float>(kH);
        s.identityMode[0] = 1; s.identityMode[1] = draws[i].prior ? 1u : 0u;
        s.sibling[1] = i; s.sibling[2] = f.residents[i].vertices; s.sibling[3] = static_cast<unsigned>(draws.size());
        edvr::FlatForegroundShadow::DrawInput& in = f.inputs[i];
        in.views[0] = f.residents[i].now.Get(); in.views[1] = f.residents[i].before.Get();
        in.views[5] = f.residents[i].identity.Get(); in.views[6] = f.residents[i].priorIdentity.Get(); in.views[10] = f.residents[i].instance.Get();
        in.constants = &s; in.constantBytes = sizeof(s); in.dispatchMoments = draws[i].prior;
    }
    return f;
}

inline std::vector<ShadowRecord> modelRecords(const std::vector<Draw>& draws) {
    std::vector<ShadowDrawRef> refs;
    for (const Draw& d : draws) refs.push_back(modelOf(d));
    return edvr::flatShadowReference(refs);
}

// Run the shadow over a frame and read its records back (waiting).
inline std::vector<ShadowRecord> runShadow(ID3D11Device* dev, ID3D11DeviceContext* ctx, edvr::FlatForegroundShadow& shadow, const std::vector<Draw>& draws, unsigned frameNumber,
                                           bool* ran = nullptr) {
    Frame frame = makeFrame(dev, draws);
    const bool ok = shadow.run(ctx, frame.inputs.data(), static_cast<unsigned>(frame.inputs.size()), frameNumber);
    if (ran) *ran = ok;
    if (!ok) return {};
    shadow.poll(ctx, frameNumber, true);
    return shadow.lastRecords();
}

// The shader's records against the model's. Returns what differs first, or an empty string. `closeness` is the model's distance to a gate threshold
// for each draw (flatshadow_test::oracle): a draw within 5e-3 of one is not held to its gate bits.
inline std::string differences(const std::vector<ShadowRecord>& gpu, const std::vector<ShadowRecord>& model, const std::vector<double>& closeness) {
    char text[300];
    if (gpu.size() != model.size()) return "record count";
    for (size_t i = 0; i < gpu.size(); ++i) {
        const ShadowRecord& g = gpu[i];
        const ShadowRecord& m = model[i];
        const auto bad = [&](const char* what) { std::snprintf(text, sizeof(text), "draw %zu: %s (shader kind %g donors %g pooled %g gates %g spread %g resid %g rmsMean %g rmsAffine %g evaluated %g own %g,%g; "
                                                                                    "model kind %g donors %g pooled %g gates %g spread %g resid %g rmsMean %g rmsAffine %g evaluated %g own %g,%g)",
                                                 i, what, g.kind, g.donorDraws, g.pooled, g.gates, g.spread, g.residual, g.rmsMean, g.rmsAffine, g.evaluated, g.ownMean[0], g.ownMean[1],
                                                 m.kind, m.donorDraws, m.pooled, m.gates, m.spread, m.residual, m.rmsMean, m.rmsAffine, m.evaluated, m.ownMean[0], m.ownMean[1]);
                                   return std::string(text); };
        // Within `absolute` px plus 1e-3 of the value: 5e-3, and 1e-2 for the fit's residual and error, which are differences of sums of squares in 32-bit float.
        const auto within = [&](float a, float b, double absolute) {
            const double d = std::fabs(static_cast<double>(a) - b);
            return d <= absolute + 1e-3 * std::fabs(static_cast<double>(b));
        };
        if (g.kind != m.kind) return bad("kind");
        const unsigned kind = static_cast<unsigned>(m.kind + 0.5f);
        if (kind == 0) continue;
        if (g.donorDraws != m.donorDraws) return bad("donor draws");
        if (kind == static_cast<unsigned>(ShadowKind::MatchedAlone) || kind == static_cast<unsigned>(ShadowKind::MatchedWithDonors)) {
            if (!within(g.ownMean[0], m.ownMean[0], 5e-3) || !within(g.ownMean[1], m.ownMean[1], 5e-3)) return bad("own mean motion");
        }
        const bool donors = kind == static_cast<unsigned>(ShadowKind::MatchedWithDonors) || kind == static_cast<unsigned>(ShadowKind::ReceiverWithDonors);
        if (!donors) {
            if (g.gates != 0 || m.gates != 0) return bad("gates of a draw with no donor");
            continue;
        }
        if (g.pooled != m.pooled) return bad("donor vertices");
        // The fit of a pool the model finds ill-conditioned is noise in 32-bit float (its solution divides by a determinant within zero): that pool's
        // fit residual, its affine error and the residual gate are not compared; the conditioning gate that refuses it is.
        const bool conditioned = (static_cast<unsigned>(m.gates) & edvr::kGateConditioning) != 0;
        if (!within(g.spread, m.spread, 5e-3) || (conditioned && !within(g.residual, m.residual, 1e-2))) return bad("spread or residual");
        if (closeness.empty() || closeness[i] >= 5e-3) {
            const unsigned mask = conditioned ? ~0u : ~static_cast<unsigned>(edvr::kGateResidual);
            if ((static_cast<unsigned>(g.gates) & mask) != (static_cast<unsigned>(m.gates) & mask)) return bad("gate bits");
        }
        if (kind == static_cast<unsigned>(ShadowKind::MatchedWithDonors)) {
            if (g.evaluated != m.evaluated) return bad("vertices compared");
            if (!within(g.rmsMean, m.rmsMean, 1e-2) || (conditioned && !within(g.rmsAffine, m.rmsAffine, 1e-2))) return bad("error of the mean or the affine model");
        }
    }
    return {};
}

inline std::vector<double> closenessOf(const std::vector<Draw>& draws) {
    std::vector<ShadowDrawRef> refs;
    for (const Draw& d : draws) refs.push_back(modelOf(d));
    std::vector<double> out;
    for (size_t i = 0; i < refs.size(); ++i) out.push_back(flatshadow_test::oracle(refs, i).close);
    return out;
}

// A scene of the CPU model's, as draws: each piece truncated to whole triangles; a piece left with none is a prior that cannot be used (or, for a
// receiver, a position that is not valid).
inline std::vector<Draw> drawsOf(const std::vector<ShadowDrawRef>& refs, bool vary30 = false) {
    std::vector<Draw> out;
    for (size_t i = 0; i < refs.size(); ++i) {
        const ShadowDrawRef& r = refs[i];
        Draw d;
        d.x = r.identityX; d.y = r.identityY; d.unreadable = !r.identityReadable;
        d.matched = r.matched; d.prior = r.matched;
        if (vary30) { d.byte30 = static_cast<unsigned>(3 * i + 1) & 0xFF; d.priorByte30 = static_cast<unsigned>(5 * i + 2) & 0xFF; }
        if (r.matched) { for (const ShadowVertex& v : r.vertices) d.verts.push_back(v); }
        else { for (const edvr::ShadowHullVertex& h : r.hull) { ShadowVertex v; v.u = h.u; v.v = h.v; v.w = h.w; d.verts.push_back(v); } }
        d.verts.resize(d.verts.size() / 3 * 3);
        if (d.verts.empty()) {
            ShadowVertex v;
            v.u = 0.f; v.v = 0.f; v.w = 1.f;
            d.verts.assign(3, v);
            d.invalidNow = true;
        }
        out.push_back(d);
    }
    return out;
}

}  // namespace flatshadow_gpu

inline void flatShadowGpuTests(ID3D11Device* dev, ID3D11DeviceContext* ctx) {
    using namespace flatshadow_gpu;
    using namespace flatshadow_test;
    using edvr::FlatForegroundShadow;
    FlatForegroundShadow shadow;
    unsigned frame = 1000;
    const Camera cam;
    const P3 roll{0, 0, 1}, yaw{0, 1, 0};

    // One scene on the device and in the model: the parity check, and the shader's records for the caller's own expectations. (Measured on WARP: the
    // largest float difference compared, over every scene below, is 0.0028 against the tolerances in differences().)
    const auto scene = [&](const char* name, const std::vector<Draw>& draws) {
        bool ran = false;
        const std::vector<ShadowRecord> gpu = runShadow(dev, ctx, shadow, draws, ++frame, &ran);
        const std::vector<ShadowRecord> model = modelRecords(draws);
        check(ran && gpu.size() == draws.size(), (std::string("shadow GPU ") + name + ": the frame runs and every draw has a record").c_str());
        const std::string why = differences(gpu, model, closenessOf(draws));
        if (!why.empty()) std::printf("shadow GPU %s: %s\n", name, why.c_str());
        check(why.empty(), (std::string("shadow GPU ") + name + ": every draw's record equals the model's (kinds, donors, gate bits exactly; numbers within float precision)").c_str());
        return gpu;
    };
    const auto kindOf = [](const ShadowRecord& r) { return static_cast<unsigned>(r.kind + 0.5f); };
    const auto missing = [](const ShadowRecord& r) { return edvr::kGateAffine & ~static_cast<unsigned>(r.gates); };
    const auto pieces = [&](const std::vector<ShadowDrawRef>& refs, bool vary30 = true) { return drawsOf(refs, vary30); };
    const auto receiver = [&](double u0, double u1, double v0, double v1, double w, uint32_t x = kIdentityX) {
        Draw d;
        d.prior = false; d.matched = false; d.x = x;
        const auto at = [&](double u, double v) { ShadowVertex s; s.u = static_cast<float>(u); s.v = static_cast<float>(v); s.w = static_cast<float>(w); d.verts.push_back(s); };
        at(u0, v0); at(u1, v0); at(u0, v1); at(u1, v0); at(u1, v1); at(u0, v1);
        return d;
    };
    const auto roll1 = gripRotation(cam, kWeapon, roll, 1.0);

    // ---- the rigid, rotating weapon: six pieces of one identity, rolled about the grip with depth relief and a drift (identity.y byte 30 differs) ----
    {
        const auto gpu = scene("rigid roll, 6 pieces", pieces(weapon(cam, kWeapon, 1.0, gripRotation(cam, kWeapon, roll, 1.0, P3{0.001, -0.0007, 0.0006}))));
        const Summary s = summarize(gpu);
        check(s.count == 6 && s.allKind && missingAffine(gpu) == 0 && s.worstAffine < 0.1 && s.maxRatio < 0.05 && !s.anyCurrentAccept && s.minSpread > 1.f,
              "shadow GPU rigid roll: on the device each piece has five donors, the affine model leaves under 0.1 px (a twentieth of the mean's error) and passes every gate, "
              "and the production policy refuses (spread over a pixel)");
        bool counts = true;
        for (const ShadowRecord& r : gpu) counts = counts && r.donorDraws == 5.f && r.pooled > 1000.f && r.evaluated >= 150.f;
        check(counts, "shadow GPU rigid roll: five donors and over a thousand donor vertices for each piece");
    }
    {   // a 3-D rotation (yaw) of a plane: a homography, not an affine field
        const auto gpu = scene("rigid yaw, 6 pieces", pieces(weapon(cam, kWeapon, 0.0, gripRotation(cam, kWeapon, yaw, 1.0))));
        const Summary s = summarize(gpu);
        check(s.count == 6 && s.worstAffine > 0.05 && s.worstAffine < 0.3 && s.maxRatio < 0.5 && missingAffine(gpu) == 0,
              "shadow GPU rigid yaw: the perspective residual (0.14 px at one degree) is on the device too, twice as good as the mean, inside the gate");
    }
    {   // the whole-screen weapon yawed two degrees: the residual gate refuses it
        const auto gpu = scene("whole-screen yaw 2 degrees", pieces(weapon(cam, kWholeScreenWeapon, 0.0, gripRotation(cam, kWholeScreenWeapon, yaw, 2.0))));
        check(missingAffine(gpu) == edvr::kGateResidual && summarize(gpu).maxResidual > 1.f, "shadow GPU whole-screen yaw: kGateResidual fails alone, with over a pixel of residual");
    }

    // ---- donors the affine model cannot explain, and donors that cannot determine it ----
    {
        auto refs = weapon(cam, kWeapon, 0.0, roll1);
        for (auto& d : refs) for (ShadowVertex& v : d.vertices) { v.mx = v.u < -0.025f ? 3.f : -3.f; v.my = 0; }
        const auto gpu = scene("step field +3/-3", pieces(refs));
        double residual = 1e9;
        for (const ShadowRecord& r : gpu) residual = std::fmin(residual, r.residual);
        check(missingAffine(gpu) == edvr::kGateResidual && residual > 1.2 && residual < 1.8, "shadow GPU step field: kGateResidual fails alone, the residual is about 1.5 px");
        for (auto& d : refs) for (ShadowVertex& v : d.vertices) v.mx = 3.f;
        check(missingAffine(scene("step field control, uniform +3", pieces(refs))) == 0, "shadow GPU step field control: without the step every gate passes");
    }
    {
        std::vector<ShadowDrawRef> refs(4, emptyDraw());
        for (int i = 0; i < 396; ++i) refs[static_cast<size_t>(i % 4)].vertices.push_back(moved(cam, -0.2 + 0.35 * i / 395.0, 0.7, 0.5, roll1));
        const auto gpu = scene("collinear donors", pieces(refs));
        check(missingAffine(gpu) == (edvr::kGateConditioning | edvr::kGateResidual), "shadow GPU collinear donors: kGateConditioning fails (and kGateResidual, unsolved), nothing else");
    }

    // ---- receivers: inside, out of hull, out of depth, and the three ways a draw has no history of its own ----
    {
        auto draws = pieces(weapon(cam, kWeapon, 0.0, roll1, false, 0.0));
        draws.push_back(receiver(-0.1, 0.1, 0.6, 0.8, 0.5));
        draws.push_back(receiver(0.7, 0.9, 0.6, 0.8, 0.5));
        draws.push_back(receiver(-0.1, 0.1, 0.6, 0.8, 0.8));
        draws.push_back(receiver(-0.1, 0.1, 0.6, 0.8, 0.45));
        draws.push_back(receiver(-0.1, 0.1, 0.6, 0.8, 0.35));
        draws.push_back(receiver(-0.1, 0.1, 0.6, 0.8, 0.6));
        draws.push_back(receiver(-0.1, 0.1, 0.6, 0.8, 0.65));
        // the hull margin: 0.05 of the donors' extent and 0.01, to either side of it
        const double mu = 0.05 * (kWeapon.u1 - kWeapon.u0) + 0.01, mv = 0.05 * (kWeapon.v1 - kWeapon.v0) + 0.01;
        draws.push_back(receiver(-0.1, kWeapon.u1 + 0.9 * mu, 0.6, 0.8, 0.5));
        draws.push_back(receiver(-0.1, kWeapon.u1 + 1.1 * mu, 0.6, 0.8, 0.5));
        draws.push_back(receiver(-0.1, 0.1, 0.6, kWeapon.v1 + 0.9 * mv, 0.5));
        draws.push_back(receiver(-0.1, 0.1, 0.6, kWeapon.v1 + 1.1 * mv, 0.5));
        draws.push_back(receiver(-0.1, 0.1, kWeapon.v0 - 1.1 * mv, 0.8, 0.5));
        draws.push_back(receiver(kWeapon.u0 - 1.1 * mu, 0.1, 0.6, 0.8, 0.5));
        const auto gpu = scene("receivers: inside, hull, depth, margins", draws);
        check(kindOf(gpu[6]) == 3 && missing(gpu[6]) == 0 && gpu[6].donorDraws == 6.f && gpu[6].rmsMean == 0.f && gpu[6].evaluated == 0.f,
              "shadow GPU receiver inside the weapon: a receiver with six donors, passes every gate, no error of its own");
        check(missing(gpu[7]) == edvr::kGateHull, "shadow GPU receiver outside the donors' extent: kGateHull fails alone");
        check(missing(gpu[8]) == edvr::kGateDepth, "shadow GPU receiver at w = 0.8 against donors at 0.5: kGateDepth fails alone");
        check(missing(gpu[9]) == 0 && missing(gpu[10]) == edvr::kGateDepth, "shadow GPU receiver at w = 0.45 passes (0.5 / 1.25 = 0.4) and at 0.35 fails kGateDepth");
        check(missing(gpu[11]) == 0 && missing(gpu[12]) == edvr::kGateDepth, "shadow GPU and at w = 0.6 passes (0.5 x 1.25 = 0.625) and at 0.65 fails kGateDepth");
        check(missing(gpu[13]) == 0 && missing(gpu[14]) == edvr::kGateHull && missing(gpu[15]) == 0 && missing(gpu[16]) == edvr::kGateHull &&
                  missing(gpu[17]) == edvr::kGateHull && missing(gpu[18]) == edvr::kGateHull,
              "shadow GPU the hull's margin: a receiver 0.9 of it past the donors' extent passes, 1.1 of it fails, on the right, below, above and on the left");
    }
    {
        auto draws = pieces(weapon(cam, kWeapon, 0.0, roll1, false, 0.0));
        Draw none = receiver(-0.1, 0.1, 0.6, 0.8, 0.5);
        Draw other = none; other.prior = true; other.matched = false;                    // a prior, of another identity
        Draw flagged = none; flagged.prior = true; flagged.matched = true; flagged.notAuthentic = true;   // a prior of its identity, but a pool row not authentic
        for (ShadowVertex& v : flagged.verts) { v.mx = 2.f; v.my = 1.f; }
        draws.push_back(none); draws.push_back(other); draws.push_back(flagged);
        const auto gpu = scene("no history: no prior, other identity, flagged", draws);
        check(kindOf(gpu[6]) == 3 && kindOf(gpu[7]) == 3 && kindOf(gpu[8]) == 3 && gpu[6].gates == gpu[7].gates && gpu[7].gates == gpu[8].gates && missing(gpu[6]) == 0,
              "shadow GPU a draw with no prior, with a prior of another identity, and with a flagged instance index is a receiver of the weapon's six donors in each case");
    }
    {
        auto draws = pieces(weapon(cam, kWeapon, 0.0, roll1));
        Draw loneMatched = draws[0];
        loneMatched.x ^= 0x40u;
        Draw loneReceiver = receiver(-0.1, 0.1, 0.6, 0.8, 0.5, kIdentityX ^ 0x80u);
        Draw dead = receiver(-0.1, 0.1, 0.6, 0.8, 0.5);
        dead.unreadable = true;
        Draw deadWithPrior = draws[1];
        deadWithPrior.unreadable = true;
        draws.push_back(loneMatched); draws.push_back(loneReceiver); draws.push_back(dead); draws.push_back(deadWithPrior);
        const auto gpu = scene("alone and unreadable", draws);
        check(kindOf(gpu[6]) == 2 && gpu[6].donorDraws == 0.f && gpu[6].gates == 0.f && gpu[6].ownMean[0] != 0.f,
              "shadow GPU a lone matched piece (an identity word of its own) is MatchedAlone: no donors, no gates, its own mean motion recorded");
        check(kindOf(gpu[7]) == 4 && gpu[7].gates == 0.f, "shadow GPU a receiver with no donor of its identity is ReceiverAlone");
        check(kindOf(gpu[8]) == 5 && kindOf(gpu[9]) == 5 && gpu[8].gates == 0.f, "shadow GPU a draw whose identity cannot be read (Identity z = 0) is IdentityUnreadable, with or without a prior");
        check(gpu[0].donorDraws == 5.f, "shadow GPU and it is no donor: the weapon's pieces still have five donors each");
    }
    {   // the identity: byte 30 is ignored, byte 31 and the bone base are not
        auto draws = pieces(weapon(cam, kWeapon, 0.0, roll1));
        draws[5].y ^= 0x01000000u;
        draws[4].x ^= 0x1u;
        const auto gpu = scene("identity words differ in byte 31 and x", draws);
        check(kindOf(gpu[5]) == 2 && kindOf(gpu[4]) == 2 && gpu[0].donorDraws == 3.f,
              "shadow GPU a piece whose identity.y differs in byte 31 and one whose bone base differs are not pooled: each alone, the other four have three donors each");
        auto same = pieces(weapon(cam, kWeapon, 0.0, roll1));
        for (size_t i = 0; i < same.size(); ++i) { same[i].byte30 = static_cast<unsigned>(40 * i + 7); same[i].priorByte30 = static_cast<unsigned>(9 * i); }
        const auto control = scene("identity byte 30 differs", same);
        check(missingAffine(control) == 0 && control[0].donorDraws == 5.f, "shadow GPU control: pieces that differ in byte 30 alone (current and prior) are one identity");
    }
    {   // a matched draw none of whose vertices the map takes
        auto draws = pieces(weapon(cam, kWeapon, 0.0, roll1));
        draws[2].badPrior = true;
        draws[3].invalidNow = true;
        const auto gpu = scene("matched draws with no vertex", draws);
        check(kindOf(gpu[2]) == 0 && kindOf(gpu[3]) == 0 && gpu[0].donorDraws == 3.f,
              "shadow GPU a draw whose prior cannot be used, and one whose positions are not valid, are kind none and no donors: the other four have three each");
    }

    // ---- the triangles the map refuses one by one: the shadow takes a vertex exactly when the donor pass does ----
    {
        auto draws = pieces(weapon(cam, kWeapon, 0.0, roll1));
        for (Draw& d : draws) d.bad.assign(d.verts.size() / 3, 0);
        draws[0].bad[5] = 1; draws[0].bad[40] = 1;     // a motion of 300 px, over the shader's limit of 256
        draws[1].bad[7] = 2;                           // a prior with a negative w
        draws[2].bad[3] = 3; draws[2].bad[4] = 3;      // a prior position that is not a number
        draws[3].bad[9] = 4; draws[3].bad[11] = 4;     // a current z that differs within the triangle
        const auto gpu = scene("refused triangles", draws);
        const size_t counts[4] = {draws[0].verts.size() - 6, draws[1].verts.size() - 3, draws[2].verts.size() - 6, draws[3].verts.size() - 6};
        bool taken = true;
        for (unsigned i = 0; i < 4; ++i) taken = taken && gpu[i].evaluated == static_cast<float>(counts[i]);
        check(taken && gpu[4].evaluated == static_cast<float>(draws[4].verts.size()),
              "shadow GPU a triangle with a motion over 256 px, a negative previous w, a prior position that is not a number or a current z that differs is left out: "
              "the pieces compare on three vertices less for each, and the shadow's donors lack them as the donor pass's do");
        check(gpu[5].pooled == static_cast<float>(draws[0].verts.size() + draws[1].verts.size() + draws[2].verts.size() + draws[3].verts.size() + draws[4].verts.size() - 21) &&
                  missingAffine(gpu) == 0,
              "shadow GPU and the sixth piece's donors are all the others' vertices less the 21 refused ones, with no gate missing");
    }

    // ---- 24 random scenes, and the table at its capacity ----
    {
        for (uint32_t seed = 1; seed <= 24; ++seed) {
            char name[40];
            std::snprintf(name, sizeof(name), "random scene %u", seed);
            scene(name, drawsOf(randomScene(seed), true));
        }
    }
    {
        const auto wide = weapon(cam, kWeapon, 0.0, roll1, false, 1.0);
        std::vector<ShadowVertex> all;
        for (const auto& d : wide) all.insert(all.end(), d.vertices.begin(), d.vertices.end());
        std::vector<Draw> draws;
        for (unsigned i = 0; i < FlatForegroundShadow::kDraws; ++i) {
            Draw d;
            d.byte30 = i & 0xFF;
            d.verts.assign(all.begin() + 6 * i, all.begin() + 6 * i + 6);
            draws.push_back(d);
        }
        const auto gpu = scene("128 draws (the table's capacity)", draws);
        bool every = gpu.size() == 128;
        for (const ShadowRecord& r : gpu) every = every && kindOf(r) == 1 && r.donorDraws == 127.f && r.pooled == 762.f && r.evaluated == 6.f;
        check(every, "shadow GPU 128 draws: the last draw's record is as good as the first's: each has 127 donors and 762 donor vertices");
        // 129 are refused, nothing dispatched
        draws.push_back(draws[0]);
        bool ran = true;
        FlatForegroundShadow refused;
        Frame f = makeFrame(dev, draws);
        ran = refused.run(ctx, f.inputs.data(), 129, ++frame);
        check(!ran, "shadow GPU 129 draws are refused");
        refused.poll(ctx, frame, true);
        check(refused.stats().sampledFrames == 0 && refused.stats().failed == 0 && refused.lastRecords().empty(), "shadow GPU a refused frame leaves no slot pending and no failure");
        check(!refused.run(ctx, f.inputs.data(), 0, ++frame) && !refused.run(nullptr, f.inputs.data(), 3, frame) && !refused.run(ctx, nullptr, 3, frame),
              "shadow GPU no draws, no context and no draw array are refused too");
        Frame f128 = makeFrame(dev, std::vector<Draw>(draws.begin(), draws.begin() + 128));
        check(refused.run(ctx, f128.inputs.data(), 128, ++frame), "shadow GPU and 128 are taken after it (the refusal took nothing)");
        refused.poll(ctx, frame, true);
        check(refused.stats().sampledFrames == 1, "shadow GPU a frame that was run is read once");
    }

    // ---- four slots ----
    {
        FlatForegroundShadow slots;
        const auto draws = pieces(weapon(cam, kWeapon, 0.0, roll1));
        Frame f = makeFrame(dev, draws);
        const unsigned n = static_cast<unsigned>(draws.size());
        bool four = true;
        for (unsigned i = 0; i < FlatForegroundShadow::kSlots; ++i) four = four && slots.run(ctx, f.inputs.data(), n, 500);
        check(four, "shadow GPU four frames fit in the four slots");
        check(!slots.run(ctx, f.inputs.data(), n, 500), "shadow GPU a fifth, while all four are pending, is refused");
        slots.poll(ctx, 500, false);
        check(!slots.run(ctx, f.inputs.data(), n, 500) && slots.stats().sampledFrames == 0, "shadow GPU a poll that does not wait leaves a slot run this very frame alone (a result is read two frames on)");
        slots.poll(ctx, 500, true);
        check(slots.stats().sampledFrames == 4 && slots.stats().notReady == 0 && slots.stats().failed == 0, "shadow GPU a poll that waits reads all four");
        check(slots.run(ctx, f.inputs.data(), n, 501), "shadow GPU and then a slot is free again");
        slots.poll(ctx, 501, true);
        check(slots.stats().sampledFrames == 5, "shadow GPU five frames sampled in all");
        // the counters are the accumulation of the records read, once per frame
        edvr::ShadowStats expected;
        const auto records = slots.lastRecords();
        for (unsigned i = 0; i < 5; ++i) edvr::flatShadowAccumulate(expected, records.data(), static_cast<unsigned>(records.size()));
        const edvr::ShadowStats& got = slots.stats();
        check(got.drawsRead == expected.drawsRead && got.byKind[1] == expected.byKind[1] && got.currentAccepts == expected.currentAccepts && got.affineAccepts == expected.affineAccepts &&
                  got.bothAccept == expected.bothAccept && got.donorDrawsSum == expected.donorDrawsSum && got.donorVerticesSum == expected.donorVerticesSum &&
                  got.byKind[1] == 30 && got.affineAccepts == 30 && got.currentAccepts == 0,
              "shadow GPU the statistics are five frames' accumulation of the records (six matched pieces each, the affine model accepting all, the production's policy none)");
    }

    // ---- the game's compute stage is put back ----
    {
        ComPtr<ID3D11ComputeShader> sentinelCs;
        const char* source = "[numthreads(1,1,1)] void main(){}";
        ComPtr<ID3DBlob> code, errors;
        hr(D3DCompile(source, std::strlen(source), "shadow sentinel", nullptr, nullptr, "main", "cs_5_0", 0, 0, &code, &errors));
        hr(dev->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &sentinelCs));
        D3D11_BUFFER_DESC sb{};
        sb.ByteWidth = 64; sb.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS; sb.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED; sb.StructureByteStride = 16;
        ComPtr<ID3D11Buffer> srvBuffer, uavBuffers[uavs], cbBuffers[cbs];
        ComPtr<ID3D11ShaderResourceView> srv[srvs];
        ComPtr<ID3D11UnorderedAccessView> uav[uavs];
        hr(dev->CreateBuffer(&sb, nullptr, &srvBuffer));
        for (unsigned i = 0; i < srvs; ++i) hr(dev->CreateShaderResourceView(srvBuffer.Get(), nullptr, &srv[i]));
        for (unsigned i = 0; i < uavs; ++i) { hr(dev->CreateBuffer(&sb, nullptr, &uavBuffers[i])); hr(dev->CreateUnorderedAccessView(uavBuffers[i].Get(), nullptr, &uav[i])); }
        D3D11_BUFFER_DESC cb{};
        cb.ByteWidth = 64; cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        for (unsigned i = 0; i < cbs; ++i) hr(dev->CreateBuffer(&cb, nullptr, &cbBuffers[i]));
        D3D11_SAMPLER_DESC sd{};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT; sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP; sd.ComparisonFunc = D3D11_COMPARISON_NEVER; sd.MaxLOD = D3D11_FLOAT32_MAX;
        ComPtr<ID3D11SamplerState> sampler;
        hr(dev->CreateSamplerState(&sd, &sampler));
        const auto hold = [&] {
            ctx->CSSetShader(sentinelCs.Get(), nullptr, 0);
            ID3D11ShaderResourceView* v[srvs];
            for (unsigned i = 0; i < srvs; ++i) v[i] = srv[i].Get();
            ctx->CSSetShaderResources(0, srvs, v);
            ID3D11UnorderedAccessView* u[uavs];
            for (unsigned i = 0; i < uavs; ++i) u[i] = uav[i].Get();
            ctx->CSSetUnorderedAccessViews(0, uavs, u, nullptr);
            ID3D11Buffer* c[cbs];
            for (unsigned i = 0; i < cbs; ++i) c[i] = cbBuffers[i].Get();
            ctx->CSSetConstantBuffers(0, cbs, c);
            ID3D11SamplerState* s = sampler.Get();
            ctx->CSSetSamplers(0, 1, &s);
        };
        const auto kept = [&] {
            ComPtr<ID3D11ComputeShader> shader;
            ctx->CSGetShader(&shader, nullptr, nullptr);
            ID3D11ShaderResourceView* v[srvs]{};
            ctx->CSGetShaderResources(0, srvs, v);
            ID3D11UnorderedAccessView* u[uavs]{};
            ctx->CSGetUnorderedAccessViews(0, uavs, u);
            ID3D11Buffer* c[cbs]{};
            ctx->CSGetConstantBuffers(0, cbs, c);
            ID3D11SamplerState* s = nullptr;
            ctx->CSGetSamplers(0, 1, &s);
            bool ok = shader.Get() == sentinelCs.Get() && s == sampler.Get();
            for (unsigned i = 0; i < srvs; ++i) { ok = ok && v[i] == srv[i].Get(); if (v[i]) v[i]->Release(); }
            for (unsigned i = 0; i < uavs; ++i) { ok = ok && u[i] == uav[i].Get(); if (u[i]) u[i]->Release(); }
            for (unsigned i = 0; i < cbs; ++i) { ok = ok && c[i] == cbBuffers[i].Get(); if (c[i]) c[i]->Release(); }
            if (s) s->Release();
            return ok;
        };
        const auto clearStage = [&] {
            ID3D11ShaderResourceView* none[srvs]{};
            ctx->CSSetShaderResources(0, srvs, none);
            ID3D11UnorderedAccessView* noUav[uavs]{};
            ctx->CSSetUnorderedAccessViews(0, uavs, noUav, nullptr);
            ID3D11Buffer* noCb[cbs]{};
            ctx->CSSetConstantBuffers(0, cbs, noCb);
            ID3D11SamplerState* noSampler = nullptr;
            ctx->CSSetSamplers(0, 1, &noSampler);
            ctx->CSSetShader(nullptr, nullptr, 0);
        };
        const auto draws = pieces(weapon(cam, kWeapon, 0.0, roll1));
        Frame f = makeFrame(dev, draws);
        FlatForegroundShadow stage;
        hold();
        check(kept(), "shadow GPU the sentinel compute stage is in place before the run");
        check(stage.run(ctx, f.inputs.data(), static_cast<unsigned>(draws.size()), 700), "shadow GPU the frame runs with the game's compute stage bound");
        check(kept(), "shadow GPU and after it the game's compute stage (shader, t0-t22, u0-u6, b0-b2, s0) is exactly as it was, by identity");
        stage.poll(ctx, 700, true);
        check(kept(), "shadow GPU and after the readback too");
        ctx->CSSetShader(nullptr, nullptr, 0);
        check(!kept(), "shadow GPU mutation control: a compute stage whose shader was lost fails the check");
        clearStage();
        check(!stage.run(ctx, f.inputs.data(), 0, 701) , "shadow GPU a refused frame binds nothing");
        hold();
        FlatForegroundShadow refused;
        check(!refused.run(ctx, f.inputs.data(), 200, 702) && kept(), "shadow GPU and leaves the stage as it found it");
        clearStage();
    }

    // ---- mutation controls: the parity check sees every field of a record, and the shaders are what produced them ----
    {
        const auto draws = pieces(weapon(cam, kWeapon, 1.0, gripRotation(cam, kWeapon, roll, 1.0, P3{0.001, -0.0007, 0.0006})));
        bool ran = false;
        const auto gpu = runShadow(dev, ctx, shadow, draws, ++frame, &ran);
        const auto model = modelRecords(draws);
        const auto closeness = closenessOf(draws);
        check(ran && differences(gpu, model, closeness).empty(), "shadow GPU controls: the unmutated scene agrees");
        struct Mutation { const char* name; void (*apply)(ShadowRecord&); };
        const Mutation mutations[] = {
            {"kind", [](ShadowRecord& r) { r.kind = 3.f; }},
            {"donor draws", [](ShadowRecord& r) { r.donorDraws += 1.f; }},
            {"mean-model error", [](ShadowRecord& r) { r.rmsMean += 0.1f; }},
            {"affine-model error (the model with the affine terms zeroed: mean only)", [](ShadowRecord& r) { r.rmsAffine = r.rmsMean; }},
            {"fit residual", [](ShadowRecord& r) { r.residual += 0.1f; }},
            {"spread", [](ShadowRecord& r) { r.spread += 0.1f; }},
            {"donor vertices", [](ShadowRecord& r) { r.pooled += 1.f; }},
            {"vertices compared", [](ShadowRecord& r) { r.evaluated += 1.f; }},
            {"own mean motion x", [](ShadowRecord& r) { r.ownMean[0] += 0.1f; }},
            {"own mean motion y", [](ShadowRecord& r) { r.ownMean[1] += 0.1f; }},
            {"gate bit 1 (min vertices)", [](ShadowRecord& r) { r.gates = static_cast<float>(static_cast<unsigned>(r.gates) ^ 1u); }},
            {"gate bit 2 (spread)", [](ShadowRecord& r) { r.gates = static_cast<float>(static_cast<unsigned>(r.gates) ^ 2u); }},
            {"gate bit 4 (fit vertices)", [](ShadowRecord& r) { r.gates = static_cast<float>(static_cast<unsigned>(r.gates) ^ 4u); }},
            {"gate bit 8 (conditioning)", [](ShadowRecord& r) { r.gates = static_cast<float>(static_cast<unsigned>(r.gates) ^ 8u); }},
            {"gate bit 16 (residual)", [](ShadowRecord& r) { r.gates = static_cast<float>(static_cast<unsigned>(r.gates) ^ 16u); }},
            {"gate bit 32 (hull)", [](ShadowRecord& r) { r.gates = static_cast<float>(static_cast<unsigned>(r.gates) ^ 32u); }},
            {"gate bit 64 (depth)", [](ShadowRecord& r) { r.gates = static_cast<float>(static_cast<unsigned>(r.gates) ^ 64u); }},
        };
        for (const Mutation& m : mutations) {
            auto mutated = model;
            m.apply(mutated[2]);
            check(!differences(gpu, mutated, closeness).empty(), (std::string("shadow GPU mutation control: an expected record with its ") + m.name + " changed fails the parity check").c_str());
        }
        // The same scene with one factor changed in the device's input: the records move to where the model puts them for that scene, not the old one.
        auto noPriors = draws;
        for (Draw& d : noPriors) { d.prior = false; d.matched = false; }
        const auto gpuNone = runShadow(dev, ctx, shadow, noPriors, ++frame, &ran);
        bool alone = ran && gpuNone.size() == draws.size();
        for (const ShadowRecord& r : gpuNone) alone = alone && kindOf(r) == 4;
        check(alone && !differences(gpuNone, model, closeness).empty() && differences(gpuNone, modelRecords(noPriors), closenessOf(noPriors)).empty(),
              "shadow GPU control: the same pieces with no prior bound are six receivers alone on the device, which is not what the matched scene's model says and is what its own says");
        Frame f = makeFrame(dev, draws);
        for (auto& in : f.inputs) in.dispatchMoments = false;
        FlatForegroundShadow noMoments;
        check(noMoments.run(ctx, f.inputs.data(), static_cast<unsigned>(draws.size()), 800), "shadow GPU control: a frame whose first shader never ran");
        noMoments.poll(ctx, 800, true);
        bool dead = noMoments.lastRecords().size() == draws.size();
        for (const ShadowRecord& r : noMoments.lastRecords()) dead = dead && kindOf(r) == 4;
        check(dead && !differences(noMoments.lastRecords(), model, closeness).empty(),
              "shadow GPU control: without the first shader's moments every draw is a receiver alone: the second shader reads the first's table, not the buffers");
    }
}

// ---- the shadow as the adapter drives it: the real FlatForegroundMotion over real WARP draws (the scenes of flat_sibling_tests.h, S1's pair of pieces) ----
// The adapter hands the shadow the views the donor pass binds (the draw's positions and identity, its priors', the retained instance index) and the
// map's constants; the sibling pass's tables, read back from the same draws, are the cross-check: the shadow's donor draws and vertices are the fit
// table's, and a matched draw's own mean motion is the donor pass's mean.
template<class Raster>
void flatShadowAdapterTests(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* owners, ID3D11ShaderResourceView* depth, unsigned width, unsigned height,
                            edvr::FlatForegroundMotion::Inputs inputs, const float world[6][4], ID3D11Buffer* sceneIb, ID3D11Buffer* pool, unsigned (&poolData)[12 * 84], Raster raster) {
    using edvr::FlatForegroundMotion;
    inputs.gpuIdentity = true; inputs.identity = {}; inputs.certificate = {}; inputs.phaseX = inputs.phaseY = 0;
    const auto place = [](float centre, float projection) { Pose p{}; p.projection = projection; p.mouse = centre / projection; return p; };
    std::vector<UINT> lodIndices;
    for (unsigned k = 0; k < 8; ++k) for (UINT i : {0u, 1u, 2u, 0u, 2u, 3u, 0u, 0u, 0u}) lodIndices.push_back(i);
    D3D11_BUFFER_DESC ibd{};
    ibd.ByteWidth = UINT(lodIndices.size() * 4); ibd.Usage = D3D11_USAGE_DEFAULT; ibd.BindFlags = D3D11_BIND_INDEX_BUFFER;
    D3D11_SUBRESOURCE_DATA ibi{lodIndices.data(), 0, 0};
    Microsoft::WRL::ComPtr<ID3D11Buffer> lodIb;
    hr(dev->CreateBuffer(&ibd, &ibi, &lodIb));
    unsigned rowWords[12][2];
    for (unsigned r = 0; r < 12; ++r) { rowWords[r][0] = poolData[r * 84]; rowWords[r][1] = poolData[r * 84 + 7]; }
    poolData[84] = poolData[0]; poolData[84 + 7] = poolData[7];        // pool row 1 takes row 0's identity words: the two pieces are one object
    ctx->UpdateSubresource(pool, 0, nullptr, poolData, 0, 0);
    const auto piece = [&](FlatForegroundMotion& motion, const Pose& pose, unsigned row, unsigned frame, bool first, unsigned count, unsigned start) {
        const unsigned token = row + 1;
        raster(pose, row, token, first); inputs.writerToken = token;
        ctx->IASetIndexBuffer(lodIb.Get(), DXGI_FORMAT_R32_UINT, 0);
        const bool ok = motion.capture(ctx, issue, count, 1, start, 0, 0, frame, inputs);
        ctx->IASetIndexBuffer(sceneIb, DXGI_FORMAT_R32_UINT, 0);
        return ok;
    };
    const auto qualify = [&](FlatForegroundMotion& motion, unsigned frame, FlatForegroundMotion::Output& out) { return motion.prepareH(ctx, owners, depth, world, frame, width, height, out); };
    const auto drained = [&](FlatForegroundMotion& motion, unsigned frame) { motion.rigShadow().poll(ctx, frame + 8, true); return motion.stats().shadow; };
    const float pr = .3f;
    const unsigned f = 7000;   // a multiple of 8: the default interval samples it
    FlatForegroundMotion motion;
    FlatForegroundMotion::Output out;
    const Pose body = place(-.45f, pr), detail = place(.05f, pr);
    Pose bodyNow = body, detailNow = detail;
    bodyNow.mouse += .1f; detailNow.mouse += .1f;
    {
        FlatForegroundMotion::RigShadowEvery every(1);
        // the seed frame: two draws with no history
        piece(motion, body, 0, f, true, 9, 0); piece(motion, detail, 1, f, false, 9, 9);
        check(qualify(motion, f, out) && out.qualified, "shadow adapter: the seed frame qualifies");
        const edvr::ShadowStats seed = drained(motion, f);
        const auto& seedRecords = motion.rigShadow().lastRecords();
        check(seed.sampledFrames == 1 && seed.drawsRead == 2 && seed.byKind[4] == 2 && seedRecords.size() == 2 && seed.modeTwoFires == 2 && seed.movingFrames == 0,
              "shadow adapter: the seed frame was sampled (interval one): two draws read, both receivers with no donor of their identity (the production's view-attached case)");
        // the next frame: the body matched; the detail was swapped for another level of detail and has no history of its own
        piece(motion, bodyNow, 0, f + 1, true, 9, 0);
        piece(motion, detailNow, 1, f + 1, false, 6, 9);
        check(qualify(motion, f + 1, out) && out.qualified && motion.siblingRan(), "shadow adapter: the next frame qualifies, and the sibling pass ran for it");
        const edvr::ShadowStats now = drained(motion, f + 1);
        const std::vector<edvr::ShadowRecord> records = motion.rigShadow().lastRecords();
        check(now.sampledFrames == 2 && now.drawsRead == 4 && records.size() == 2, "shadow adapter: the second frame was sampled too, two more draws read");
        std::vector<edvr::SiblingDonor> donors;
        std::vector<std::array<float, 8>> fit;
        unsigned n = 0;
        const bool got = motion.readSiblingTables(ctx, donors, fit, n);
        check(got && n == 2 && donors[0].matched && !donors[1].matched, "shadow adapter: the sibling pass's tables say the body matched and the detail did not");
        if (got && n == 2 && records.size() == 2) {
            const edvr::ShadowRecord& a = records[0];
            const edvr::ShadowRecord& b = records[1];
            check(static_cast<unsigned>(a.kind + 0.5f) == 2 && a.donorDraws == 0.f && a.gates == 0.f,
                  "shadow adapter: the body is a matched draw alone (no other piece matched)");
            check(std::fabs(a.ownMean[0] - donors[0].mean[0]) < 1e-3f && std::fabs(a.ownMean[1] - donors[0].mean[1]) < 1e-3f && std::fabs(a.ownMean[0]) > .5f,
                  "shadow adapter: its own mean motion is the donor pass's mean, real motion (the same vertices through the same decision)");
            check(static_cast<unsigned>(b.kind + 0.5f) == 3 && b.donorDraws == static_cast<float>(fit[1][4]) && b.donorDraws == 1.f && b.pooled == static_cast<float>(donors[0].vertices) &&
                      b.pooled == fit[1][5] && b.evaluated == 0.f,
                  "shadow adapter: the swapped piece is a receiver with one donor, whose vertex count is the fit table's (the shadow reads the same draws the sibling pass does)");
            check(std::fabs(b.spread - fit[1][3]) < 1e-3f, "shadow adapter: and its donors' spread is the fit table's");
            check(now.byKind[2] == 1 && now.byKind[3] == 1 && now.receiverCurrentAccepts + now.receiverCurrentRefuses == 1,
                  "shadow adapter: the statistics count a matched draw alone and a receiver with donors, and file the receiver under the production's verdict");
        }
        // no shadow when the frame has one draw: there is nothing to pool
        piece(motion, bodyNow, 0, f + 2, true, 9, 0);
        check(qualify(motion, f + 2, out), "shadow adapter: a frame with one draw qualifies");
        check(drained(motion, f + 2).sampledFrames == 2, "shadow adapter: and is not sampled (fewer than two draws)");
    }
    {
        FlatForegroundMotion::RigShadowEvery off(0);
        piece(motion, bodyNow, 0, f + 3, true, 9, 0); piece(motion, detailNow, 1, f + 3, false, 6, 9);
        check(qualify(motion, f + 3, out), "shadow adapter: a frame qualifies with the shadow switched off");
        check(drained(motion, f + 3).sampledFrames == 2, "shadow adapter: RigShadowEvery(0) turns the sampling off: no frame is taken");
    }
    {
        FlatForegroundMotion::RigShadowEvery eight(8);
        for (unsigned frame = f + 8; frame < f + 8 + 8; ++frame) {
            piece(motion, bodyNow, 0, frame, true, 9, 0); piece(motion, detailNow, 1, frame, false, 6, 9);
            check(qualify(motion, frame, out), "shadow adapter: a frame qualifies at the default interval");
        }
        check(drained(motion, f + 15).sampledFrames == 3, "shadow adapter: at the default interval one frame in eight is sampled: of eight frames from a multiple of 8, one");
    }
    {
        // no override in force: the constant's interval, eight. A multiple of four that is not a multiple of eight is skipped, a multiple of eight is taken.
        piece(motion, bodyNow, 0, f + 12, true, 9, 0); piece(motion, detailNow, 1, f + 12, false, 6, 9);
        check(qualify(motion, f + 12, out), "shadow adapter: a frame at a multiple of four qualifies");
        check(drained(motion, f + 12).sampledFrames == 3, "shadow adapter: and is not sampled with no override in force (a multiple of four, not of eight)");
        piece(motion, bodyNow, 0, f + 16, true, 9, 0); piece(motion, detailNow, 1, f + 16, false, 6, 9);
        check(qualify(motion, f + 16, out), "shadow adapter: the frame at the next multiple of eight qualifies");
        check(drained(motion, f + 16).sampledFrames == 4, "shadow adapter: and is sampled with no override in force (kFlatShadowEveryFrames = 8)");
    }
    {   // a receiver listed before its donors: the frame's draw count is what lets the second shader scan past the receiver's own index
        FlatForegroundMotion::RigShadowEvery every(1);
        poolData[2 * 84] = poolData[0]; poolData[2 * 84 + 7] = poolData[7];
        ctx->UpdateSubresource(pool, 0, nullptr, poolData, 0, 0);
        FlatForegroundMotion three;
        const Pose a = place(-.45f, pr), b = place(.05f, pr), c = place(.55f, pr);
        Pose a2 = a, b2 = b, c2 = c;
        a2.mouse += .1f; b2.mouse += .1f; c2.mouse += .1f;
        const unsigned g = f + 24;
        piece(three, a, 0, g, true, 9, 0); piece(three, b, 1, g, false, 9, 9); piece(three, c, 2, g, false, 9, 18);
        check(qualify(three, g, out), "shadow adapter: three pieces of one object, the seed frame qualifies");
        const uint64_t before = drained(three, g).sampledFrames;
        piece(three, b2, 1, g + 1, true, 6, 9);    // swapped for another level of detail, and listed first
        piece(three, a2, 0, g + 1, false, 9, 0);
        piece(three, c2, 2, g + 1, false, 9, 18);
        check(qualify(three, g + 1, out) && out.qualified, "shadow adapter: the next frame qualifies");
        const edvr::ShadowStats now = drained(three, g + 1);
        const std::vector<edvr::ShadowRecord> records = three.rigShadow().lastRecords();
        std::vector<edvr::SiblingDonor> donors;
        std::vector<std::array<float, 8>> fit;
        unsigned n = 0;
        const bool got = three.readSiblingTables(ctx, donors, fit, n);
        check(now.sampledFrames == before + 1 && records.size() == 3 && got && n == 3 && !donors[0].matched && donors[1].matched && donors[2].matched,
              "shadow adapter: the next frame was sampled; the first draw (the swapped piece) did not match, the other two did");
        if (records.size() == 3 && got && n == 3) {
            check(static_cast<unsigned>(records[0].kind + 0.5f) == 3 && records[0].donorDraws == 2.f && records[0].donorDraws == fit[0][4] &&
                      records[0].pooled == static_cast<float>(donors[1].vertices + donors[2].vertices) && records[0].pooled == fit[0][5],
                  "shadow adapter: the receiver listed first has the two pieces listed after it as donors, with the fit table's vertex count (the frame's draw count reaches past its own index)");
            check(static_cast<unsigned>(records[1].kind + 0.5f) == 1 && static_cast<unsigned>(records[2].kind + 0.5f) == 1 && records[1].donorDraws == 1.f && records[2].donorDraws == 1.f &&
                      std::fabs(records[1].ownMean[0] - donors[1].mean[0]) < 1e-3f && std::fabs(records[2].ownMean[0] - donors[2].mean[0]) < 1e-3f,
                  "shadow adapter: each matched piece is predicted from the other (matched with donors, leave-one-out), its own mean motion the donor pass's");
        }
    }
    for (unsigned r = 0; r < 12; ++r) { poolData[r * 84] = rowWords[r][0]; poolData[r * 84 + 7] = rowWords[r][1]; }
    ctx->UpdateSubresource(pool, 0, nullptr, poolData, 0, 0);
}
