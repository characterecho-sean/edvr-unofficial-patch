#pragma once
// corpus_identity: does the patched pixel shader write the game's own
// G-buffer targets bit-identically to the stock one? Run on WARP against the
// REAL game bytecode (a local edvr_logs dump; the game's shaders are not in
// the repository).
//
// For one (stock PS, patched PS) pair:
//  - a synthetic vertex shader exports every input the PATCHED PS reads, at
//    the same register, components, semantic and type (D3D11 links the stages
//    by register): a full-screen triangle at z = 0.5, floats as distinct
//    linear ramps over the screen, the slot input as 0x80000005 (flag bit 31,
//    slot 5), other integers small. Its output signature is reflected and must
//    cover both PSs' inputs, or the pair is skipped with the reason;
//  - every resource either PS declares gets a patterned dummy at its slot.
//    The game's shaders carry no RDEF chunk, so reflection names no bound
//    resource: the declarations are read from the disassembly;
//  - both PSs draw the same triangle into cleared 64x64 targets that keep
//    every bit, under two data sets (the second has zeros and small integers
//    in the buffers, so both sides of the shaders' branches run), and
//    SV_Target0..3 plus the depth buffer must match byte for byte;
//  - MRT6 must hold 11.0 (2 * 5 + 1) and the fragment's own depth (0.5, the
//    depth-buffer bits) on every texel the draw covered.

#include <d3d11.h>
#include <d3d11shader.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../../src/d3d11/dxbc_engine_velocity.h"

namespace corpus_identity {
using Microsoft::WRL::ComPtr;

struct Result {
    unsigned targets = 0, texelsCompared = 0, mismatches = 0, covered = 0, slotChecked = 0, slotBad = 0;
    bool driven = false;
    std::string skipped;
};

namespace detail {

constexpr unsigned kSize = 64;               // the targets' width and height
constexpr uint32_t kSlotWord = 0x80000005u;  // the slot input: flag bit 31, slot 5
constexpr float kSlotCode = 11.0f;           // 2 * 5 + 1
constexpr float kDepth = 0.5f;               // the triangle's z

// One PS input element: its register, first component and width (from the
// lowest to the highest bit of its mask), type and semantic.
struct Element {
    std::string semantic;
    unsigned index = 0, reg = 0, first = 0, width = 0;
    D3D_REGISTER_COMPONENT_TYPE type = D3D_REGISTER_COMPONENT_FLOAT32;
    D3D_NAME sv = D3D_NAME_UNDEFINED;
};

inline bool reflect(const void* bytes, size_t size, ComPtr<ID3D11ShaderReflection>& r, D3D11_SHADER_DESC& desc) {
    return SUCCEEDED(D3DReflect(bytes, size, IID_PPV_ARGS(&r))) && SUCCEEDED(r->GetDesc(&desc));
}

// The PS's input signature in (register, first component) order, less the
// values the rasterizer generates rather than a vertex shader exports.
inline bool inputs(const std::vector<BYTE>& ps, std::vector<Element>& out, std::string& why) {
    ComPtr<ID3D11ShaderReflection> r;
    D3D11_SHADER_DESC desc{};
    if (!reflect(ps.data(), ps.size(), r, desc)) {
        why = "PS does not reflect";
        return false;
    }
    out.clear();
    for (UINT i = 0; i < desc.InputParameters; ++i) {
        D3D11_SIGNATURE_PARAMETER_DESC p{};
        r->GetInputParameterDesc(i, &p);
        if (p.SystemValueType == D3D_NAME_IS_FRONT_FACE || p.SystemValueType == D3D_NAME_SAMPLE_INDEX ||
            p.SystemValueType == D3D_NAME_PRIMITIVE_ID || !(p.Mask & 15u))
            continue;
        Element e;
        e.semantic = p.SemanticName;
        e.index = p.SemanticIndex;
        e.reg = p.Register;
        unsigned high = 3;
        while (!(p.Mask & (1u << high))) --high;
        while (!(p.Mask & (1u << e.first))) ++e.first;
        e.width = high - e.first + 1;
        e.type = p.ComponentType;
        e.sv = p.SystemValueType;
        if (e.sv == D3D_NAME_POSITION) e.first = 0, e.width = 4;
        out.push_back(e);
    }
    std::sort(out.begin(), out.end(), [](const Element& a, const Element& b) {
        return a.reg != b.reg ? a.reg < b.reg : a.first < b.first;
    });
    return true;
}

inline std::string typeName(D3D_REGISTER_COMPONENT_TYPE type, unsigned width) {
    const std::string t = type == D3D_REGISTER_COMPONENT_UINT32   ? "uint"
                          : type == D3D_REGISTER_COMPONENT_SINT32 ? "int"
                                                                  : "float";
    return width > 1 ? t + static_cast<char>('0' + width) : t;
}

// One component's value: floats are a distinct linear ramp over the screen
// (uv in 0..1 across the viewport, values about -1.75..2.25), the slot
// component is kSlotWord, other integers are small, padding is zero.
inline std::string value(const Element& e, unsigned c, bool pad, const edvr::EngineVelocityInputs& in) {
    char s[96];
    const unsigned k = e.reg * 4 + e.first + c;
    if (e.type != D3D_REGISTER_COMPONENT_FLOAT32) {
        const bool slot = !pad && e.reg == in.identityRegister && e.first + c == in.identityComponent;
        const unsigned v = pad ? 0u : slot ? kSlotWord : 1u + k % 3u;
        if (e.type == D3D_REGISTER_COMPONENT_UINT32) std::snprintf(s, sizeof s, "0x%Xu", v);
        else std::snprintf(s, sizeof s, "%u", v);
        return s;
    }
    if (pad) return "0.0";
    const float a = -0.75f + 0.05f * float((k * 7 + 3) % 31), b = -0.75f + 0.05f * float((k * 11 + 17) % 31),
                z = -0.25f + 0.05f * float((k * 13 + 5) % 21);
    std::snprintf(s, sizeof s, "(%.6f + %.6f * uv.x + %.6f * uv.y)", z, a, b);
    return s;
}

// The synthetic VS: one member per PS input element in register order. A
// register's gaps are filled with EDVRPAD<n> members of the register's type,
// and every register but the last is filled to w, so the compiler's in-order
// packing puts each element exactly where the PS reads it.
inline std::string vsSource(const std::vector<Element>& ps, const edvr::EngineVelocityInputs& in) {
    std::string members, body;
    unsigned n = 0, pads = 0;
    auto emit = [&](const Element& e, bool pad) {
        const std::string type = typeName(e.type, e.width);
        const std::string semantic = pad                         ? "EDVRPAD" + std::to_string(pads++)
                                     : e.sv == D3D_NAME_POSITION ? std::string("SV_Position")
                                                                 : e.semantic + std::to_string(e.index);
        members += "    " + type + " m" + std::to_string(n) + " : " + semantic + ";\n";
        std::string v = type + "(";
        if (e.sv == D3D_NAME_POSITION && !pad) v = "float4(uv * float2(2, -2) + float2(-1, 1), 0.5, 1";
        else
            for (unsigned c = 0; c < e.width; ++c) v += (c ? ", " : "") + value(e, c, pad, in);
        body += "    o.m" + std::to_string(n++) + " = " + v + ");\n";
    };
    const unsigned last = ps.empty() ? 0 : ps.back().reg;
    size_t i = 0;
    for (unsigned reg = 0; reg <= last; ++reg) {
        const D3D_REGISTER_COMPONENT_TYPE type =
            i < ps.size() && ps[i].reg == reg ? ps[i].type : D3D_REGISTER_COMPONENT_FLOAT32;
        Element pad;
        pad.reg = reg;
        pad.type = type;
        unsigned next = 0;
        for (; i < ps.size() && ps[i].reg == reg; ++i) {
            if (ps[i].first > next) {
                pad.first = next;
                pad.width = ps[i].first - next;
                emit(pad, true);
            }
            emit(ps[i], false);
            next = ps[i].first + ps[i].width;
        }
        if (next < 4 && reg < last) {
            pad.first = next;
            pad.width = 4 - next;
            emit(pad, true);
        }
    }
    return "struct VsOut {\n" + members +
           "};\nVsOut main(uint id : SV_VertexID) {\n    const float2 uv = float2((id << 1) & 2, id & 2);\n"
           "    VsOut o;\n" + body + "    return o;\n}\n";
}

inline ComPtr<ID3DBlob> compileVs(const std::string& source, std::string& why) {
    ComPtr<ID3DBlob> code, errors;
    if (FAILED(D3DCompile(source.data(), source.size(), "corpus-identity-vs", nullptr, nullptr, "main", "vs_5_0",
                          D3DCOMPILE_SKIP_OPTIMIZATION, 0, &code, &errors))) {
        why = "synthetic VS does not compile: " +
              (errors ? std::string(static_cast<const char*>(errors->GetBufferPointer())) : std::string("?"));
        return nullptr;
    }
    return code;
}

// Every element the PS reads must leave the VS at the same register, name,
// index and type, with a mask covering it.
inline bool covers(ID3DBlob* vs, const std::vector<Element>& ps, std::string& why) {
    ComPtr<ID3D11ShaderReflection> r;
    D3D11_SHADER_DESC desc{};
    if (!reflect(vs->GetBufferPointer(), vs->GetBufferSize(), r, desc)) {
        why = "synthetic VS does not reflect";
        return false;
    }
    for (const auto& e : ps) {
        const unsigned mask = ((1u << e.width) - 1u) << e.first;
        bool found = false;
        for (UINT i = 0; i < desc.OutputParameters && !found; ++i) {
            D3D11_SIGNATURE_PARAMETER_DESC o{};
            r->GetOutputParameterDesc(i, &o);
            found = o.Register == e.reg && o.SemanticIndex == e.index && (o.Mask & mask) == mask &&
                    o.ComponentType == e.type && _stricmp(o.SemanticName, e.semantic.c_str()) == 0;
        }
        if (!found) {
            why = "synthetic VS does not export " + e.semantic + std::to_string(e.index) + " at v" +
                  std::to_string(e.reg) + "." + "xyzw"[e.first];
            return false;
        }
    }
    return true;
}

// One declared resource: 'b' a constant buffer (size = float4 count), 's' a
// sampler (kind = its mode), 't' a shader resource (kind = its dimension,
// ret = its return type, size = a structure's stride).
struct Decl {
    char space = 0;
    unsigned slot = 0, size = 0;
    std::string kind, ret;
};

// The declarations, read from the disassembly. A UAV, or a dimension this rig
// has no dummy for, declines the pair with the line that stopped it.
inline bool declarations(const std::vector<BYTE>& ps, std::vector<Decl>& out, std::string& why) {
    ComPtr<ID3DBlob> text;
    if (FAILED(D3DDisassemble(ps.data(), ps.size(), 0, nullptr, &text))) {
        why = "PS does not disassemble";
        return false;
    }
    const std::string all(static_cast<const char*>(text->GetBufferPointer()), text->GetBufferSize());
    for (size_t pos = 0; pos < all.size();) {
        size_t end = all.find('\n', pos);
        if (end == std::string::npos) end = all.size();
        std::string line = all.substr(pos, end - pos);
        pos = end + 1;
        const size_t lead = line.find_first_not_of(" \t");
        if (lead == std::string::npos || line.compare(lead, 4, "dcl_") != 0) continue;
        line.erase(0, lead);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\0')) line.pop_back();
        Decl d;
        char kind[32] = {}, ret[32] = {};
        const char* l = line.c_str();
        if (line.rfind("dcl_uav", 0) == 0 || line.rfind("dcl_tgsm", 0) == 0) {
            why = "PS declares a UAV or shared memory: " + line;
            return false;
        }
        if (sscanf_s(l, "dcl_constantbuffer %*[cCbB]%u[%u]", &d.slot, &d.size) == 2) {
            d.space = 'b';
        } else if (sscanf_s(l, "dcl_sampler s%u, mode_%31s", &d.slot, kind, unsigned(sizeof kind)) == 2) {
            d.space = 's';
            d.kind = kind;
        } else if (sscanf_s(l, "dcl_resource_structured t%u, %u", &d.slot, &d.size) == 2) {
            d.space = 't';
            d.kind = "structured";
        } else if (sscanf_s(l, "dcl_resource_raw t%u", &d.slot) == 1) {
            d.space = 't';
            d.kind = "raw";
        } else if (line.rfind("dcl_resource_", 0) == 0) {
            if (sscanf_s(l, "dcl_resource_%31s (%31[a-z]%*[^)]) t%u", kind, unsigned(sizeof kind), ret,
                         unsigned(sizeof ret), &d.slot) != 3) {
                why = "unparsed declaration: " + line;
                return false;
            }
            d.space = 't';
            d.kind = kind;
            d.ret = ret;
            if (d.kind != "texture2d" && d.kind != "texture2darray" && d.kind != "texturecube" &&
                d.kind != "texture3d" && d.kind != "buffer") {
                why = "no dummy for: " + line;
                return false;
            }
        } else {
            continue;
        }
        bool seen = false;
        for (const auto& e : out) seen = seen || (e.space == d.space && e.slot == d.slot);
        if (!seen) out.push_back(d);
    }
    return true;
}

inline uint32_t mix(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    return x ^ (x >> 16);
}

// Test data as 32-bit words for a format (UNKNOWN: constant, structured and
// raw data). Set 0: floats in 0.1..1.5, halves in 0.125..2, integers 0..15.
// Set 1: a quarter zeros, a quarter small integers, the rest floats in
// -1.5..1.5; halves take random signs.
inline std::vector<uint32_t> words(DXGI_FORMAT format, unsigned seed, size_t count, uint32_t salt) {
    std::vector<uint32_t> out(count);
    for (size_t i = 0; i < count; ++i) {
        const uint32_t h = mix(uint32_t(i) * 2654435761u + salt * 0x9e3779b9u + seed * 0x85ebca6bu + 1u);
        uint32_t w = h;
        if (format == DXGI_FORMAT_R16G16B16A16_FLOAT) {
            w = 0x30003000u | (h & 0x0fff0fffu) | (seed ? h & 0x80008000u : 0u);
        } else if (format == DXGI_FORMAT_R32G32B32A32_UINT || format == DXGI_FORMAT_R32G32B32A32_SINT) {
            w = h & 15u;
        } else if (format == DXGI_FORMAT_R8G8B8A8_UNORM || format == DXGI_FORMAT_R8G8B8A8_SNORM) {
            w = h;
        } else if (seed && (h & 3u) == 0u) {
            w = 0u;
        } else if (seed && (h & 3u) == 1u) {
            w = (h >> 8) & 7u;
        } else {
            const float unit = float(h >> 8) / 16777216.0f;
            const float f = seed ? -1.5f + 3.0f * unit : 0.1f + 1.4f * unit;
            std::memcpy(&w, &f, 4);
        }
        out[i] = w;
    }
    return out;
}

inline DXGI_FORMAT srvFormat(const std::string& ret) {
    if (ret == "uint") return DXGI_FORMAT_R32G32B32A32_UINT;
    if (ret == "sint") return DXGI_FORMAT_R32G32B32A32_SINT;
    if (ret == "unorm") return DXGI_FORMAT_R8G8B8A8_UNORM;
    if (ret == "snorm") return DXGI_FORMAT_R8G8B8A8_SNORM;
    return DXGI_FORMAT_R16G16B16A16_FLOAT;
}

inline unsigned texelBytes(DXGI_FORMAT f) {
    if (f == DXGI_FORMAT_R16G16B16A16_FLOAT) return 8;
    return f == DXGI_FORMAT_R8G8B8A8_UNORM || f == DXGI_FORMAT_R8G8B8A8_SNORM ? 4u : 16u;
}

// Creates the dummy for one declaration from data set `seed` and binds it at
// its slot. Textures are 16x16 (3D: 8^3) with full mips, patterned per level
// and slice; arrays have 4 slices; buffers 16 structures, 64 typed elements
// or 1 KB raw; samplers are linear wrap (comparison: LESS_EQUAL).
inline bool bind(ID3D11Device* dev, ID3D11DeviceContext* ctx, const Decl& d, unsigned seed, std::string& why) {
    const uint32_t salt = uint32_t(d.space) * 64u + d.slot;
    why = std::string("dummy for ") + d.space + std::to_string(d.slot) + " not created";
    if (d.space == 'b') {
        const auto data = words(DXGI_FORMAT_UNKNOWN, seed, size_t(d.size) * 4u, salt);
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = d.size * 16u;
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        const D3D11_SUBRESOURCE_DATA init{data.data(), 0, 0};
        ComPtr<ID3D11Buffer> b;
        if (!d.size || d.size > 4096 || FAILED(dev->CreateBuffer(&bd, &init, &b))) return false;
        ctx->PSSetConstantBuffers(d.slot, 1, b.GetAddressOf());
        return true;
    }
    if (d.space == 's') {
        const bool comparison = d.kind == "comparison";
        D3D11_SAMPLER_DESC sd{};
        sd.Filter = comparison ? D3D11_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR : D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
        sd.MaxAnisotropy = 1;
        sd.ComparisonFunc = comparison ? D3D11_COMPARISON_LESS_EQUAL : D3D11_COMPARISON_NEVER;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        ComPtr<ID3D11SamplerState> s;
        if (FAILED(dev->CreateSamplerState(&sd, &s))) return false;
        ctx->PSSetSamplers(d.slot, 1, s.GetAddressOf());
        return true;
    }
    ComPtr<ID3D11Resource> res;
    D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
    const bool buffer = d.kind == "structured" || d.kind == "raw" || d.kind == "buffer";
    const DXGI_FORMAT f = d.kind == "structured" || d.kind == "raw" ? DXGI_FORMAT_UNKNOWN : srvFormat(d.ret);
    if (buffer) {
        const unsigned stride = d.kind == "structured" ? d.size : d.kind == "raw" ? 4u : texelBytes(f);
        const unsigned count = d.kind == "structured" ? 16u : d.kind == "raw" ? 256u : 64u;
        if (!stride || stride % 4u) return false;
        const auto data = words(f, seed, size_t(stride) * count / 4u, salt);
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = stride * count;
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (d.kind == "structured") bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, bd.StructureByteStride = stride;
        if (d.kind == "raw") bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
        const D3D11_SUBRESOURCE_DATA init{data.data(), 0, 0};
        ComPtr<ID3D11Buffer> b;
        if (FAILED(dev->CreateBuffer(&bd, &init, &b))) return false;
        res = b;
        vd.Format = d.kind == "raw" ? DXGI_FORMAT_R32_TYPELESS : f;
        vd.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
        vd.BufferEx.NumElements = count;
        vd.BufferEx.Flags = d.kind == "raw" ? D3D11_BUFFEREX_SRV_FLAG_RAW : 0u;
    } else {
        const bool cube = d.kind == "texturecube", volume = d.kind == "texture3d";
        const unsigned size = volume ? 8u : 16u, mips = volume ? 4u : 5u, bpp = texelBytes(f);
        const unsigned layers = cube ? 6u : d.kind == "texture2darray" ? 4u : 1u;
        std::vector<std::vector<uint32_t>> data;
        std::vector<D3D11_SUBRESOURCE_DATA> init;
        for (unsigned layer = 0; layer < layers; ++layer)
            for (unsigned m = 0; m < mips; ++m) {
                const unsigned w = size >> m;
                data.push_back(words(f, seed, size_t(w) * w * (volume ? w : 1u) * bpp / 4u, salt * 131u + layer * 8u + m));
            }
        for (size_t i = 0; i < data.size(); ++i) {
            const unsigned w = size >> (i % mips);
            init.push_back({data[i].data(), w * bpp, w * w * bpp});
        }
        HRESULT hr = E_FAIL;
        if (volume) {
            D3D11_TEXTURE3D_DESC td{};
            td.Width = td.Height = td.Depth = size;
            td.MipLevels = mips;
            td.Format = f;
            td.Usage = D3D11_USAGE_DEFAULT;
            td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            ComPtr<ID3D11Texture3D> t;
            hr = dev->CreateTexture3D(&td, init.data(), &t);
            res = t;
        } else {
            D3D11_TEXTURE2D_DESC td{};
            td.Width = td.Height = size;
            td.MipLevels = mips;
            td.ArraySize = layers;
            td.Format = f;
            td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_DEFAULT;
            td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            td.MiscFlags = cube ? D3D11_RESOURCE_MISC_TEXTURECUBE : 0u;
            ComPtr<ID3D11Texture2D> t;
            hr = dev->CreateTexture2D(&td, init.data(), &t);
            res = t;
        }
        if (FAILED(hr)) return false;
        vd.Format = f;
        vd.ViewDimension = cube ? D3D11_SRV_DIMENSION_TEXTURECUBE
                           : volume ? D3D11_SRV_DIMENSION_TEXTURE3D
                           : layers > 1 ? D3D11_SRV_DIMENSION_TEXTURE2DARRAY
                                        : D3D11_SRV_DIMENSION_TEXTURE2D;
        vd.Texture2DArray.MipLevels = mips;   // MipLevels sits at the same offset in every texture view
        vd.Texture2DArray.ArraySize = layers;
    }
    ComPtr<ID3D11ShaderResourceView> srv;
    if (FAILED(dev->CreateShaderResourceView(res.Get(), &vd, &srv))) return false;
    ctx->PSSetShaderResources(d.slot, 1, srv.GetAddressOf());
    why.clear();
    return true;
}

// SV_Target0..3 of the stock PS, each as a 128-bit format that keeps every
// bit the shader wrote (UNKNOWN where not declared).
inline bool outputs(const std::vector<BYTE>& ps, DXGI_FORMAT (&formats)[4], std::string& why) {
    ComPtr<ID3D11ShaderReflection> r;
    D3D11_SHADER_DESC desc{};
    if (!reflect(ps.data(), ps.size(), r, desc)) {
        why = "PS does not reflect";
        return false;
    }
    bool any = false;
    for (UINT i = 0; i < desc.OutputParameters; ++i) {
        D3D11_SIGNATURE_PARAMETER_DESC p{};
        r->GetOutputParameterDesc(i, &p);
        if (p.SystemValueType != D3D_NAME_TARGET || p.Register > 3) continue;
        formats[p.Register] = p.ComponentType == D3D_REGISTER_COMPONENT_UINT32   ? DXGI_FORMAT_R32G32B32A32_UINT
                              : p.ComponentType == D3D_REGISTER_COMPONENT_SINT32 ? DXGI_FORMAT_R32G32B32A32_SINT
                                                                                 : DXGI_FORMAT_R32G32B32A32_FLOAT;
        any = true;
    }
    if (!any) why = "PS writes none of SV_Target0..3";
    return any;
}

// The clear a game target starts from, and the same as a texel's bytes.
inline const float* clearColor(DXGI_FORMAT f) {
    static const float positive[4] = {12345.0f, 12345.0f, 12345.0f, 12345.0f};
    static const float negative[4] = {-12345.0f, -12345.0f, -12345.0f, -12345.0f};
    return f == DXGI_FORMAT_R32G32B32A32_UINT ? positive : negative;
}

inline void clearTexel(DXGI_FORMAT f, BYTE (&out)[16]) {
    const float c = clearColor(f)[0];
    const uint32_t word = f == DXGI_FORMAT_R32G32B32A32_FLOAT ? 0u : uint32_t(int32_t(c));
    for (unsigned i = 0; i < 4; ++i) {
        if (f == DXGI_FORMAT_R32G32B32A32_FLOAT) std::memcpy(out + 4 * i, &c, 4);
        else std::memcpy(out + 4 * i, &word, 4);
    }
}

inline ComPtr<ID3D11Texture2D> texture(ID3D11Device* dev, DXGI_FORMAT f, UINT bind) {
    D3D11_TEXTURE2D_DESC d{};
    d.Width = d.Height = kSize;
    d.MipLevels = d.ArraySize = 1;
    d.Format = f;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = bind;
    ComPtr<ID3D11Texture2D> t;
    if (FAILED(dev->CreateTexture2D(&d, nullptr, &t))) return nullptr;
    return t;
}

// A staging copy of a whole kSize x kSize texture, rows packed.
inline std::vector<BYTE> readback(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11Texture2D* tex, unsigned bytes) {
    D3D11_TEXTURE2D_DESC d{};
    tex->GetDesc(&d);
    d.Usage = D3D11_USAGE_STAGING;
    d.BindFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(dev->CreateTexture2D(&d, nullptr, &staging))) return {};
    ctx->CopyResource(staging.Get(), tex);
    if (FAILED(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m))) return {};
    std::vector<BYTE> out(size_t(kSize) * kSize * bytes);
    for (unsigned y = 0; y < kSize; ++y)
        std::memcpy(&out[size_t(y) * kSize * bytes], static_cast<const BYTE*>(m.pData) + size_t(y) * m.RowPitch,
                    size_t(kSize) * bytes);
    ctx->Unmap(staging.Get(), 0);
    return out;
}

// One draw's readback: SV_Target0..3 (empty where not declared), the depth
// buffer and, for the patched draw, MRT6.
struct Frame {
    std::vector<BYTE> target[4], depth, slot;
};

// Draws the full-screen triangle with `ps` into freshly cleared targets:
// o0..o3 at the stock formats, MRT6 (R32G32_FLOAT, cleared to (-1, 0)) when
// `slot`, depth D32 cleared to 0 with ALWAYS and writes on; no blend state,
// no culling. The resources are already bound.
inline bool draw(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11VertexShader* vs, ID3D11PixelShader* ps,
                 const DXGI_FORMAT (&formats)[4], bool slot, Frame& out) {
    ComPtr<ID3D11Texture2D> tex[7];
    ComPtr<ID3D11RenderTargetView> rtv[7];
    ID3D11RenderTargetView* views[7] = {};
    const float slotClear[4] = {-1.0f, 0.0f, 0.0f, 0.0f};
    for (unsigned i = 0; i < 7; ++i) {
        const DXGI_FORMAT f = i < 4 ? formats[i] : i == 6 && slot ? DXGI_FORMAT_R32G32_FLOAT : DXGI_FORMAT_UNKNOWN;
        if (f == DXGI_FORMAT_UNKNOWN) continue;
        tex[i] = texture(dev, f, D3D11_BIND_RENDER_TARGET);
        if (!tex[i] || FAILED(dev->CreateRenderTargetView(tex[i].Get(), nullptr, &rtv[i]))) return false;
        ctx->ClearRenderTargetView(rtv[i].Get(), i == 6 ? slotClear : clearColor(f));
        views[i] = rtv[i].Get();
    }
    const ComPtr<ID3D11Texture2D> depth = texture(dev, DXGI_FORMAT_R32_TYPELESS, D3D11_BIND_DEPTH_STENCIL);
    D3D11_DEPTH_STENCIL_VIEW_DESC dd{};
    dd.Format = DXGI_FORMAT_D32_FLOAT;
    dd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    D3D11_DEPTH_STENCIL_DESC ds{};
    ds.DepthEnable = TRUE;
    ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    ds.DepthFunc = D3D11_COMPARISON_ALWAYS;
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    ComPtr<ID3D11DepthStencilView> dsv;
    ComPtr<ID3D11DepthStencilState> dss;
    ComPtr<ID3D11RasterizerState> rs;
    if (!depth || FAILED(dev->CreateDepthStencilView(depth.Get(), &dd, &dsv)) ||
        FAILED(dev->CreateDepthStencilState(&ds, &dss)) || FAILED(dev->CreateRasterizerState(&rd, &rs)))
        return false;
    ctx->ClearDepthStencilView(dsv.Get(), D3D11_CLEAR_DEPTH, 0.0f, 0);
    const D3D11_VIEWPORT vp{0.0f, 0.0f, float(kSize), float(kSize), 0.0f, 1.0f};
    ctx->OMSetRenderTargets(7, views, dsv.Get());
    ctx->OMSetBlendState(nullptr, nullptr, 0xffffffffu);
    ctx->OMSetDepthStencilState(dss.Get(), 0);
    ctx->RSSetState(rs.Get());
    ctx->RSSetViewports(1, &vp);
    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(vs, nullptr, 0);
    ctx->PSSetShader(ps, nullptr, 0);
    ctx->Draw(3, 0);
    ctx->OMSetRenderTargets(0, nullptr, nullptr);
    for (unsigned i = 0; i < 4; ++i)
        if (tex[i]) out.target[i] = readback(dev, ctx, tex[i].Get(), 16);
    out.depth = readback(dev, ctx, depth.Get(), 4);
    if (slot) out.slot = readback(dev, ctx, tex[6].Get(), 8);
    return !out.depth.empty() && (!slot || !out.slot.empty());
}

// How many distinct texels a readback holds: a target the inputs never reach
// is uniform, and an identity over uniform targets proves little.
inline size_t distinct(const std::vector<BYTE>& v, unsigned bytes) {
    std::vector<std::string> texels;
    for (size_t at = 0; at + bytes <= v.size(); at += bytes)
        texels.emplace_back(reinterpret_cast<const char*>(&v[at]), bytes);
    std::sort(texels.begin(), texels.end());
    return size_t(std::unique(texels.begin(), texels.end()) - texels.begin());
}

} // namespace detail

// Draws the stock and the patched PS over the same inputs and resources, under
// two data sets, and compares what they wrote: SV_Target0..3 and depth byte
// for byte, MRT6 against the slot code and the fragment's depth. A pair the rig
// cannot drive faithfully is skipped with the reason, never passed.
inline Result compare(ID3D11Device* dev, ID3D11DeviceContext* ctx, const std::vector<BYTE>& stockPs,
                      const std::vector<BYTE>& patchedPs, const edvr::EngineVelocityInputs& in,
                      void (*check)(bool, const char*), const char* name) {
    using namespace detail;
    Result r;
    std::string why;
    std::vector<Element> patchedIn, stockIn;
    std::vector<Decl> decls;
    DXGI_FORMAT formats[4] = {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN};
    ComPtr<ID3DBlob> vsCode;
    const bool ready = inputs(patchedPs, patchedIn, why) && inputs(stockPs, stockIn, why) &&
                       (vsCode = compileVs(vsSource(patchedIn, in), why)).Get() != nullptr &&
                       covers(vsCode.Get(), patchedIn, why) && covers(vsCode.Get(), stockIn, why) &&
                       declarations(stockPs, decls, why) && declarations(patchedPs, decls, why) &&
                       outputs(stockPs, formats, why);
    auto skip = [&](const std::string& reason) {
        ctx->ClearState();
        r = Result{};
        r.skipped = reason;
        std::printf("  identity: %s: skipped: %s\n", name, reason.c_str());
        return r;
    };
    if (!ready) return skip(why);
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> stock, patched;
    check(SUCCEEDED(dev->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(), nullptr, &vs)) &&
              SUCCEEDED(dev->CreatePixelShader(stockPs.data(), stockPs.size(), nullptr, &stock)) &&
              SUCCEEDED(dev->CreatePixelShader(patchedPs.data(), patchedPs.size(), nullptr, &patched)),
          "identity: the synthetic VS and both PSs created");
    unsigned first = 0;   // coverage is read from the first declared target: o0 in every real family
    while (formats[first] == DXGI_FORMAT_UNKNOWN) ++first;
    BYTE clear[16];
    clearTexel(formats[first], clear);
    const char* const labels[5] = {"o0", "o1", "o2", "o3", "depth"};
    auto word = [](const std::vector<BYTE>& v, size_t at) {
        uint32_t w;
        std::memcpy(&w, &v[at], 4);
        return w;
    };
    Frame base;               // data set 0's stock frame
    size_t varied[4] = {};    // distinct stock texels per target, data set 0
    unsigned changed = 0;     // stock texels data set 1 changed
    for (unsigned seed = 0; seed < 2; ++seed) {
        ctx->ClearState();
        for (const auto& d : decls)
            if (!bind(dev, ctx, d, seed, why)) return skip(why);
        Frame a, b;
        check(draw(dev, ctx, vs.Get(), stock.Get(), formats, false, a) &&
                  draw(dev, ctx, vs.Get(), patched.Get(), formats, true, b),
              "identity: both draws ran and read back");
        for (unsigned t = 0; t < 5; ++t) {
            const std::vector<BYTE>& x = t < 4 ? a.target[t] : a.depth;
            const std::vector<BYTE>& y = t < 4 ? b.target[t] : b.depth;
            if (x.empty()) continue;
            check(x.size() == y.size(), "identity: both draws read back the same target");
            const unsigned bytes = t < 4 ? 16u : 4u;
            r.targets += seed == 0 && t < 4 ? 1u : 0u;
            for (unsigned p = 0; p < kSize * kSize; ++p) {
                ++r.texelsCompared;
                const size_t at = size_t(p) * bytes;
                if (std::memcmp(&x[at], &y[at], bytes) == 0 || r.mismatches++ >= 8) continue;
                std::printf("    %s: data set %u, %s differs at (%u,%u): stock", name, seed, labels[t], p % kSize,
                            p / kSize);
                for (unsigned w = 0; w < bytes; w += 4) std::printf(" %08x", word(x, at + w));
                std::printf(", patched");
                for (unsigned w = 0; w < bytes; w += 4) std::printf(" %08x", word(y, at + w));
                std::printf("\n");
            }
        }
        for (unsigned p = 0; p < kSize * kSize; ++p) {
            if (std::memcmp(&a.target[first][size_t(p) * 16], clear, 16) != 0) ++r.covered;
            if (std::memcmp(&b.target[first][size_t(p) * 16], clear, 16) == 0) continue;
            ++r.slotChecked;
            float s[2], z;
            std::memcpy(s, &b.slot[size_t(p) * 8], 8);
            std::memcpy(&z, &b.depth[size_t(p) * 4], 4);
            if (s[0] == kSlotCode && s[1] == kDepth && word(b.slot, size_t(p) * 8 + 4) == word(b.depth, size_t(p) * 4))
                continue;
            if (r.slotBad++ < 4)
                std::printf("    %s: data set %u, MRT6 at (%u,%u) = (%.9g, %.9g), depth %.9g; want (11, 0.5), the depth\n",
                            name, seed, p % kSize, p / kSize, s[0], s[1], z);
        }
        for (unsigned t = 0; t < 4; ++t) {
            if (seed == 0) varied[t] = distinct(a.target[t], 16);
            for (size_t at = 0; seed == 1 && at < a.target[t].size(); at += 16)
                changed += std::memcmp(&a.target[t][at], &base.target[t][at], 16) != 0 ? 1u : 0u;
        }
        if (seed == 0) base = std::move(a);
    }
    ctx->ClearState();
    r.driven = true;
    std::printf("  identity: %s: %u targets + depth, 2 data sets: %u texels compared, %u mismatches; %u covered, "
                "MRT6 %u checked, %u bad; distinct o0..o3 %zu/%zu/%zu/%zu, data set 1 changed %u\n",
                name, r.targets, r.texelsCompared, r.mismatches, r.covered, r.slotChecked, r.slotBad, varied[0],
                varied[1], varied[2], varied[3], changed);
    check(*std::max_element(varied, varied + 4) > 1, "identity: vacuous -- every stock target is uniform");
    check(decls.empty() || changed > 0, "identity: vacuous -- the second data set changed no stock output");
    check(r.mismatches == 0, "identity: the patched PS writes SV_Target0..3 and depth bit-identically to the stock PS");
    check(r.covered > 0, "identity: vacuous -- the stock draw covered no texel");
    check(r.slotChecked > 0 && r.slotBad == 0,
          "identity: MRT6 holds 2 * slot + 1 and the fragment's depth on every texel the patched draw covered");
    return r;
}

} // namespace corpus_identity
