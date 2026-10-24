// F85c rig: the reticle's hit-depth march on a WARP device, the PRODUCTION bytecode (kReticleDepthBytecode, built by tools\temporal_shader_build from
// src\d3d11\reticle_depth_shader.h) against nofoot::f85::marchCpu and against the analytic answer.
//
// The scenes are synthetic depth buffers (R32_FLOAT, reversed-Z with an infinite far plane: depth = near / view depth) seen through a synthetic eye's scene
// rows: a plane at 1.5 m, a slanted wall, sky, a surface under the 0.5 m minimum (ignored), a plane just past the minimum, a plane past the maximum, an
// off-axis line, a line that never meets the screen, and a plane with a nearer patch off the line (the patch must not matter). The dispatch uses the same
// constants layout the hook writes (128 bytes) and the same 16-byte structured output.
#include "../../src/common/system_d3d11.h"
#include "../native_frame_test/f85_checks.h"
#include "temporal_shader_bytecode.h"   // kReticleDepthBytecode

#include <d3d11.h>
#include <wrl/client.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

using Microsoft::WRL::ComPtr;
namespace f85 = edvr::nofoot::f85;

namespace {
unsigned g_checks = 0, g_failures = 0;
void check(bool ok, const char* name) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("FAIL: %s\n", name);
    }
}

struct Gpu {
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<ID3D11ComputeShader> cs;
    ComPtr<ID3D11Buffer> cb, out, stage;
    ComPtr<ID3D11UnorderedAccessView> uav;
    bool ok = false;
    uint32_t seq = 0;
    bool init() {
        const auto create = edvr::systemD3D11CreateDevice();
        D3D_FEATURE_LEVEL fl{};
        if (!create || FAILED(create(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev, &fl, &ctx))) return false;
        if (FAILED(dev->CreateComputeShader(edvr::kReticleDepthBytecode, sizeof(edvr::kReticleDepthBytecode), nullptr, &cs))) return false;
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = 16;
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        bd.StructureByteStride = 4;
        D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
        ud.Format = DXGI_FORMAT_UNKNOWN;
        ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        ud.Buffer.NumElements = 4;
        D3D11_BUFFER_DESC sd{};
        sd.ByteWidth = 16;
        sd.Usage = D3D11_USAGE_STAGING;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        D3D11_BUFFER_DESC cd{};
        cd.ByteWidth = 128;
        cd.Usage = D3D11_USAGE_DYNAMIC;
        cd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        cd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        ok = SUCCEEDED(dev->CreateBuffer(&bd, nullptr, &out)) && SUCCEEDED(dev->CreateUnorderedAccessView(out.Get(), &ud, &uav)) &&
             SUCCEEDED(dev->CreateBuffer(&sd, nullptr, &stage)) && SUCCEEDED(dev->CreateBuffer(&cd, nullptr, &cb));
        return ok;
    }
    // One dispatch; returns the shader's answer.
    bool run(const std::vector<float>& depth, uint32_t w, uint32_t h, const float rows[16], const double Orel[3], const double dir[3], double A, double B, f85::MarchOut* out_,
             uint32_t* seqBack) {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = w;
        td.Height = h;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R32_FLOAT;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_IMMUTABLE;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA sr{};
        sr.pSysMem = depth.data();
        sr.SysMemPitch = w * 4;
        ComPtr<ID3D11Texture2D> tex;
        ComPtr<ID3D11ShaderResourceView> srv;
        if (FAILED(dev->CreateTexture2D(&td, &sr, &tex)) || FAILED(dev->CreateShaderResourceView(tex.Get(), nullptr, &srv))) return false;
        D3D11_MAPPED_SUBRESOURCE m{};
        if (FAILED(ctx->Map(cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return false;
        struct Cb {
            float rows[16], orel[4], dir[4], prm[4];
            uint32_t dim[4];
        } c{};
        std::memcpy(c.rows, rows, sizeof(c.rows));
        for (int i = 0; i < 3; ++i) {
            c.orel[i] = static_cast<float>(Orel[i]);
            c.dir[i] = static_cast<float>(dir[i]);
        }
        c.prm[0] = static_cast<float>(A);
        c.prm[1] = static_cast<float>(B);
        c.prm[2] = static_cast<float>(f85::kReticleMinM);
        c.prm[3] = static_cast<float>(f85::kReticleMaxM);
        c.dim[0] = w;
        c.dim[1] = h;
        c.dim[2] = ++seq;
        static_assert(sizeof(Cb) == 128, "the constants are 128 bytes");
        std::memcpy(m.pData, &c, sizeof(c));
        ctx->Unmap(cb.Get(), 0);
        ctx->CSSetShader(cs.Get(), nullptr, 0);
        ID3D11Buffer* cbp = cb.Get();
        ctx->CSSetConstantBuffers(0, 1, &cbp);
        ID3D11ShaderResourceView* s = srv.Get();
        ctx->CSSetShaderResources(0, 1, &s);
        ID3D11UnorderedAccessView* u = uav.Get();
        ctx->CSSetUnorderedAccessViews(0, 1, &u, nullptr);
        ctx->Dispatch(1, 1, 1);
        ID3D11UnorderedAccessView* nu = nullptr;
        ctx->CSSetUnorderedAccessViews(0, 1, &nu, nullptr);
        ctx->CopyResource(stage.Get(), out.Get());
        D3D11_MAPPED_SUBRESOURCE r{};
        if (FAILED(ctx->Map(stage.Get(), 0, D3D11_MAP_READ, 0, &r))) return false;
        uint32_t v[4];
        std::memcpy(v, r.pData, sizeof(v));
        ctx->Unmap(stage.Get(), 0);
        float d;
        std::memcpy(&d, &v[0], 4);
        out_->d = d;
        out_->status = v[1];
        *seqBack = v[2];
        return true;
    }

    // ---- F86: the same bytecode with Dim.w = n lines (b1 holds lines 1..31), the 8-value output per line ----------------------------------------------------
    static constexpr uint32_t kWords = 8u * (1u + 32u);
    ComPtr<ID3D11Buffer> cbL, outL, stageL;
    ComPtr<ID3D11UnorderedAccessView> uavL;
    bool initLines() {
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = kWords * 4u;
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        bd.StructureByteStride = 4;
        D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
        ud.Format = DXGI_FORMAT_UNKNOWN;
        ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        ud.Buffer.NumElements = kWords;
        D3D11_BUFFER_DESC sd{};
        sd.ByteWidth = kWords * 4u;
        sd.Usage = D3D11_USAGE_STAGING;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        D3D11_BUFFER_DESC cd{};
        cd.ByteWidth = 1024;
        cd.Usage = D3D11_USAGE_DYNAMIC;
        cd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        cd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        return SUCCEEDED(dev->CreateBuffer(&bd, nullptr, &outL)) && SUCCEEDED(dev->CreateUnorderedAccessView(outL.Get(), &ud, &uavL)) &&
               SUCCEEDED(dev->CreateBuffer(&sd, nullptr, &stageL)) && SUCCEEDED(dev->CreateBuffer(&cd, nullptr, &cbL));
    }
    struct Line {
        double orel[3], dir[3], dmin, dmax;
    };
    // One dispatch of n lines; words receives the output buffer (header, then 8 values a line).
    bool runLines(const std::vector<float>& depth, uint32_t w, uint32_t h, const float rows[16], double A, double B, const Line* lines, uint32_t n, std::vector<uint32_t>* words) {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = w;
        td.Height = h;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R32_FLOAT;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_IMMUTABLE;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA sr{};
        sr.pSysMem = depth.data();
        sr.SysMemPitch = w * 4;
        ComPtr<ID3D11Texture2D> tex;
        ComPtr<ID3D11ShaderResourceView> srv;
        if (FAILED(dev->CreateTexture2D(&td, &sr, &tex)) || FAILED(dev->CreateShaderResourceView(tex.Get(), nullptr, &srv))) return false;
        D3D11_MAPPED_SUBRESOURCE m{};
        if (FAILED(ctx->Map(cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return false;
        struct Cb {
            float rows[16], orel[4], dir[4], prm[4];
            uint32_t dim[4];
        } c{};
        std::memcpy(c.rows, rows, sizeof(c.rows));
        for (int i = 0; i < 3; ++i) {
            c.orel[i] = static_cast<float>(lines[0].orel[i]);
            c.dir[i] = static_cast<float>(lines[0].dir[i]);
        }
        c.prm[0] = static_cast<float>(A);
        c.prm[1] = static_cast<float>(B);
        c.prm[2] = static_cast<float>(lines[0].dmin);
        c.prm[3] = static_cast<float>(lines[0].dmax);
        c.dim[0] = w;
        c.dim[1] = h;
        c.dim[2] = ++seq;
        c.dim[3] = n;
        std::memcpy(m.pData, &c, sizeof(c));
        ctx->Unmap(cb.Get(), 0);
        if (FAILED(ctx->Map(cbL.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return false;
        float ln[64][4] = {};
        for (uint32_t k = 1; k < n; ++k) {
            for (int i = 0; i < 3; ++i) {
                ln[2 * (k - 1)][i] = static_cast<float>(lines[k].orel[i]);
                ln[2 * (k - 1) + 1][i] = static_cast<float>(lines[k].dir[i]);
            }
            ln[2 * (k - 1)][3] = static_cast<float>(lines[k].dmin);
            ln[2 * (k - 1) + 1][3] = static_cast<float>(lines[k].dmax);
        }
        std::memcpy(m.pData, ln, sizeof(ln));
        ctx->Unmap(cbL.Get(), 0);
        ctx->CSSetShader(cs.Get(), nullptr, 0);
        ID3D11Buffer* cbs[2] = {cb.Get(), cbL.Get()};
        ctx->CSSetConstantBuffers(0, 2, cbs);
        ID3D11ShaderResourceView* s = srv.Get();
        ctx->CSSetShaderResources(0, 1, &s);
        ID3D11UnorderedAccessView* u = uavL.Get();
        ctx->CSSetUnorderedAccessViews(0, 1, &u, nullptr);
        ctx->Dispatch(1, 1, 1);
        ID3D11UnorderedAccessView* nu = nullptr;
        ctx->CSSetUnorderedAccessViews(0, 1, &nu, nullptr);
        ctx->CopyResource(stageL.Get(), outL.Get());
        D3D11_MAPPED_SUBRESOURCE r{};
        if (FAILED(ctx->Map(stageL.Get(), 0, D3D11_MAP_READ, 0, &r))) return false;
        words->resize(kWords);
        std::memcpy(words->data(), r.pData, kWords * 4u);
        ctx->Unmap(stageL.Get(), 0);
        return true;
    }
};
}  // namespace

int wmain(int argc, wchar_t** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc != 2) return 2;
    if (!std::wcscmp(argv[1], L"--dry-run")) {
        std::puts("reticle_depth_test: dry-run (no device)");
        return 0;
    }
    if (std::wcscmp(argv[1], L"--self-test")) return 2;

    Gpu gpu;
    check(gpu.init(), "WARP device, the production bytecode and the 128-byte constants");
    if (!gpu.ok) {
        std::printf("reticle_depth_test: %u checks, %u failures\n", g_checks, g_failures);
        return 1;
    }
    // the eye: the same rows the F85 checks use (an eye at the origin of its rows, looking down +z, jittered principal point)
    const f85test::Mat I = f85test::Mat{{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};
    const double nearZ = 0.025, fx = 1.38, px = 0.0153 + 0.002, py = 0.0031;
    float rows[16];
    const double e0[3] = {0, 0, 0};
    f85test::sceneRows(I, e0, fx, fx, px, py, nearZ, rows);
    double A = 0.0, B = 0.0;
    check(f85::zwFit(rows, &A, &B), "the rows fit depth = A + B / w");
    const uint32_t N = 512;
    std::vector<float> depth(static_cast<size_t>(N) * N);
    const double O[3] = {0, 0, 0}, ow[3] = {0, 0, 0};
    f85::MarchView mv;
    mv.rows = rows;
    mv.A = A;
    mv.B = B;
    mv.width = mv.height = N;
    mv.depth = depth.data();
    const auto fillPlane = [&](double wv) {
        for (auto& d : depth) d = wv > 0.0 ? static_cast<float>(nearZ / wv) : 0.0f;
    };
    const auto compare = [&](const char* name, const double dir[3], double expect, uint32_t expectStatus, double tolRel) {
        f85::MarchOut cpu = f85::marchCpu(mv, O, dir);
        f85::MarchOut g{};
        uint32_t seqBack = 0;
        const bool ran = gpu.run(depth, N, N, rows, O, dir, A, B, &g, &seqBack);
        char msg[160];
        std::snprintf(msg, sizeof(msg), "%s: the shader ran and returned its sequence number", name);
        check(ran && seqBack == gpu.seq, msg);
        std::snprintf(msg, sizeof(msg), "%s: GPU status %u equals the CPU march's %u (expected %u)", name, g.status, cpu.status, expectStatus);
        check(ran && g.status == cpu.status && cpu.status == expectStatus, msg);
        if (expectStatus == f85::kMarchHit) {
            std::snprintf(msg, sizeof(msg), "%s: GPU D_hit %.4f within %.1f%% of the analytic %.4f (CPU %.4f)", name, static_cast<double>(g.d), tolRel * 100.0, expect, cpu.d);
            check(ran && std::fabs(g.d - expect) <= tolRel * expect && std::fabs(g.d - cpu.d) <= 0.005 * expect, msg);
        } else if (expectStatus == f85::kMarchSky) {
            std::snprintf(msg, sizeof(msg), "%s: GPU gives the 50 m maximum", name);
            check(ran && std::fabs(g.d - f85::kReticleMaxM) < 1e-3, msg);
        }
    };
    const double fwd[3] = {0, 0, 1};
    fillPlane(1.5);
    compare("plane at 1.5 m", fwd, 1.5, f85::kMarchHit, 0.01);
    fillPlane(0.62);
    compare("plane at 0.62 m", fwd, 0.62, f85::kMarchHit, 0.015);
    fillPlane(7.0);
    compare("plane at 7 m", fwd, 7.0, f85::kMarchHit, 0.01);
    fillPlane(40.0);
    compare("plane at 40 m", fwd, 40.0, f85::kMarchHit, 0.01);
    fillPlane(0.3);
    compare("a surface 0.3 m out", fwd, 0.5, f85::kMarchNear, 0.0);
    fillPlane(80.0);
    compare("a plane at 80 m", fwd, 50.0, f85::kMarchSky, 0.0);
    fillPlane(0.0);
    compare("sky", fwd, 50.0, f85::kMarchSky, 0.0);
    // off-axis: the line 3 degrees to the right meets the plane 2.2 m deep at 2.2 / cos
    {
        fillPlane(2.2);
        const double t = std::tan(3.0 * f85::kPi / 180.0);
        double dir[3] = {t, 0.0, 1.0};
        const double l = f85::len3(dir);
        for (double& v : dir) v /= l;
        compare("off-axis line, plane 2.2 m deep", dir, 2.2 / dir[2], f85::kMarchHit, 0.012);
    }
    // a wall at an angle (n . x = 3 in the eye's frame), the aim down the eye's z axis: d = 3 / n.z
    {
        const double nrm[3] = {std::sin(0.35), 0.0, std::cos(0.35)};
        for (uint32_t iy = 0; iy < N; ++iy)
            for (uint32_t ix = 0; ix < N; ++ix) {
                const double nx = ((ix + 0.5) / N) * 2.0 - 1.0, ny = 1.0 - ((iy + 0.5) / N) * 2.0;
                const double tx = (nx - px) / fx, ty = (ny - py) / fx;
                const double den = nrm[0] * tx + nrm[1] * ty + nrm[2];
                const double s = den > 1e-6 ? 3.0 / den : 1e30;
                depth[static_cast<size_t>(iy) * N + ix] = s < 1e29 ? static_cast<float>(nearZ / s) : 0.0f;
            }
        compare("slanted wall", fwd, 3.0 / nrm[2], f85::kMarchHit, 0.01);
    }
    // a nearer patch off the aim line must not matter
    {
        fillPlane(4.0);
        for (uint32_t iy = 0; iy < 64; ++iy)
            for (uint32_t ix = 0; ix < 64; ++ix) depth[static_cast<size_t>(iy) * N + ix] = static_cast<float>(nearZ / 0.4);
        compare("plane at 4 m with a near patch in a corner", fwd, 4.0, f85::kMarchHit, 0.01);
    }
    // a line that leaves the screen at once
    {
        fillPlane(2.0);
        const double t = std::tan(70.0 * f85::kPi / 180.0);
        double dir[3] = {t, 0.0, 1.0};
        const double l = f85::len3(dir);
        for (double& v : dir) v /= l;
        compare("a line 70 degrees off axis never on the screen", dir, 50.0, f85::kMarchOffscreen, 0.0);
    }
    // a frame's constants are not carried over: the sequence number comes back as sent
    {
        fillPlane(1.5);
        f85::MarchOut g{};
        uint32_t s1 = 0, s2 = 0;
        gpu.run(depth, N, N, rows, O, fwd, A, B, &g, &s1);
        gpu.run(depth, N, N, rows, O, fwd, A, B, &g, &s2);
        check(s2 == s1 + 1, "the sequence number of each dispatch comes back (a stale read-back would be refused)");
    }
    (void)ow;

    // ---- F86: the same bytecode, n lines a dispatch (Dim.w = n; b1 holds lines 1..31; 8 values a line), against nofoot::f86::marchRangeCpu --------------------------
    {
        namespace f86 = edvr::nofoot::f86;
        check(gpu.initLines(), "F86: the 264-word output, its staging copy and the 1024-byte lines buffer are made");
        const auto lineOf = [](const std::vector<uint32_t>& wd, uint32_t k) {
            f86::LineOut lo;
            const uint32_t b = 8u + 8u * k;
            lo.status = wd[b];
            float fv[7];
            std::memcpy(fv, &wd[b + 1], sizeof(fv));
            lo.d = fv[0];
            lo.px = fv[1];
            lo.py = fv[2];
            for (int q = 0; q < 4; ++q) lo.nb[q] = fv[3 + q];
            lo.pixOk = lo.status == f86::kLineHit;
            return lo;
        };
        std::vector<Gpu::Line> L;
        const auto add = [&](double x, double y, double z, double dmin, double dmax) {
            Gpu::Line l{};
            const double len = std::sqrt(x * x + y * y + z * z);
            l.dir[0] = x / len;
            l.dir[1] = y / len;
            l.dir[2] = z / len;
            l.dmin = dmin;
            l.dmax = dmax;
            L.push_back(l);
        };
        // compares every line of a dispatch with the CPU march: status, distance, hit pixel (one pixel), the neighbours' view depths (2%); returns the lines compared
        const auto compareAll = [&](const char* scene, const std::vector<uint32_t>& wd) {
            bool statusOk = true, distOk = true, pixOk = true, nbOk = true;
            for (uint32_t k = 0; k < L.size(); ++k) {
                const f86::LineOut cpu = f86::marchRangeCpu(mv, O, L[k].dir, L[k].dmin, L[k].dmax);
                const f86::LineOut g = lineOf(wd, k);
                statusOk = statusOk && g.status == cpu.status;
                if (cpu.status == f86::kLineHit && g.status == f86::kLineHit) {
                    distOk = distOk && std::fabs(g.d - cpu.d) <= 0.005 * cpu.d;
                    pixOk = pixOk && std::fabs(g.px - cpu.px) <= 1.0 && std::fabs(g.py - cpu.py) <= 1.0;
                    for (int q = 0; q < 4; ++q) nbOk = nbOk && std::fabs(g.nb[q] - cpu.nb[q]) <= 0.02 * cpu.nb[q];
                }
            }
            char msg[200];
            std::snprintf(msg, sizeof(msg), "F86 %s: every line's status equals the CPU march's", scene);
            check(statusOk, msg);
            std::snprintf(msg, sizeof(msg), "F86 %s: every hit's distance is within 0.5%% of the CPU march's", scene);
            check(distOk, msg);
            std::snprintf(msg, sizeof(msg), "F86 %s: every hit pixel is within one pixel of the CPU march's", scene);
            check(pixOk, msg);
            std::snprintf(msg, sizeof(msg), "F86 %s: every hit's four neighbour depths match the CPU's (2%%)", scene);
            check(nbOk, msg);
        };
        const double tan70 = std::tan(70.0 * f85::kPi / 180.0);
        // scene 1: a plane at 0.395 m, ten lines: near, five panel rays, near, unused, miss, off screen
        {
            fillPlane(0.395);
            L.clear();
            add(0, 0, 1, 0.5, 50.0);
            add(0, 0, 1, 0.12, 0.8);
            add(0.04, 0.02, 1, 0.12, 0.8);
            add(-0.04, 0.02, 1, 0.12, 0.8);
            add(0.04, -0.02, 1, 0.12, 0.8);
            add(-0.04, -0.02, 1, 0.12, 0.8);
            add(0, 0, 1, 0.5, 0.9);
            add(0, 0, 1, 0.3, 0.3);
            add(0, 0, 1, 0.12, 0.3);
            add(tan70, 0, 1, 0.12, 0.8);
            std::vector<uint32_t> wd;
            const bool ran = gpu.runLines(depth, N, N, rows, A, B, L.data(), static_cast<uint32_t>(L.size()), &wd);
            check(ran && wd[0] == gpu.seq && wd[1] == L.size(), "F86: the header carries the dispatch's sequence number and the line count");
            if (ran) {
                compareAll("scene 1 (a plane at 0.395 m)", wd);
                const f86::LineOut a = lineOf(wd, 1), z = lineOf(wd, 7);
                check(a.status == f86::kLineHit && std::fabs(a.d - 0.395) < 0.002 && lineOf(wd, 0).status == f86::kLineNear && lineOf(wd, 6).status == f86::kLineNear &&
                          z.status == f86::kLineUnused && lineOf(wd, 8).status == f86::kLineMiss && lineOf(wd, 9).status == f86::kLineOffscreen,
                      "F86: the panel ray hits at 0.395 m, a range behind the surface is near, a range short of it a miss, an empty range unused, a ray off screen off screen");
            }
        }
        // scene 2: a plane 34 degrees off the view ray at 0.94 m: the hit pixel's neighbours give the plane's normal
        {
            const double a34 = 34.0 * f85::kPi / 180.0;
            const double nrm[3] = {std::sin(a34), 0.0, std::cos(a34)};
            for (uint32_t iy = 0; iy < N; ++iy)
                for (uint32_t ix = 0; ix < N; ++ix) {
                    const double nx = ((ix + 0.5) / N) * 2.0 - 1.0, ny = 1.0 - ((iy + 0.5) / N) * 2.0;
                    const double tx = (nx - px) / fx, ty = (ny - py) / fx;
                    const double den = nrm[0] * tx + nrm[1] * ty + nrm[2];
                    const double s = den > 1e-6 ? 0.94 * std::cos(a34) / den : 1e30;
                    depth[static_cast<size_t>(iy) * N + ix] = s < 1e29 ? static_cast<float>(nearZ / s) : 0.0f;
                }
            L.clear();
            add(0, 0, 1, 0, 0);
            add(0, 0, 1, 0.5, 1.5);
            add(0.1, 0.05, 1, 0.5, 1.5);
            add(-0.1, -0.05, 1, 0.5, 1.5);
            std::vector<uint32_t> wd;
            const bool ran = gpu.runLines(depth, N, N, rows, A, B, L.data(), static_cast<uint32_t>(L.size()), &wd);
            check(ran && lineOf(wd, 0).status == f86::kLineUnused, "F86: line 0 may be unused (key 10 not drawn) while the others march");
            if (ran) {
                compareAll("scene 2 (a plane 34 degrees off the view ray)", wd);
                const f86::LineOut g = lineOf(wd, 1);
                const f85::Rows Wr = f85::rowsOfWorld(rows);
                const double H[3] = {g.d * L[1].dir[0], g.d * L[1].dir[1], g.d * L[1].dir[2]};
                const double w = Wr.r[2][0] * H[0] + Wr.r[2][1] * H[1] + Wr.r[2][2] * H[2] + Wr.r[2][3];
                const f86::SurfaceNormal sn = f86::surfaceNormal(Wr, N, N, g, w, L[1].dir);
                const double dot = -(sn.n[0] * nrm[0] + sn.n[1] * nrm[1] + sn.n[2] * nrm[2]);
                check(g.status == f86::kLineHit && sn.ok && std::acos(std::min(1.0, dot)) * f85::kDeg < 1.0,
                      "F86: the normal from the GPU's neighbour depths is the plane's within 1 degree");
            }
        }
        // scene 3: all 32 lines of a dispatch, each its own origin, direction and range
        {
            fillPlane(1.5);
            L.clear();
            for (uint32_t k = 0; k < 32; ++k) {
                const double a = 0.02 * (static_cast<double>(k) - 15.5), b = 0.015 * std::sin(0.7 * k);
                add(a, b, 1, 0.3 + 0.02 * k, 3.0 + 0.1 * k);
                if (k % 7 == 2) L.back().dmax = L.back().dmin;   // some unused (not the last)
            }
            std::vector<uint32_t> wd;
            const bool ran = gpu.runLines(depth, N, N, rows, A, B, L.data(), static_cast<uint32_t>(L.size()), &wd);
            check(ran && wd[1] == 32u, "F86: 32 lines are dispatched at once");
            if (ran) {
                compareAll("scene 3 (32 lines)", wd);
                check(lineOf(wd, 31).status == f86::kLineHit && std::fabs(lineOf(wd, 31).d - 1.5 / std::cos(std::atan(std::hypot(0.02 * 15.5, 0.015 * std::sin(0.7 * 31))))) < 0.02,
                      "F86: the last line's own values are written at the end of the output");
            }
        }
        // the legacy contract is unaffected by the larger buffers: a single-line dispatch with Dim.w = 0 on the old resources still answers
        {
            fillPlane(1.5);
            f85::MarchOut g{};
            uint32_t sb = 0;
            const bool ran = gpu.run(depth, N, N, rows, O, fwd, A, B, &g, &sb);
            check(ran && sb == gpu.seq && g.status == f85::kMarchHit && std::fabs(g.d - 1.5) < 0.02, "F86: after multi-line dispatches the single-line contract (Dim.w = 0) still answers as before");
        }
    }
    std::printf("reticle_depth_test: %u checks, %u failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
