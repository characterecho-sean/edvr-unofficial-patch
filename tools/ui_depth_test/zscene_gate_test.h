// The motion prep's absent-layer gate (docs/edvr-gpu-trims-2026-10-14.md, P2 / G09): zSceneAt
// (src/d3d11/temporal_shader_source.h) does not fetch t6 (the smoke's depth) or t7 (the private UI
// depth) when probe.w bit 64 or 256 says the dispatch left that view unbound, and uses the +0.0 a null
// view returns in its place. Held here, bit for bit, against the expression it replaced:
//   * parity: the production function (its text read from the source, probe.w from a cbuffer) against
//     the old expression compiled beside it, over Z / ZS / ZUI holding every combination of ten bit
//     patterns (both zeros, the smallest and the largest denormal, both quiet NaNs, +Inf, -1, 0.5, 1), for
//     all four presence combinations with the bits matching the bindings, and with the bits clear (a
//     caller that does not say): asuint-equal on every texel, with and without the screen map's bit 32;
//   * controls: an absent bit set while its layer IS bound must change exactly the texels where that
//     layer wins and no other (the gate is live, and skips only that layer), and bit 32 with the screen
//     map set returns the map's depth in both;
//   * the CPU half (cpuHalf, no device): depthLayersAbsentBits over its slots, and both dispatches in
//     temporal_pass.cpp reading it off the array they bind and writing it before setParams and the Dispatch.
// The investigation behind the gate, and the no-smoke A/B bench whose method and fixture --bench adapts,
// are RocketsProjects' (2026-10-23).
// WARP, inside the build's run. By hand:
//   ui_depth_test --zscene-gate [--adapter nvidia] [--bench]
//     --adapter nvidia  the parity on the RTX (a desk check, never part of the build)
//     --bench           times temporal_mv_fast_cs at 2016x2016 one eye, smoke absent, UI depth off and on:
//                       (a) the shader before the gate, (b) the gate with the bits set, (c) a compiled no-ZS
//                       variant (the ideal; bench-only), (d) the gate with the bits clear and the smoke
//                       bound, each paired against (a) (pairedRounds below), every output byte checked first
#pragma once
#include <d3d11shader.h>
#include <dxgi.h>
#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>
#include <string>
#include <utility>
#include <vector>
#pragma comment(lib, "dxgi.lib")
#include "../../src/d3d11/temporal_pass.h"   // edvr::depthLayersAbsentBits, the CPU half (cpuHalf)

namespace zgate {

// The function as it was before the gate (origin/main c16a46b3), byte for byte: the reference.
const char kOldZSceneAt[] =
    "float zSceneAt(int2 q) {\n"
    "    if((uint(probe.w+.5)&32u)!=0u) { float4 s=Screen.Load(int3(q,0));if(s.w>0)return s.z; }\n"
    "    return max(max(Z.Load(int3(q, 0)), ZS.Load(int3(q, 0))), ZUI.Load(int3(q, 0)));\n"
    "}\n";
const uint32_t kPatterns[10] = {0x00000000u, 0x80000000u, 0x00000001u, 0x007FFFFFu, 0x7FC00000u,
                                0xFFC00000u, 0x7F800000u, 0xBF800000u, 0x3F000000u, 0x3F800000u};
constexpr UINT kW = 40, kH = 25;   // 1000 texels: every (Z, ZS, ZUI) triple of the ten patterns

std::string temporalSource() {
    std::ifstream f("src/d3d11/temporal_shader_source.h", std::ios::binary);
    std::string s((std::istreambuf_iterator<char>(f)), {});
    s.erase(std::remove(s.begin(), s.end(), '\r'), s.end());
    return s;
}
// kTemporalCsHlsl as the compiler sees it: the adjacent raw literals joined (screen_consumer_test.h's walk).
std::string temporalHlsl(const std::string& source) {
    std::string hlsl;
    size_t cursor = source.find("constexpr char kTemporalCsHlsl[]");
    const size_t end = source.find(")HLSL\";", cursor);
    check(cursor != std::string::npos && end != std::string::npos, "zscene gate: the production temporal shader found");
    for (;;) {
        size_t begin = source.find("R\"HLSL(", cursor);
        if (begin == std::string::npos || begin > end) break;
        begin += 7;
        const size_t close = source.find(")HLSL\"", begin);
        check(close != std::string::npos && close <= end, "zscene gate: complete temporal shader literal");
        hlsl.append(source, begin, close - begin);
        cursor = close + 6;
    }
    return hlsl;
}
// The production zSceneAt, from its signature to zAt's (the span ui_depth_test's accessor check reads).
std::pair<size_t, size_t> zSceneSpan(const std::string& hlsl) {
    const size_t a = hlsl.find("float zSceneAt("), b = hlsl.find("float zAt(", a);
    check(a != std::string::npos && b != std::string::npos && hlsl.find("float zSceneAt(", a + 1) == std::string::npos,
          "zscene gate: exactly one production zSceneAt, followed by zAt");
    return {a, b};
}
std::string declLine(const std::string& hlsl, const char* start) {
    const size_t a = hlsl.find(start);
    check(a != std::string::npos, "zscene gate: temporal register declaration found");
    return hlsl.substr(a, hlsl.find('\n', a) - a) + "\n";
}

ComPtr<ID3DBlob> compileEntry(const std::string& text, const char* entry, const D3D_SHADER_MACRO* macros, UINT flags) {
    ComPtr<ID3DBlob> blob, errors;
    const HRESULT made = D3DCompile(text.data(), text.size(), "temporal", macros, nullptr, entry, "cs_5_0", flags, 0, &blob, &errors);
    if (FAILED(made) && errors) std::puts(static_cast<const char*>(errors->GetBufferPointer()));
    check(SUCCEEDED(made), "zscene gate: shader compiles");
    return blob;
}
ComPtr<ID3D11ComputeShader> makeCs(ID3D11Device* dev, const void* code, size_t size) {
    ComPtr<ID3D11ComputeShader> cs;
    check(SUCCEEDED(dev->CreateComputeShader(code, size, nullptr, &cs)), "zscene gate: compute shader created");
    return cs;
}
ComPtr<ID3D11Texture2D> tex2d(ID3D11Device* dev, UINT w, UINT h, DXGI_FORMAT fmt, UINT bind, const void* data, UINT pitch) {
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w; td.Height = h; td.MipLevels = td.ArraySize = 1; td.Format = fmt; td.SampleDesc.Count = 1; td.BindFlags = bind;
    D3D11_SUBRESOURCE_DATA init{data, pitch, 0};
    ComPtr<ID3D11Texture2D> t;
    check(SUCCEEDED(dev->CreateTexture2D(&td, data ? &init : nullptr, &t)), "zscene gate: texture created");
    return t;
}
ComPtr<ID3D11ShaderResourceView> srvOf(ID3D11Device* dev, ID3D11Resource* r) {
    ComPtr<ID3D11ShaderResourceView> v;
    check(SUCCEEDED(dev->CreateShaderResourceView(r, nullptr, &v)), "zscene gate: view created");
    return v;
}
ComPtr<ID3D11UnorderedAccessView> uavOf(ID3D11Device* dev, ID3D11Resource* r) {
    ComPtr<ID3D11UnorderedAccessView> v;
    check(SUCCEEDED(dev->CreateUnorderedAccessView(r, nullptr, &v)), "zscene gate: unordered view created");
    return v;
}
std::vector<uint32_t> readU32(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11Texture2D* t) {
    D3D11_TEXTURE2D_DESC td{}; t->GetDesc(&td);
    td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> stage;
    check(SUCCEEDED(dev->CreateTexture2D(&td, nullptr, &stage)), "zscene gate: staging texture");
    ctx->CopyResource(stage.Get(), t);
    D3D11_MAPPED_SUBRESOURCE m{};
    check(SUCCEEDED(ctx->Map(stage.Get(), 0, D3D11_MAP_READ, 0, &m)), "zscene gate: staging map");
    std::vector<uint32_t> out(td.Width * td.Height);
    for (UINT y = 0; y < td.Height; ++y) std::memcpy(&out[y * td.Width], static_cast<const char*>(m.pData) + y * m.RowPitch, td.Width * 4);
    ctx->Unmap(stage.Get(), 0);
    return out;
}

// The parity and its controls on one device. Returns the texels compared.
uint64_t parity(ID3D11Device* dev, ID3D11DeviceContext* ctx, const char* where) {
    const std::string hlsl = temporalHlsl(temporalSource());
    const auto span = zSceneSpan(hlsl);
    std::string decls;
    for (const char* start : {"Texture2D<float> Z :", "Texture2D<float> ZS :", "Texture2D<float> ZUI :", "Texture2D<float4> Screen :"})
        decls += declLine(hlsl, start);
    decls += "cbuffer G : register(b0) { float4 probe; };\n";
    const std::string tail = "RWTexture2D<uint> Result : register(u0);\n"
                             "[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID) { Result[id.xy] = asuint(zSceneAt(int2(id.xy))); }\n";
    const std::string newText = decls + hlsl.substr(span.first, span.second - span.first) + tail;
    const std::string oldText = decls + kOldZSceneAt + tail;
    check(hlsl.compare(span.first, span.second - span.first, kOldZSceneAt) != 0, "zscene gate: the production zSceneAt is not the old one (the gate is in)");
    auto newCode = compileEntry(newText, "main", nullptr, D3DCOMPILE_ENABLE_STRICTNESS);
    auto oldCode = compileEntry(oldText, "main", nullptr, D3DCOMPILE_ENABLE_STRICTNESS);
    auto newCs = makeCs(dev, newCode->GetBufferPointer(), newCode->GetBufferSize());
    auto oldCs = makeCs(dev, oldCode->GetBufferPointer(), oldCode->GetBufferSize());

    std::vector<uint32_t> z(kW * kH), zs(kW * kH), zu(kW * kH);
    std::vector<float> screen(kW * kH * 4, 0.0f);
    for (UINT i = 0; i < kW * kH; ++i) {
        z[i] = kPatterns[i % 10]; zs[i] = kPatterns[(i / 10) % 10]; zu[i] = kPatterns[i / 100];
        if (i % 3 == 0) { std::memcpy(&screen[i * 4 + 2], &kPatterns[(i / 3) % 10], 4); screen[i * 4 + 3] = 1.0f; }
    }
    auto zT = tex2d(dev, kW, kH, DXGI_FORMAT_R32_FLOAT, D3D11_BIND_SHADER_RESOURCE, z.data(), kW * 4);
    auto zsT = tex2d(dev, kW, kH, DXGI_FORMAT_R32_FLOAT, D3D11_BIND_SHADER_RESOURCE, zs.data(), kW * 4);
    auto zuT = tex2d(dev, kW, kH, DXGI_FORMAT_R32_FLOAT, D3D11_BIND_SHADER_RESOURCE, zu.data(), kW * 4);
    auto scT = tex2d(dev, kW, kH, DXGI_FORMAT_R32G32B32A32_FLOAT, D3D11_BIND_SHADER_RESOURCE, screen.data(), kW * 16);
    auto outT = tex2d(dev, kW, kH, DXGI_FORMAT_R32_UINT, D3D11_BIND_UNORDERED_ACCESS, nullptr, 0);
    auto zV = srvOf(dev, zT.Get()), zsV = srvOf(dev, zsT.Get()), zuV = srvOf(dev, zuT.Get()), scV = srvOf(dev, scT.Get());
    auto outV = uavOf(dev, outT.Get());
    D3D11_BUFFER_DESC bd{}; bd.ByteWidth = 16; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    ComPtr<ID3D11Buffer> cb;
    check(SUCCEEDED(dev->CreateBuffer(&bd, nullptr, &cb)), "zscene gate: constant buffer");

    // One dispatch: which layers are bound, the probe.w bits, whether the screen map is bound.
    auto run = [&](ID3D11ComputeShader* cs, bool t6, bool t7, uint32_t bits, bool withScreen) {
        const float probe[4] = {0, 0, 0, static_cast<float>(bits)};
        ctx->UpdateSubresource(cb.Get(), 0, nullptr, probe, 0, 0);
        ctx->ClearState();
        ID3D11ShaderResourceView* nothing[15] = {};
        ctx->CSSetShaderResources(0, 15, nothing);
        ID3D11ShaderResourceView* layers[2] = {t6 ? zsV.Get() : nullptr, t7 ? zuV.Get() : nullptr};
        ctx->CSSetShaderResources(2, 1, zV.GetAddressOf());
        ctx->CSSetShaderResources(6, 2, layers);
        if (withScreen) ctx->CSSetShaderResources(14, 1, scV.GetAddressOf());
        ctx->CSSetConstantBuffers(0, 1, cb.GetAddressOf());
        ctx->CSSetUnorderedAccessViews(0, 1, outV.GetAddressOf(), nullptr);
        ctx->CSSetShader(cs, nullptr, 0);
        ctx->Dispatch((kW + 7) / 8, (kH + 7) / 8, 1);
        auto out = readU32(dev, ctx, outT.Get());
        ctx->ClearState();
        return out;
    };
    auto absent = [](bool t6, bool t7) { return (t6 ? 0u : 64u) | (t7 ? 0u : 256u); };
    uint64_t texels = 0;
    char label[200];
    for (uint32_t screenBit : {0u, 32u}) {
        const bool withScreen = screenBit != 0;
        for (int combo = 0; combo < 4; ++combo) {
            const bool t6 = (combo & 1) != 0, t7 = (combo & 2) != 0;
            const auto old = run(oldCs.Get(), t6, t7, screenBit, withScreen);
            const auto matched = run(newCs.Get(), t6, t7, screenBit | absent(t6, t7), withScreen);
            const auto clear = run(newCs.Get(), t6, t7, screenBit, withScreen);
            size_t firstDiff = old.size();
            for (size_t i = 0; i < old.size() && firstDiff == old.size(); ++i) if (old[i] != matched[i]) firstDiff = i;
            if (firstDiff != old.size())
                std::printf("zscene gate (%s): t6 %s, t7 %s, bit32 %u: texel %zu (Z %08X ZS %08X ZUI %08X) old %08X, gated %08X\n", where,
                            t6 ? "bound" : "null", t7 ? "bound" : "null", screenBit, firstDiff, z[firstDiff], zs[firstDiff], zu[firstDiff],
                            old[firstDiff], matched[firstDiff]);
            std::snprintf(label, sizeof(label), "zscene gate (%s): t6 %s, t7 %s, bit32 %u: the bits matching the bindings give the old expression to the bit on every texel",
                          where, t6 ? "bound" : "null", t7 ? "bound" : "null", screenBit);
            check(firstDiff == old.size(), label);
            std::snprintf(label, sizeof(label), "zscene gate (%s): t6 %s, t7 %s, bit32 %u: the bits clear fetch as before, to the bit",
                          where, t6 ? "bound" : "null", t7 ? "bound" : "null", screenBit);
            check(clear == old, label);
            texels += 2 * old.size();
            if (withScreen) {
                bool screenWins = true;
                for (UINT i = 0; i < kW * kH; ++i) {
                    uint32_t s; std::memcpy(&s, &screen[i * 4 + 2], 4);
                    if (i % 3 == 0 && (old[i] != s || matched[i] != s)) screenWins = false;
                }
                std::snprintf(label, sizeof(label), "zscene gate (%s): bit 32 with the screen map set returns the map's depth in both (t6 %s, t7 %s)",
                              where, t6 ? "bound" : "null", t7 ? "bound" : "null");
                check(screenWins, label);
            }
        }
    }
    // The controls: an absent bit while its layer IS bound skips exactly that layer, so the result is the old
    // expression with that view null -- and it differs from the old expression with the view bound exactly
    // where that layer won, which some texels must.
    for (int layer = 0; layer < 2; ++layer) {
        for (bool otherBound : {false, true}) {
            const bool t6 = layer == 0 ? true : otherBound, t7 = layer == 1 ? true : otherBound;
            const uint32_t bit = layer == 0 ? 64u : 256u;
            const auto oldBound = run(oldCs.Get(), t6, t7, 0, false);
            const auto oldNull = run(oldCs.Get(), layer == 0 ? false : t6, layer == 1 ? false : t7, 0, false);
            const auto lie = run(newCs.Get(), t6, t7, bit | absent(t6, t7), false);
            size_t wins = 0;
            bool exact = true;
            for (size_t i = 0; i < oldBound.size(); ++i) {
                if (oldBound[i] != oldNull[i]) ++wins;
                if (lie[i] != oldNull[i]) exact = false;
            }
            std::snprintf(label, sizeof(label), "zscene gate (%s) control: bit %u set while %s is bound skips that layer and only it (the other %s)",
                          where, bit, layer == 0 ? "t6" : "t7", otherBound ? "bound" : "null");
            check(exact, label);
            std::snprintf(label, sizeof(label), "zscene gate (%s) control: the skipped %s wins on some texels, so the gate is seen (the other %s)",
                          where, layer == 0 ? "t6" : "t7", otherBound ? "bound" : "null");
            check(wins > 0 && lie != oldBound, label);
            texels += oldBound.size();
        }
    }
    std::printf("PASS: zscene gate parity on %s: %llu texels asuint-equal across 16 runs (10 bit patterns cubed, four presence "
                "combinations, bits matched and clear, screen bit off and on), 4 live-gate controls\n",
                where, static_cast<unsigned long long>(texels));
    return texels;
}

// ------------------------------------------------------------------ the CPU half (temporal_pass.cpp)
// The bits are only as good as the slots they are read from, and every GPU check above computes its own: a slip to
// srvs[5] (always null) would set bit 64 on every dispatch, skip a bound smoke depth, and leave them all green. So:
//   (a) edvr::depthLayersAbsentBits (temporal_pass.h) itself, over the four presence combinations of t6/t7 with a
//       distinct non-null pointer in every other of the 24 slots, and with each other slot nulled alone;
//   (b) at each dispatch that calls it in temporal_pass.cpp, the bits are read off the very array that the last
//       CSSetShaderResources(0, ...) before that Dispatch binds, declared after the previous Dispatch and untouched
//       by index after; OR'd into p.probe[3] by the statement holding the call or the next one, that OR before a
//       setParams(ctx, p), and that setParams before the Dispatch, with no other write to p.probe[3] after the OR.
std::string passSource() {
    std::ifstream f("src/d3d11/temporal_pass.cpp", std::ios::binary);
    std::string s((std::istreambuf_iterator<char>(f)), {});
    s.erase(std::remove(s.begin(), s.end(), '\r'), s.end());
    return s;
}
bool identChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_'; }
// The next occurrence of `word` at or after `from` (and before `to`) that is a whole identifier, or npos.
size_t findWord(const std::string& s, const std::string& word, size_t from, size_t to = std::string::npos) {
    for (size_t at = s.find(word, from); at != std::string::npos && at < to; at = s.find(word, at + 1))
        if ((at == 0 || !identChar(s[at - 1])) && (at + word.size() >= s.size() || !identChar(s[at + word.size()]))) return at;
    return std::string::npos;
}
// The last occurrence of `needle` in [from, to), or npos.
size_t findLast(const std::string& s, const std::string& needle, size_t from, size_t to) {
    size_t last = std::string::npos;
    for (size_t at = s.find(needle, from); at != std::string::npos && at < to; at = s.find(needle, at + 1)) last = at;
    return last;
}
bool inLineComment(const std::string& s, size_t at) {
    const size_t lineStart = s.rfind('\n', at) == std::string::npos ? 0 : s.rfind('\n', at) + 1;
    return s.find("//", lineStart) < at;
}
// The CPU-half checks over one temporal_pass.cpp text; false (with the reason printed) on the first that fails.
// `sites` returns the dispatches checked.
bool passSiteChecks(const std::string& src, int& sites) {
    sites = 0;
    auto fail = [](const char* why, size_t at, const std::string& s) {
        const size_t line = static_cast<size_t>(std::count(s.begin(), s.begin() + static_cast<std::ptrdiff_t>(std::min(at, s.size())), '\n')) + 1;
        std::printf("zscene gate CPU half: temporal_pass.cpp:%zu: %s\n", line, why);
        return false;
    };
    const std::string kCall = "depthLayersAbsentBits(";
    size_t previousDispatch = 0;
    for (size_t call = src.find(kCall); call != std::string::npos; call = src.find(kCall, call + 1)) {
        if (inLineComment(src, call) || (call > 0 && identChar(src[call - 1]))) continue;
        const size_t argBegin = call + kCall.size(), argEnd = src.find(')', argBegin);
        const std::string arg = src.substr(argBegin, argEnd - argBegin);
        if (arg.empty() || !std::all_of(arg.begin(), arg.end(), identChar)) return fail("the bits read off something other than a named array", call, src);
        const size_t dispatch = src.find("->Dispatch(", call);
        if (dispatch == std::string::npos) return fail("no Dispatch after the bits", call, src);
        if (call < previousDispatch) return fail("two calls for one Dispatch", call, src);
        // The array: declared after the previous Dispatch, before the call, and never indexed again before this one.
        const size_t prior = findLast(src, "->Dispatch(", 0, call);
        const size_t decl = findLast(src, "ID3D11ShaderResourceView* " + arg + "[", prior == std::string::npos ? 0 : prior, call);
        if (decl == std::string::npos) return fail("the bits' array is not declared for this dispatch", call, src);
        const size_t declEnd = src.find("};", decl);   // the initializer's end (its comments hold semicolons)
        if (declEnd == std::string::npos || declEnd > call) return fail("the bits' array is not a braced initializer before the call", decl, src);
        for (size_t use = findWord(src, arg, declEnd, dispatch); use != std::string::npos; use = findWord(src, arg, use + 1, dispatch))
            if (src.compare(use + arg.size(), 1, "[") == 0) return fail("the bits' array is indexed after its declaration", use, src);
        // The binding this Dispatch sees: the last CSSetShaderResources before it, from slot 0, of that same array.
        const size_t bind = findLast(src, "CSSetShaderResources(", call, dispatch);
        if (bind == std::string::npos) return fail("no CSSetShaderResources between the bits and the Dispatch", call, src);
        const size_t bindArgs = bind + std::strlen("CSSetShaderResources("), bindEnd = src.find(')', bindArgs);
        std::string args = src.substr(bindArgs, bindEnd - bindArgs);
        args.erase(std::remove(args.begin(), args.end(), ' '), args.end());
        const size_t c1 = args.find(','), c2 = args.find(',', c1 + 1);
        if (args.substr(0, c1) != "0" || c2 == std::string::npos || args.substr(c2 + 1) != arg)
            return fail("the Dispatch's CSSetShaderResources does not bind the bits' array from slot 0", bind, src);
        // The OR into p.probe[3]: the statement holding the call, or the next one, through the variable the call set.
        const size_t stmtBegin = src.find_last_of(";{}", call) + 1, stmtEnd = src.find(';', call);
        const std::string stmt = src.substr(stmtBegin, stmtEnd - stmtBegin);
        size_t orAt = std::string::npos, orEnd = std::string::npos;
        if (stmt.find("p.probe[3] =") != std::string::npos) {
            if (stmt.find("static_cast<uint32_t>(p.probe[3]) | " + kCall) == std::string::npos) return fail("the bits are not OR'd into p.probe[3]", call, src);
            orAt = stmtBegin; orEnd = stmtEnd;
        } else {
            const size_t eq = stmt.rfind('=', call - stmtBegin);   // `const uint32_t name = depthLayersAbsentBits(...)`
            std::string var;
            if (eq != std::string::npos && eq > 0) {
                const size_t nameEnd = stmt.find_last_not_of(" \t\n", eq - 1) + 1, nameBegin = stmt.find_last_of(" \t\n", nameEnd - 1) + 1;
                var = stmt.substr(nameBegin, nameEnd - nameBegin);
            }
            const size_t next = stmtEnd + 1, nextEnd = src.find(';', next);
            const std::string nextStmt = src.substr(next, nextEnd - next);
            if (var.empty() || nextStmt.find("p.probe[3] =") == std::string::npos ||
                nextStmt.find("static_cast<uint32_t>(p.probe[3]) | " + var + ")") == std::string::npos)
                return fail("the statement after the bits does not OR them into p.probe[3]", next, src);
            orAt = next; orEnd = nextEnd;
        }
        // setParams after the OR and before the Dispatch, and nothing writing p.probe[3] after the OR.
        if (findLast(src, "setParams(ctx, p)", orEnd, dispatch) == std::string::npos)
            return fail("no setParams(ctx, p) between the OR into p.probe[3] and the Dispatch", orAt, src);
        if (src.find("probe[3] =", orEnd) < dispatch) return fail("p.probe[3] written again between the OR and the Dispatch", orEnd, src);
        previousDispatch = dispatch;
        ++sites;
    }
    if (sites != 2) { std::printf("zscene gate CPU half: %d dispatches compute the bits (want the mv prep's and main's: 2)\n", sites); return false; }
    return true;
}
int cpuHalf() {
    // (a) The function: cookies, never dereferenced, one distinct address a slot.
    static unsigned char cookies[24][16];
    ID3D11ShaderResourceView* all[24];
    for (int i = 0; i < 24; ++i) all[i] = reinterpret_cast<ID3D11ShaderResourceView*>(cookies[i]);
    char label[200];
    int cases = 0;
    for (int combo = 0; combo < 4; ++combo) {
        ID3D11ShaderResourceView* srvs[24];
        std::copy(all, all + 24, srvs);
        const bool t6 = (combo & 1) != 0, t7 = (combo & 2) != 0;
        if (!t6) srvs[6] = nullptr;
        if (!t7) srvs[7] = nullptr;
        const uint32_t want = (t6 ? 0u : 64u) | (t7 ? 0u : 256u), got = edvr::depthLayersAbsentBits(srvs);
        std::snprintf(label, sizeof(label), "zscene gate CPU half: depthLayersAbsentBits = %u with t6 %s and t7 %s, every other slot bound (want %u)",
                      got, t6 ? "bound" : "null", t7 ? "bound" : "null", want);
        check(got == want, label);
        ++cases;
    }
    for (int k = 0; k < 24; ++k) {
        if (k == 6 || k == 7) continue;
        ID3D11ShaderResourceView* srvs[24];
        std::copy(all, all + 24, srvs);
        srvs[k] = nullptr;
        std::snprintf(label, sizeof(label), "zscene gate CPU half: depthLayersAbsentBits ignores slot %d (t6 and t7 bound, slot %d null)", k, k);
        check(edvr::depthLayersAbsentBits(srvs) == 0u, label);
        ++cases;
    }
    // (b) The two dispatches.
    int sites = 0;
    check(passSiteChecks(passSource(), sites),
          "zscene gate CPU half: both dispatches read the bits off the array they bind and OR them into probe.w before setParams and the Dispatch");
    std::printf("PASS: zscene gate CPU half: depthLayersAbsentBits over %d slot cases; %d dispatches read the bits off the array they bind, "
                "before setParams and the Dispatch\n", cases, sites);
    return cases;
}

// ------------------------------------------------------------------ the bench (--bench, by hand)

struct Device { ComPtr<ID3D11Device> dev; ComPtr<ID3D11DeviceContext> ctx; std::string name; bool warp = true; };
Device makeDevice(bool nvidia) {
    Device d;
    ComPtr<IDXGIAdapter> adapter;
    D3D_DRIVER_TYPE type = D3D_DRIVER_TYPE_WARP;
    d.name = "WARP";
    if (nvidia) {
        ComPtr<IDXGIFactory1> factory;
        check(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))), "zscene gate: DXGI factory");
        for (UINT i = 0;; ++i) {
            ComPtr<IDXGIAdapter> a;
            if (factory->EnumAdapters(i, &a) == DXGI_ERROR_NOT_FOUND) break;
            DXGI_ADAPTER_DESC desc{};
            a->GetDesc(&desc);
            if (desc.VendorId == 0x10DE) {
                adapter = a;
                char name[128] = {};
                WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, sizeof(name) - 1, nullptr, nullptr);
                d.name = name;
                break;
            }
        }
        check(adapter != nullptr, "zscene gate: an NVIDIA adapter for --adapter nvidia");
        type = D3D_DRIVER_TYPE_UNKNOWN;
        d.warp = false;
    }
    D3D_FEATURE_LEVEL level{};
    check(SUCCEEDED(D3D11CreateDevice(adapter.Get(), type, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &d.dev, &level, &d.ctx)),
          "zscene gate: device created");
    return d;
}

double median(std::vector<double> v) { std::sort(v.begin(), v.end()); return v.empty() ? -1.0 : v[v.size() / 2]; }

// The paired timing (method adapted from RocketsProjects' no-smoke A/B, 2026-10-23): blocks of 48 back-to-back dispatches
// between two GPU timestamps, a disjoint clock a failure; 12 rounds of four blocks, A B B A then B A A B on alternate rounds,
// so slow drift falls on both arms alike; each round's delta is (A1 + A2 - B1 - B2) / 2. Binding and constants are set
// before each block, outside its timestamps. One change for a fast GPU: every block of a comparison is queued back to back
// and the clocks read at the end. A 48-dispatch block is about 2.5 ms on an RTX 5090, and draining the queries after each
// one left the GPU idle between blocks, its clocks falling and rising: arm medians that moved by half between runs.
// Run the executable three times for three runs.
constexpr UINT kPairedDispatches = 48, kPairedRounds = 12;
struct Block { ComPtr<ID3D11Query> disjoint, t0, t1; };
Block issueBlock(Device& d, const std::function<void()>& bind, UINT gx, UINT gy, UINT dispatches) {
    ID3D11DeviceContext* ctx = d.ctx.Get();
    Block k;
    D3D11_QUERY_DESC qd{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
    check(SUCCEEDED(d.dev->CreateQuery(&qd, &k.disjoint)), "zscene gate: disjoint query");
    qd.Query = D3D11_QUERY_TIMESTAMP;
    check(SUCCEEDED(d.dev->CreateQuery(&qd, &k.t0)) && SUCCEEDED(d.dev->CreateQuery(&qd, &k.t1)), "zscene gate: timestamp queries");
    bind();
    ctx->Begin(k.disjoint.Get());
    ctx->End(k.t0.Get());
    for (UINT i = 0; i < dispatches; ++i) ctx->Dispatch(gx, gy, 1);
    ctx->End(k.t1.Get());
    ctx->End(k.disjoint.Get());
    return k;
}
double readBlock(Device& d, Block& k, UINT dispatches) {
    ID3D11DeviceContext* ctx = d.ctx.Get();
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dd{};
    while (ctx->GetData(k.disjoint.Get(), &dd, sizeof(dd), 0) == S_FALSE) {}
    UINT64 a = 0, b = 0;
    while (ctx->GetData(k.t0.Get(), &a, sizeof(a), 0) == S_FALSE) {}
    while (ctx->GetData(k.t1.Get(), &b, sizeof(b), 0) == S_FALSE) {}
    check(!dd.Disjoint && dd.Frequency != 0 && b > a, "zscene gate: a valid, non-disjoint GPU timer");
    return static_cast<double>(b - a) / static_cast<double>(dd.Frequency) * 1000.0 / dispatches;
}
struct Paired { double baseMs = 0, candMs = 0, savedMs = 0, baseMin = 0, candMin = 0; unsigned positive = 0; };
Paired pairedRounds(Device& d, const std::function<void()>& bindA, const std::function<void()>& bindB, UINT gx, UINT gy) {
    // Warm-up: about 40 blocks' worth of both arms, queued with the measured blocks so the clocks are up when they start.
    std::vector<Block> warm;
    for (int i = 0; i < 40; ++i) warm.push_back(issueBlock(d, (i & 1) ? bindB : bindA, gx, gy, kPairedDispatches));
    std::vector<Block> blocks;
    std::vector<bool> isB;
    for (UINT round = 0; round < kPairedRounds; ++round)
        for (UINT order = 0; order < 4; ++order) {
            const bool b = (order == 1 || order == 2) != ((round & 1) != 0);
            blocks.push_back(issueBlock(d, b ? bindB : bindA, gx, gy, kPairedDispatches));
            isB.push_back(b);
        }
    d.ctx->ClearState();
    d.ctx->Flush();
    for (auto& k : warm) readBlock(d, k, kPairedDispatches);
    std::vector<double> base, cand, deltas;
    for (UINT round = 0; round < kPairedRounds; ++round) {
        double a[2] = {}, b[2] = {};
        for (UINT order = 0; order < 4; ++order) {
            const size_t at = round * 4 + order;
            const double ms = readBlock(d, blocks[at], kPairedDispatches);
            (isB[at] ? b : a)[order >= 2 ? 1 : 0] = ms;
            (isB[at] ? cand : base).push_back(ms);
        }
        deltas.push_back((a[0] + a[1] - b[0] - b[1]) * 0.5);
    }
    Paired p;
    p.baseMs = median(base); p.candMs = median(cand); p.savedMs = median(deltas);
    p.baseMin = *std::min_element(base.begin(), base.end()); p.candMin = *std::min_element(cand.begin(), cand.end());
    p.positive = static_cast<unsigned>(std::count_if(deltas.begin(), deltas.end(), [](double x) { return x > 0; }));
    return p;
}
// The fastest block of each arm beside the medians: another process's work on the GPU (a running game) only ever adds to a
// block, so the minimum is the figure least disturbed by it.
void printPaired(const char* name, const Paired& p) {
    std::printf("  %-58s %8.4f -> %8.4f  saved %+8.4f (%+5.1f%%)  %2u/%u rounds faster;  fastest %7.4f -> %7.4f (%+5.1f%%)\n", name,
                p.baseMs, p.candMs, p.savedMs, 100.0 * p.savedMs / p.baseMs, p.positive, kPairedRounds, p.baseMin, p.candMin,
                100.0 * (p.baseMin - p.candMin) / p.baseMin);
}

// The mv fixture (after RocketsProjects' no-smoke A/B): one eye at 2016x2016, depth bound, the head still, the jitter on and no
// previous depth; a scene of surfaces with sky and a few negative values (the null view's zero clamp), a sparse private-UI
// plane, a sparse smoke patch (bound only where an arm says); MV R16G16_FLOAT, ZC R32_FLOAT, MK R8_UNORM as the pass writes.
struct MvFixture {
    UINT w = 2016, h = 2016;
    ComPtr<ID3D11Buffer> cb[4];   // probe.w 0, 64, 320, 256
    ComPtr<ID3D11ShaderResourceView> scene, ui, smoke;
    ComPtr<ID3D11Texture2D> motion, depth, mask;
    ComPtr<ID3D11UnorderedAccessView> motionU, depthU, maskU;
};
MvFixture makeMvFixture(ID3D11Device* dev) {
    MvFixture f;
    const UINT w = f.w, h = f.h;
    // The diagnostic blob reflects the whole of cbuffer P (a fast blob may drop unused fields; the offsets are the same).
    ComPtr<ID3D11ShaderReflection> refl;
    check(SUCCEEDED(D3DReflect(edvr::kTemporalMvBytecode, sizeof(edvr::kTemporalMvBytecode), __uuidof(ID3D11ShaderReflection), &refl)),
          "zscene gate: mv reflection");
    auto* P = refl->GetConstantBufferByName("P");
    D3D11_SHADER_BUFFER_DESC pd{};
    check(SUCCEEDED(P->GetDesc(&pd)), "zscene gate: mv cbuffer P");
    std::vector<char> params(pd.Size, 0);
    size_t probeW = 0;
    auto put = [&](const char* name, const void* v, UINT bytes) {
        D3D11_SHADER_VARIABLE_DESC vd{};
        check(SUCCEEDED(P->GetVariableByName(name)->GetDesc(&vd)) && bytes <= vd.Size && vd.StartOffset + bytes <= params.size(),
              "zscene gate: mv cbuffer field");
        std::memcpy(params.data() + vd.StartOffset, v, bytes);
        if (!std::strcmp(name, "probe")) probeW = vd.StartOffset + 12;
    };
    auto f4 = [&](const char* name, float a, float b, float c, float d) { const float v[4] = {a, b, c, d}; put(name, v, 16); };
    const int region[4] = {0, 0, int(w), int(h)}, sz[2] = {int(w), int(h)};
    put("region", region, 16); put("size", sz, 8); put("texSize", sz, 8);
    f4("tanNow", -1, 1, -1, 1); f4("tanPrev", -1, 1, -1, 1);
    f4("dR0", 1, 0, 0, 0); f4("dR1", 0, 1, 0, 0); f4("dR2", 0, 0, 1, 0);
    f4("knobs", 0, 1, .025f, 0); f4("jit", .25f, -.375f, 0, .5f); f4("holoJitter", .25f, -.375f, 1, 0); f4("probe", 1, 0, 0, 0);
    const uint32_t words[4] = {0u, 64u, 320u, 256u};
    for (int k = 0; k < 4; ++k) {
        const float bits = static_cast<float>(words[k]);
        std::memcpy(params.data() + probeW, &bits, 4);
        D3D11_BUFFER_DESC bd{}; bd.ByteWidth = pd.Size; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        D3D11_SUBRESOURCE_DATA init{params.data(), 0, 0};
        check(SUCCEEDED(dev->CreateBuffer(&bd, &init, &f.cb[k])), "zscene gate: mv constants");
    }
    std::vector<float> scene(size_t(w) * h), ui(scene.size()), smoke(scene.size());
    for (UINT y = 0; y < h; ++y) for (UINT x = 0; x < w; ++x) {
        const size_t at = size_t(y) * w + x;
        scene[at] = ((x * 7 + y * 13) % 101 == 0) ? 0.0f : 0.006f + float((x * 17 + y * 19) % 139) / 1000.0f;
        if (x % 127 == 0 && y % 103 == 0) scene[at] = -0.125f;
        ui[at] = ((x / 32 + y / 16) % 7 == 0) ? 0.35f : 0.0f;
        smoke[at] = ((x / 16 + y / 16) % 9 == 0) ? 0.9f : 0.0f;
    }
    f.scene = srvOf(dev, tex2d(dev, w, h, DXGI_FORMAT_R32_FLOAT, D3D11_BIND_SHADER_RESOURCE, scene.data(), w * 4).Get());
    f.ui = srvOf(dev, tex2d(dev, w, h, DXGI_FORMAT_R32_FLOAT, D3D11_BIND_SHADER_RESOURCE, ui.data(), w * 4).Get());
    f.smoke = srvOf(dev, tex2d(dev, w, h, DXGI_FORMAT_R32_FLOAT, D3D11_BIND_SHADER_RESOURCE, smoke.data(), w * 4).Get());
    f.motion = tex2d(dev, w, h, DXGI_FORMAT_R16G16_FLOAT, D3D11_BIND_UNORDERED_ACCESS, nullptr, 0);
    f.depth = tex2d(dev, w, h, DXGI_FORMAT_R32_FLOAT, D3D11_BIND_UNORDERED_ACCESS, nullptr, 0);
    f.mask = tex2d(dev, w, h, DXGI_FORMAT_R8_UNORM, D3D11_BIND_UNORDERED_ACCESS, nullptr, 0);
    f.motionU = uavOf(dev, f.motion.Get()); f.depthU = uavOf(dev, f.depth.Get()); f.maskU = uavOf(dev, f.mask.Get());
    return f;
}
// One arm's dispatch state: the shader, which layers are bound, and the constants with that probe.w word (index into cb).
std::function<void()> mvArm(ID3D11DeviceContext* ctx, MvFixture& f, ID3D11ComputeShader* cs, bool t6, bool t7, int word) {
    return [ctx, &f, cs, t6, t7, word] {
        ctx->ClearState();
        ctx->CSSetConstantBuffers(0, 1, f.cb[word].GetAddressOf());
        ID3D11ShaderResourceView* srvs[8] = {nullptr, nullptr, f.scene.Get(), nullptr, nullptr, nullptr,
                                             t6 ? f.smoke.Get() : nullptr, t7 ? f.ui.Get() : nullptr};
        ctx->CSSetShaderResources(0, 8, srvs);
        ID3D11UnorderedAccessView* uavs[3] = {f.motionU.Get(), f.depthU.Get(), f.maskU.Get()};
        ctx->CSSetUnorderedAccessViews(3, 3, uavs, nullptr);
        ctx->CSSetShader(cs, nullptr, 0);
    };
}
// Every byte of MV, ZC and MK from one arm, outside any timing.
std::vector<uint32_t> mvOutputs(Device& d, MvFixture& f, const std::function<void()>& arm) {
    const float zero[4] = {};
    for (auto* u : {f.motionU.Get(), f.depthU.Get(), f.maskU.Get()}) d.ctx->ClearUnorderedAccessViewFloat(u, zero);
    arm();
    d.ctx->Dispatch((f.w + 7) / 8, (f.h + 7) / 8, 1);
    d.ctx->ClearState();
    std::vector<uint32_t> all = readU32(d.dev.Get(), d.ctx.Get(), f.motion.Get()), z = readU32(d.dev.Get(), d.ctx.Get(), f.depth.Get());
    all.insert(all.end(), z.begin(), z.end());
    // MK is one byte a texel: read it raw.
    D3D11_TEXTURE2D_DESC td{}; f.mask->GetDesc(&td);
    td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> stage;
    check(SUCCEEDED(d.dev->CreateTexture2D(&td, nullptr, &stage)), "zscene gate: mask staging");
    d.ctx->CopyResource(stage.Get(), f.mask.Get());
    D3D11_MAPPED_SUBRESOURCE m{};
    check(SUCCEEDED(d.ctx->Map(stage.Get(), 0, D3D11_MAP_READ, 0, &m)), "zscene gate: mask map");
    for (UINT y = 0; y < f.h; ++y) {
        const auto* row = static_cast<const unsigned char*>(m.pData) + size_t(y) * m.RowPitch;
        for (UINT x = 0; x < f.w; x += 4) all.push_back(uint32_t(row[x]) | uint32_t(row[x + 1]) << 8 | uint32_t(row[x + 2]) << 16 | uint32_t(row[x + 3]) << 24);
    }
    d.ctx->Unmap(stage.Get(), 0);
    return all;
}

// --bench: the motion prep (temporal_mv_fast_cs) with the smoke's depth absent, four arms against (a) the shader before the gate:
//   (b) the gate (production bytecode) with the absent bits set; (c) a compiled no-ZS variant, bench-only, the reference ideal
//   (RocketsProjects' expression: max(max(Z, 0), ZUI)), and at UI depth off (c') the same without ZUI either, what (b) skips there;
//   (d) the gate with the bits clear against the original, the smoke's depth BOUND (and the UI depth as the row says): the
//   common bound case, where the branches must cost nothing. Each output is checked byte for byte against (a) first.
void bench(Device& d) {
    static const D3D_SHADER_MACRO fast[] = {{"EDVR_TEMPORAL_DIAGNOSTICS", "0"}, {nullptr, nullptr}};
    const std::string hlsl = temporalHlsl(temporalSource());
    const auto span = zSceneSpan(hlsl);
    auto withZScene = [&](const char* fn) { return hlsl.substr(0, span.first) + fn + hlsl.substr(span.second); };
    const char* kNoZs =
        "float zSceneAt(int2 q) {\n"
        "    if((uint(probe.w+.5)&32u)!=0u) { float4 s=Screen.Load(int3(q,0));if(s.w>0)return s.z; }\n"
        "    return max(max(Z.Load(int3(q, 0)), 0.0f), ZUI.Load(int3(q, 0)));\n"
        "}\n";
    const char* kNoZsNoZui =
        "float zSceneAt(int2 q) {\n"
        "    if((uint(probe.w+.5)&32u)!=0u) { float4 s=Screen.Load(int3(q,0));if(s.w>0)return s.z; }\n"
        "    return max(max(Z.Load(int3(q, 0)), 0.0f), 0.0f);\n"
        "}\n";
    // The production way (tools/temporal_shader_build: cs_5_0, flags 0, the macro); the text as it is, compiled here, must be the
    // embedded production bytecode, or the variants beside it are not like-for-like references.
    auto newMv = compileEntry(hlsl, "mv", fast, 0);
    auto oldMv = compileEntry(withZScene(kOldZSceneAt), "mv", fast, 0);
    auto noZsMv = compileEntry(withZScene(kNoZs), "mv", fast, 0);
    auto noBothMv = compileEntry(withZScene(kNoZsNoZui), "mv", fast, 0);
    const bool sameAsEmbedded = newMv->GetBufferSize() == sizeof(edvr::kTemporalMvFastBytecode) &&
                                std::memcmp(newMv->GetBufferPointer(), edvr::kTemporalMvFastBytecode, newMv->GetBufferSize()) == 0;
    check(sameAsEmbedded, "zscene gate bench: the source compiled here is the embedded temporal_mv_fast_cs");
    auto gateCs = makeCs(d.dev.Get(), edvr::kTemporalMvFastBytecode, sizeof(edvr::kTemporalMvFastBytecode));
    auto origCs = makeCs(d.dev.Get(), oldMv->GetBufferPointer(), oldMv->GetBufferSize());
    auto noZsCs = makeCs(d.dev.Get(), noZsMv->GetBufferPointer(), noZsMv->GetBufferSize());
    auto noBothCs = makeCs(d.dev.Get(), noBothMv->GetBufferPointer(), noBothMv->GetBufferSize());
    MvFixture f = makeMvFixture(d.dev.Get());
    ID3D11DeviceContext* ctx = d.ctx.Get();
    const UINT gx = (f.w + 7) / 8, gy = (f.h + 7) / 8;
    enum { kBits0, kBits64, kBits320, kBits256 };
    // Correctness first, every byte of the full eye.
    for (bool ui : {false, true}) {
        const auto a = mvOutputs(d, f, mvArm(ctx, f, origCs.Get(), false, ui, kBits0));
        check(a == mvOutputs(d, f, mvArm(ctx, f, gateCs.Get(), false, ui, ui ? kBits64 : kBits320)), "zscene gate bench: (b) is (a) to the byte");
        check(a == mvOutputs(d, f, mvArm(ctx, f, noZsCs.Get(), false, ui, kBits0)), "zscene gate bench: (c) is (a) to the byte");
        if (!ui) check(a == mvOutputs(d, f, mvArm(ctx, f, noBothCs.Get(), false, false, kBits0)), "zscene gate bench: (c') is (a) to the byte");
        const auto bound = mvOutputs(d, f, mvArm(ctx, f, origCs.Get(), true, ui, kBits0));
        check(bound == mvOutputs(d, f, mvArm(ctx, f, gateCs.Get(), true, ui, kBits0)), "zscene gate bench: (d) is (a) to the byte, smoke bound");
        check(bound != mvOutputs(d, f, mvArm(ctx, f, noZsCs.Get(), true, ui, kBits0)),
              "zscene gate bench: negative control, the no-ZS variant drops a bound smoke depth");
    }
    std::printf("bench: MV/ZC/MK byte-identical to the original for (b), (c), (c'), (d); the no-ZS variant differs with the smoke bound\n");
    std::printf("== temporal_mv_fast_cs on %s, %ux%u one eye, paired ABBA/BAAB, %u rounds of 4 blocks of %u dispatches; ms per dispatch, "
                "median (a) -> median arm, median paired saving\n", d.name.c_str(), f.w, f.h, kPairedRounds, kPairedDispatches);
    for (bool ui : {false, true}) {
        std::printf(" UI depth %s, smoke absent:\n", ui ? "ON (t7 bound)" : "OFF (t7 null)");
        const auto a = mvArm(ctx, f, origCs.Get(), false, ui, kBits0);
        printPaired(ui ? "(b) gate, bit 64 (smoke skipped)" : "(b) gate, bits 64|256 (smoke and UI skipped)",
                    pairedRounds(d, a, mvArm(ctx, f, gateCs.Get(), false, ui, ui ? kBits64 : kBits320), gx, gy));
        printPaired("(c) compiled no-ZS variant (reference, not shipped)", pairedRounds(d, a, mvArm(ctx, f, noZsCs.Get(), false, ui, kBits0), gx, gy));
        if (!ui)
            printPaired("(c') compiled no-ZS, no-ZUI variant (reference)", pairedRounds(d, a, mvArm(ctx, f, noBothCs.Get(), false, false, kBits0), gx, gy));
        printPaired(ui ? "(d) gate, bits clear, smoke and UI bound" : "(d) gate, bits clear, smoke bound",
                    pairedRounds(d, mvArm(ctx, f, origCs.Get(), true, ui, kBits0), mvArm(ctx, f, gateCs.Get(), true, ui, kBits0), gx, gy));
    }
    if (d.warp) std::puts("note: --bench on WARP times a CPU; use --adapter nvidia.");
}

// The by-hand flags after the mode word: --adapter nvidia, --bench. False on anything else (usage).
bool aloneFlags(int argc, char** argv, bool& nvidia, bool& wantBench) {
    nvidia = wantBench = false;
    for (int i = 2; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--bench")) wantBench = true;
        else if (!std::strcmp(argv[i], "--adapter") && i + 1 < argc && !std::strcmp(argv[i + 1], "nvidia")) { nvidia = true; ++i; }
        else return false;
    }
    return true;
}
// ui_depth_test --zscene-gate [--adapter nvidia] [--bench]: this alone, on the adapter asked for.
int runAlone(int argc, char** argv) {
    bool nvidia = false, wantBench = false;
    if (!aloneFlags(argc, argv, nvidia, wantBench)) { std::puts("usage: ui_depth_test --zscene-gate [--adapter nvidia] [--bench]"); return 2; }
    cpuHalf();
    Device d = makeDevice(nvidia);
    parity(d.dev.Get(), d.ctx.Get(), d.name.c_str());
    if (wantBench) bench(d);
    return 0;
}

}  // namespace zgate
