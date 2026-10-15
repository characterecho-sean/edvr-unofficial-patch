// Planet patch motion in the flat profile's prep (celestial_motion.h; celestialBefore in flat_mono_shader_source.h) and the tangents it reads
// (flat_celestial.h). The CPU half pins flatCelestialTangents on symmetric rows and on the rows it must refuse. The GPU half dispatches the
// shipped prep on WARP with one planet record bound at t18 (debug.w bit 3): a pixel inside the record takes the body's own motion, the
// projection of its previous point; a pixel outside takes the camera term; a body whose previous point is behind the camera is refused;
// and with the bit clear and the record bound the output is the output with no record at all. Registered in flat_mono_resolve_test.cpp's
// kUnits as "flatCelestialGpuTests".
#pragma once

#include "../../src/d3d11/flat_celestial.h"
#include "../../src/d3d11/flat_mono_shader_source.h"
#include "temporal_shader_bytecode.h"   // kEngineMotionCoreHlsl, generated into build\gen
#include <cmath>
#include <cstring>
#include <vector>

namespace flatcelestial {
using edvr::flatCelestialTangents;

constexpr UINT kSize = 64;             // the square target, and the render size the prep dispatches over
constexpr UINT kLeft = kSize / 2;      // the record's box: the pixels with x < kLeft (the left half)
constexpr float kNear = .025f;         // the reversed-Z near plane the rows carry (rows[3][2], as camera() sets it)
constexpr double kDepth = 500.0;       // metres: every pixel of the depth texture is at this range (depth = near / z)
constexpr float kSx = 1.2f, kSy = 2.1f;   // the rows' scales: the tangents are +/-1/kSx and +/-1/kSy

// flat_mono_resolve.cpp's Constants: the 336 bytes the prep's b0 reads (that file's static_assert pins the size). Mirrored here, because the
// rig drives the prep directly.
struct MonoConstants {
    float camera[6][4], previous[6][4];
    uint32_t size[4], flags[4];
    float jitter[4], rowsJitter[4];
    uint32_t route[4], debug[4];
    float foregroundDepth[4];
    float celestialTanNow[4], celestialTanPrev[4];
};
static_assert(sizeof(MonoConstants) == 336, "flat_mono_resolve.cpp's Constants (the prep's b0)");

// The engine-motion core's CelestialRecord (flat_mono_shader_source.h reads it at t18): six float4s.
struct Record {
    float m0[4], m1[4], m2[4], box[4], span[4], ctr[4];
};
static_assert(sizeof(Record) == 96, "CelestialRecord: six float4s");

const float kIdentity[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};

// R = Rz(az) * Ry(ay) * Rx(ax), rows out[k] = the k-th axis of the rotated frame.
void rotation(float (&out)[3][3], double ax, double ay, double az) {
    const double cx = std::cos(ax), sxa = std::sin(ax), cy = std::cos(ay), sya = std::sin(ay), cz = std::cos(az), sza = std::sin(az);
    const double rx[3][3] = {{1, 0, 0}, {0, cx, -sxa}, {0, sxa, cx}};
    const double ry[3][3] = {{cy, 0, sya}, {0, 1, 0}, {-sya, 0, cy}};
    const double rz[3][3] = {{cz, -sza, 0}, {sza, cz, 0}, {0, 0, 1}};
    double m[3][3]{}, r[3][3]{};
    for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) for (int k = 0; k < 3; ++k) m[i][j] += rz[i][k] * ry[k][j];
    for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) for (int k = 0; k < 3; ++k) r[i][j] += m[i][k] * rx[k][j];
    for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) out[i][j] = static_cast<float>(r[i][j]);
}

// The rows of a symmetric perspective turned by rot (rows Rx, Ry, Rz): clip.x = sx * (Rx . p), clip.y = sy * (Ry . p), clip.z = near,
// clip.w = -(Rz . p). The shape camera() builds, with a rotation. Input component c of rows[c] is the coefficient of p[c].
void symmetricRows(float (&rows)[6][4], float sx, float sy, const float (&rot)[3][3]) {
    std::memset(rows, 0, sizeof(rows));
    for (int c = 0; c < 3; ++c) {
        rows[c][0] = sx * rot[0][c];
        rows[c][1] = sy * rot[1][c];
        rows[c][3] = -rot[2][c];
    }
    rows[3][2] = kNear;
}

bool near4(const float a[4], const float b[4], float tolerance) {
    for (int i = 0; i < 4; ++i) if (!(std::fabs(a[i] - b[i]) <= tolerance)) return false;
    return true;
}

// ---- the CPU half: the tangents ----------------------------------------------------------------------------------------------------------
void tangentChecks() {
    const float want[4] = {-1.0f / kSx, 1.0f / kSx, -1.0f / kSy, 1.0f / kSy};
    float turned[3][3];
    // Symmetric rows, the identity and two rotations: the tangents are the scales' reciprocals, to 1e-6.
    {
        const double angles[][3] = {{0, 0, 0}, {.3, -.7, 1.1}, {-1.2, .5, 2.6}};
        bool all = true;
        for (const auto& a : angles) {
            rotation(turned, a[0], a[1], a[2]);
            float rows[6][4], out[4] = {};
            symmetricRows(rows, kSx, kSy, turned);
            all = all && flatCelestialTangents(rows, 0, 0, out) && near4(out, want, 1e-6f);
        }
        check(all, "flat celestial: symmetric rows (identity and two rotations) give the tangents -1/sx, 1/sx, -1/sy, 1/sy to 1e-6");
    }
    // The phase the rows carry (x += ndcX*w, y += ndcY*w): the same tangents once the phase is passed back, and different ones without it.
    {
        const float ndcX = .25f, ndcY = -.125f;
        rotation(turned, .3, -.7, 1.1);
        float clean[6][4], phased[6][4], withPhase[4] = {}, plain[4] = {}, unpassed[4] = {};
        symmetricRows(clean, kSx, kSy, turned);
        std::memcpy(phased, clean, sizeof(clean));
        for (int c = 0; c < 3; ++c) {
            phased[c][0] += ndcX * phased[c][3];
            phased[c][1] += ndcY * phased[c][3];
        }
        const bool okPhased = flatCelestialTangents(phased, ndcX, ndcY, withPhase);
        const bool okClean = flatCelestialTangents(clean, 0, 0, plain);
        check(okPhased && okClean && near4(withPhase, plain, 1e-6f),
              "flat celestial: rows carrying a raster phase give the same tangents as the unjittered rows once the phase is passed (1e-6)");
        const bool okUnpassed = flatCelestialTangents(phased, 0, 0, unpassed);
        check(!okUnpassed || !near4(unpassed, plain, 1e-3f), "flat celestial: the phase changes the tangents when it is not removed (the check above has teeth)");
    }
    // Rows the tangents must not come from.
    {
        float rows[6][4], out[4] = {}, wl = 0;
        symmetricRows(rows, kSx, kSy, kIdentity);
        for (int c = 0; c < 3; ++c) rows[c][3] *= 1.1f;   // a w row of length 1.1: not a unit view direction
        const bool ok = flatCelestialTangents(rows, 0, 0, out, &wl, nullptr);
        check(!ok && std::fabs(wl - 1.1f) <= 1e-5f, "flat celestial: a w row of length 1.1 is refused, and the measured length is 1.1");
    }
    {
        float a[6][4], b[6][4], out[4] = {};
        symmetricRows(a, kSx, kSy, kIdentity);
        a[2][0] += .3f;   // the x row picks up a z term: not orthogonal to the w row
        symmetricRows(b, kSx, kSy, kIdentity);
        b[2][1] += .2f;   // the y row picks up a z term
        check(!flatCelestialTangents(a, 0, 0, out) && !flatCelestialTangents(b, 0, 0, out),
              "flat celestial: rows whose x or y row is not orthogonal to the w row (an off-centre term) are refused");
    }
    {
        float rows[6][4], out[4] = {};
        symmetricRows(rows, kSx, kSy, kIdentity);
        rows[0][0] = rows[1][0] = rows[2][0] = 0;   // a zero x row
        check(!flatCelestialTangents(rows, 0, 0, out), "flat celestial: a zero x row is refused");
    }
}

// ---- the GPU half: the shipped prep on WARP ----------------------------------------------------------------------------------------------
// The prep the shipped source makes, compiled the way flat_first_person_gpu_tests.h compiles it (the engine-motion core in front).
ComPtr<ID3D11ComputeShader> compilePrep(ID3D11Device* device) {
    const std::string full = std::string(edvr::kEngineMotionCoreHlsl) + std::string(edvr::kFlatMonoShaderSource);
    ComPtr<ID3DBlob> code, errors;
    const HRESULT hr = D3DCompile(full.c_str(), full.size(), "flat_celestial_prep_cs", nullptr, nullptr, "prep", "cs_5_0", 0, 0,
                                  code.GetAddressOf(), errors.GetAddressOf());
    if (FAILED(hr) || !code) {
        std::printf("info: prep compile failed: %s\n", errors ? static_cast<const char*>(errors->GetBufferPointer()) : "no message");
        check(false, "flat celestial: the shipped prep compiles");
        return nullptr;
    }
    ComPtr<ID3D11ComputeShader> shader;
    check(SUCCEEDED(device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, shader.GetAddressOf())),
          "flat celestial: the shipped prep compiles and makes a compute shader");
    return shader;
}

// Per pixel, the prep's motion (x, y, in render pixels) and rejection, read back whole.
struct Output {
    bool ok = false;
    std::vector<float> motion, rejection;
};

// The records buffer: `count` CelestialRecords, the SRV the prep's t18 reads.
ComPtr<ID3D11ShaderResourceView> recordsView(ID3D11Device* device, const std::vector<Record>& records) {
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = static_cast<UINT>(sizeof(Record) * records.size());
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    bd.StructureByteStride = sizeof(Record);
    D3D11_SUBRESOURCE_DATA init{records.data(), 0, 0};
    ComPtr<ID3D11Buffer> buffer;
    check(SUCCEEDED(device->CreateBuffer(&bd, &init, buffer.GetAddressOf())), "flat celestial: the record buffer");
    return view(device, buffer.Get());
}

// The fixed inputs of every dispatch: the engine views (complete, no keyed slot, an empty pool, the fixture's engine camera buffers), a
// depth texture at kDepth everywhere, and a colour target the prep does not read.
struct Harness {
    ID3D11Device* device;
    ID3D11DeviceContext* context;
    ResolveFixture& fx;
    ComPtr<ID3D11ComputeShader> prep;
    ComPtr<ID3D11ShaderResourceView> colour, depth, slots;
    Harness(ID3D11Device* d, ID3D11DeviceContext* c, ResolveFixture& f) : device(d), context(c), fx(f) {}
};

Output run(Harness& h, const MonoConstants& mono, ID3D11ShaderResourceView* records) {
    Output out;
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = sizeof(MonoConstants);
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA init{&mono, 0, 0};
    ComPtr<ID3D11Buffer> cb;
    check(SUCCEEDED(h.device->CreateBuffer(&bd, &init, cb.GetAddressOf())), "flat celestial: the Mono constants buffer");
    // The prep's outputs: OutDepth (u0), OutMotion (u1), OutRejection (u2), OutExpected (u3).
    ComPtr<ID3D11Texture2D> depthOut = texture(h.device, kSize, kSize, DXGI_FORMAT_R32_FLOAT, D3D11_BIND_UNORDERED_ACCESS);
    ComPtr<ID3D11Texture2D> motionOut = texture(h.device, kSize, kSize, DXGI_FORMAT_R32G32_FLOAT, D3D11_BIND_UNORDERED_ACCESS);
    ComPtr<ID3D11Texture2D> rejectionOut = texture(h.device, kSize, kSize, DXGI_FORMAT_R32_FLOAT, D3D11_BIND_UNORDERED_ACCESS);
    ComPtr<ID3D11Texture2D> expectedOut = texture(h.device, kSize, kSize, DXGI_FORMAT_R32_FLOAT, D3D11_BIND_UNORDERED_ACCESS);
    ComPtr<ID3D11UnorderedAccessView> uavs[4];
    ID3D11Texture2D* targets[4] = {depthOut.Get(), motionOut.Get(), rejectionOut.Get(), expectedOut.Get()};
    for (int k = 0; k < 4; ++k)
        check(SUCCEEDED(h.device->CreateUnorderedAccessView(targets[k], nullptr, uavs[k].GetAddressOf())), "flat celestial: an output view");
    // The inputs: t0 colour, t1 depth, t2 slots, t3 pool, t18 the records; the rest is null, as the resolver leaves it for this frame.
    ID3D11ShaderResourceView* srvs[19] = {};
    srvs[0] = h.colour.Get();
    srvs[1] = h.depth.Get();
    srvs[2] = h.slots.Get();
    srvs[3] = h.fx.poolView.Get();
    srvs[18] = records;
    ID3D11Buffer* cbs[3] = {cb.Get(), h.fx.now.Get(), h.fx.old.Get()};
    ID3D11UnorderedAccessView* uavRaw[4] = {uavs[0].Get(), uavs[1].Get(), uavs[2].Get(), uavs[3].Get()};
    h.context->CSSetShader(h.prep.Get(), nullptr, 0);
    h.context->CSSetConstantBuffers(0, 3, cbs);
    h.context->CSSetShaderResources(0, 19, srvs);
    h.context->CSSetUnorderedAccessViews(0, 4, uavRaw, nullptr);
    h.context->Dispatch(kSize / 8, kSize / 8, 1);
    ID3D11ShaderResourceView* nullSrvs[19] = {};
    ID3D11UnorderedAccessView* nullUavs[4] = {};
    h.context->CSSetShaderResources(0, 19, nullSrvs);
    h.context->CSSetUnorderedAccessViews(0, 4, nullUavs, nullptr);
    h.context->CSSetShader(nullptr, nullptr, 0);
    std::vector<unsigned char> bytes;
    if (!readWhole(h.context, motionOut.Get(), bytes, 8) || bytes.size() != size_t(kSize) * kSize * 8) return out;
    out.motion.resize(size_t(kSize) * kSize * 2);
    std::memcpy(out.motion.data(), bytes.data(), bytes.size());
    if (!readWhole(h.context, rejectionOut.Get(), bytes, 4) || bytes.size() != size_t(kSize) * kSize * 4) return out;
    out.rejection.resize(size_t(kSize) * kSize);
    std::memcpy(out.rejection.data(), bytes.data(), bytes.size());
    out.ok = true;
    return out;
}

// The previous uv of the point a pixel at (u, v) sees on a body whose record says last frame's point is the current one plus t (metres,
// identity rotation): P is the pixel's view-space point at kDepth, dp = P + t is last frame's, projected through the previous tangents tp.
// False when that point is behind the camera (the prep refuses it).
bool previousUv(double u, double v, const float tn[4], const float tp[4], const double t[3], double out[2]) {
    const double px = (tn[0] + u * (tn[1] - tn[0])) * kDepth, py = (tn[3] - v * (tn[3] - tn[2])) * kDepth;
    const double dx = px + t[0], dy = py + t[1], dz = -kDepth + t[2];
    const double w = -dz;
    if (!(w > 0)) return false;
    out[0] = (dx / w - tp[0]) / (tp[1] - tp[0]);
    out[1] = (tp[3] - dy / w) / (tp[3] - tp[2]);
    return true;
}

// One record for the body: identity rotation, translation t, the box over the left half, the depth span [100, 1000] m, a flat body (span.w 0).
// The second record is all zero, span.z 0: the end of the list.
std::vector<Record> bodyRecords(const float t[3]) {
    std::vector<Record> records(16);
    Record& b = records[0];
    b.m0[0] = 1; b.m0[3] = t[0];
    b.m1[1] = 1; b.m1[3] = t[1];
    b.m2[2] = 1; b.m2[3] = t[2];
    b.box[0] = -.5f; b.box[1] = -.5f; b.box[2] = kLeft - .5f; b.box[3] = kSize - .5f;   // pixel indices 0..kLeft-1 in x, every row
    b.span[0] = 100; b.span[1] = 1000; b.span[2] = 1; b.span[3] = 0;
    return records;
}

void gpuTests(ID3D11Device* device, ID3D11DeviceContext* context) {
    ResolveFixture fx(device, context);   // the engine camera buffers and the empty pool, as the world route's cases have them
    Harness h(device, context, fx);
    h.prep = compilePrep(device);
    if (!h.prep) return;
    // Every pixel at the same range; the slot at every pixel is -1 (no keyed record: the engine term is the camera term's), the pool empty.
    std::vector<float> slots(size_t(kSize) * kSize * 2);
    for (size_t i = 0; i < slots.size(); i += 2) { slots[i] = -1; slots[i + 1] = .01f; }
    h.slots = view(device, texture(device, kSize, kSize, DXGI_FORMAT_R32G32_FLOAT, D3D11_BIND_SHADER_RESOURCE, slots.data(), kSize * 8).Get());
    std::vector<float> depth(size_t(kSize) * kSize, kNear / static_cast<float>(kDepth));
    h.depth = view(device, texture(device, kSize, kSize, DXGI_FORMAT_R32_FLOAT, D3D11_BIND_SHADER_RESOURCE, depth.data(), kSize * 4).Get());
    h.colour = view(device, texture(device, kSize, kSize, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE).Get());

    // The still camera: the same symmetric rows now and before, no translation, no phase, the origin at zero.
    float rows[6][4];
    symmetricRows(rows, kSx, kSy, kIdentity);
    MonoConstants base{};
    std::memcpy(base.camera, rows, sizeof(rows));
    std::memcpy(base.previous, rows, sizeof(rows));
    base.size[0] = base.size[1] = base.size[2] = base.size[3] = kSize;
    base.flags[1] = 1;   // complete engine views
    base.foregroundDepth[0] = 1;
    float tanNow[4] = {}, tanPrev[4] = {};
    flatCelestialTangents(rows, 0, 0, tanNow);
    flatCelestialTangents(rows, 0, 0, tanPrev);
    MonoConstants on = base;   // the planet records bound: debug.w bit 3, the tangents in the constants
    on.debug[3] = 8;
    std::memcpy(on.celestialTanNow, tanNow, sizeof(tanNow));
    std::memcpy(on.celestialTanPrev, tanPrev, sizeof(tanPrev));

    // 1. A body straight across the left half, last frame 3, -2, -5 m away (it was 5 m farther and off to the side).
    {
        const float t[3] = {3, -2, -5};
        const double td[3] = {3, -2, -5};
        const auto records = bodyRecords(t);
        const auto srv = recordsView(device, records);
        const Output out = run(h, on, srv.Get());
        bool analytic = out.ok, accepted = out.ok, moved = out.ok, rightCamera = out.ok, rightAccepted = out.ok;
        for (UINT y = 0; out.ok && y < kSize; ++y) for (UINT x = 0; x < kSize; ++x) {
            const size_t i = size_t(y) * kSize + x;
            const double u = (x + .5) / kSize, v = (y + .5) / kSize;
            const float mx = out.motion[2 * i], my = out.motion[2 * i + 1];
            if (x < kLeft) {
                double prev[2] = {};
                const bool ok = previousUv(u, v, tanNow, tanPrev, td, prev);
                const double ex = (prev[0] - u) * kSize, ey = (prev[1] - v) * kSize;
                analytic = analytic && ok && std::fabs(mx - ex) <= 1e-3 && std::fabs(my - ey) <= 1e-3;
                accepted = accepted && out.rejection[i] == 0;
                moved = moved && (std::fabs(ex) > .01 || std::fabs(ey) > .01);
            } else {
                rightCamera = rightCamera && std::fabs(mx) <= 1e-3f && std::fabs(my) <= 1e-3f;
                rightAccepted = rightAccepted && out.rejection[i] == 0;
            }
        }
        check(analytic, "flat celestial: a pixel inside the body takes the projection of its previous point (1e-3 px)");
        check(accepted, "flat celestial: a pixel inside the body is accepted (rejection 0)");
        check(moved, "flat celestial: the analytic motion inside the body is more than 0.01 px at every pixel (the precondition: the camera term's zero would fail the check above)");
        check(rightCamera, "flat celestial: a pixel outside the body takes the camera term (zero motion, the rows did not move)");
        check(rightAccepted, "flat celestial: a pixel outside the body is accepted (rejection 0)");
    }
    // 2. The same body with its previous point behind the camera (t = 600 m towards the camera): the left half is refused, the right is not.
    {
        const float t[3] = {0, 0, 600};
        const auto records = bodyRecords(t);
        const auto srv = recordsView(device, records);
        const Output out = run(h, on, srv.Get());
        bool refused = out.ok, still = out.ok, rightAccepted = out.ok;
        for (UINT y = 0; out.ok && y < kSize; ++y) for (UINT x = 0; x < kSize; ++x) {
            const size_t i = size_t(y) * kSize + x;
            if (x < kLeft) {
                refused = refused && out.rejection[i] == 1 && out.motion[2 * i] == 0 && out.motion[2 * i + 1] == 0;
            } else {
                still = still && std::fabs(out.motion[2 * i]) <= 1e-3f && std::fabs(out.motion[2 * i + 1]) <= 1e-3f;
                rightAccepted = rightAccepted && out.rejection[i] == 0;
            }
        }
        check(refused, "flat celestial: a body whose previous point is behind the camera is refused (rejection 1, motion 0)");
        check(still && rightAccepted, "flat celestial: the pixels outside that body are untouched (camera term, accepted)");
    }
    // 3. The bit clear, the records bound: the camera moves 20 m (so the camera term is live), and the output must equal the output with no
    // record bound at all. The bit alone decides whether the records are read.
    {
        MonoConstants off = on;
        off.debug[3] = 0;
        off.previous[5][0] = 20.f;   // the camera moved 20 m: the camera term is live, and every pixel moves
        const float t[3] = {3, -2, -5};
        const auto records = bodyRecords(t);
        const auto srv = recordsView(device, records);
        const Output bound = run(h, off, srv.Get());
        const Output unbound = run(h, off, nullptr);
        bool moved = false;
        for (size_t i = 0; bound.ok && i < bound.motion.size(); i += 2)
            if (std::fabs(bound.motion[i]) > .01f || std::fabs(bound.motion[i + 1]) > .01f) moved = true;
        check(bound.ok && unbound.ok && bound.motion == unbound.motion && bound.rejection == unbound.rejection,
              "flat celestial: with debug.w bit 3 clear, the records bound at t18 change nothing (the output equals the run with none bound)");
        check(moved, "flat celestial: with the bit clear the camera term moves pixels in that case (the equality above has teeth)");
    }
}
}  // namespace flatcelestial

inline void flatCelestialGpuTests(ID3D11Device* device, ID3D11DeviceContext* context) {
    flatcelestial::tangentChecks();
    flatcelestial::gpuTests(device, context);
}
