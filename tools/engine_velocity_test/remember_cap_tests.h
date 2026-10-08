#pragma once
// engine_velocity_test: a full remember table says so, once per kind (2026-10-08).
//
// engine_velocity.cpp keeps the game's keyed shader objects, at most kRememberCap each of vertex and pixel
// shaders. Past the cap an object was dropped without a word, its family never ran substituted, and a flight
// log showed only the "not created by the game this session ... 0 draws" signature, which a shader the game
// never created also shows. Now the first drop of each kind logs one line naming the kind, the cap and the
// dropped hash. The cases, per kind:
//   R1  filling the table to exactly the cap says nothing, at every one of the kept objects
//   R2  an object already kept, offered again at the cap, is not a drop and says nothing
//   R3  the first object past the cap writes exactly one line: the kind, the cap, the dropped hash
//   R4  more drops stay silent, and the table stays at the cap (the cap itself is unchanged)
//   R5  the other kind's line is its own: one kind's drop writes none for the other, and each prints once
// This runs LAST in the rig: it fills both tables for the rest of the process, and the line is once per
// session, so a test after it could not remember a shader of its own.

#include <d3d11.h>
#include <wrl/client.h>

#include <cstdio>
#include <string>
#include <vector>

#include "lifecycle_tests.h"
#include "shader_tests.h"
#include "../../src/d3d11/engine_velocity.h"
#include "../../src/d3d11/engine_velocity_families.h"

namespace edvr {
// Rig-only accessors in engine_velocity.cpp (EDVR_ENGINE_VELOCITY_RIG): the table's size and the cap.
size_t engineVelocityRememberedForRig(bool pixel);
size_t engineVelocityRememberCapForRig();
}  // namespace edvr

namespace remember_cap_tests {
using Microsoft::WRL::ComPtr;
using lifecycle_fake::g_log;

inline size_t lines(const char* fragment, size_t from) {
    size_t n = 0;
    for (size_t i = from; i < g_log.size(); ++i)
        if (g_log[i].find(fragment) != std::string::npos) ++n;
    return n;
}
inline std::string hex64(uint64_t v) {
    char t[24];
    std::snprintf(t, sizeof(t), "%016llX", static_cast<unsigned long long>(v));
    return t;
}

// One kind through R1-R4. `fill` keys the objects that fill the table, `drop` the ones past the cap (a different
// keyed hash, so the line is seen to name the dropped shader and not the family or the last one kept).
struct Kind {
    bool pixel;
    const char* word;       // "vertex" / "pixel"
    const char* prefix;     // "vs_" / "ps_"
    uint64_t fill, drop[3];
    const char* fragment;   // the line's fixed text
};

inline void oneKind(const lifecycle_tests::Harness& h, const Kind& k, const std::vector<unsigned char>& bytes,
                    ID3DBlob* blob) {
    const size_t cap = edvr::engineVelocityRememberCapForRig();
    auto made = [&]() {
        ComPtr<ID3D11DeviceChild> object;
        if (k.pixel) {
            ComPtr<ID3D11PixelShader> ps;
            h.check(SUCCEEDED(h.device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &ps)),
                    "cap: a pixel shader fixture");
            ps.As(&object);
        } else {
            ComPtr<ID3D11VertexShader> vs;
            h.check(SUCCEEDED(h.device->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &vs)),
                    "cap: a vertex shader fixture");
            vs.As(&object);
        }
        return object;
    };
    auto offer = [&](ID3D11DeviceChild* object, uint64_t hash) {
        if (k.pixel)
            edvr::engineVelocityRememberPs(static_cast<ID3D11PixelShader*>(object), hash, bytes.data(), bytes.size(), false);
        else
            edvr::engineVelocityRememberVs(static_cast<ID3D11VertexShader*>(object), hash, bytes.data(), bytes.size(), false);
    };
    auto kept = [&]() { return edvr::engineVelocityRememberedForRig(k.pixel); };

    h.check(kept() < cap, "cap R1: the earlier cases left room in the table");   // they remember a handful
    std::vector<ComPtr<ID3D11DeviceChild>> held;
    size_t mark = g_log.size();
    // R1: to exactly the cap, a line at none of them (bounded: a table that stopped growing fails below, not forever).
    for (size_t i = 0; i < cap + 4 && kept() < cap; ++i) {
        held.push_back(made());
        offer(held.back().Get(), k.fill);
        h.check(g_log.size() == mark, (std::string("cap R1: ") + k.word + " shaders below the cap say nothing").c_str());
    }
    h.check(kept() == cap, (std::string("cap R1: ") + k.word + " table filled to exactly the cap").c_str());
    h.check(lines(k.fragment, mark) == 0, (std::string("cap R1: ") + k.word + " table full, still no line").c_str());

    // R2: an object already kept is not a drop.
    offer(held.front().Get(), k.fill);
    h.check(g_log.size() == mark && kept() == cap,
            (std::string("cap R2: a ") + k.word + " shader already kept, offered again at the cap, says nothing").c_str());

    // R3: the first object past the cap: one line, naming the kind, the cap and the dropped hash.
    held.push_back(made());
    offer(held.back().Get(), k.drop[0]);
    h.check(lines(k.fragment, mark) == 1,
            (std::string("cap R3: the first ") + k.word + " drop writes exactly one line").c_str());
    std::string line;
    for (size_t i = mark; i < g_log.size(); ++i)
        if (g_log[i].find(k.fragment) != std::string::npos) line = g_log[i];
    h.check(line.find(std::to_string(cap)) != std::string::npos,
            (std::string("cap R3: the ") + k.word + " line names the cap").c_str());
    h.check(line.find(k.prefix + hex64(k.drop[0])) != std::string::npos,
            (std::string("cap R3: the ") + k.word + " line names the dropped shader's hash").c_str());
    h.check(line.find(k.prefix + hex64(k.fill)) == std::string::npos,
            (std::string("cap R3: the ") + k.word + " line does not name the hash that filled the table").c_str());
    h.check(kept() == cap, (std::string("cap R3: the dropped ") + k.word + " shader was not kept").c_str());

    // R4: further drops are silent and change nothing.
    const size_t after = g_log.size();
    for (int i = 1; i < 3; ++i) {
        held.push_back(made());
        offer(held.back().Get(), k.drop[i]);
    }
    h.check(g_log.size() == after && lines(k.fragment, mark) == 1,
            (std::string("cap R4: later ") + k.word + " drops stay silent (once per kind per session)").c_str());
    h.check(kept() == cap, (std::string("cap R4: the ") + k.word + " table stays at the cap").c_str());
}

inline void run(const lifecycle_tests::Harness& h) {
    using edvr::engine_velocity_family::kFamilies;
    namespace shaders = shader_tests;
    const auto vsBlob = shaders::compile({h.device, h.context, h.check},
                                         "float4 main(float4 p : POSITION) : SV_Position { return p; }", "vs_5_0");
    const auto psBlob = shaders::compile({h.device, h.context, h.check},
                                         "float4 main() : SV_Target0 { return float4(0, 0, 0, 1); }", "ps_5_0");
    // The remembered bytes only have to be non-empty; the fixtures' own bytecode serves.
    const std::vector<unsigned char> vsBytes(static_cast<const unsigned char*>(vsBlob->GetBufferPointer()),
                                             static_cast<const unsigned char*>(vsBlob->GetBufferPointer()) + vsBlob->GetBufferSize());
    const std::vector<unsigned char> psBytes(static_cast<const unsigned char*>(psBlob->GetBufferPointer()),
                                             static_cast<const unsigned char*>(psBlob->GetBufferPointer()) + psBlob->GetBufferSize());
    // Keyed hashes all: family 0's vertex shader fills, the next two families' are dropped; family 0's pixel
    // shaders fill and drop (the fourth and fifth are unused elsewhere in the rig).
    const Kind vs{false, "vertex", "vs_", kFamilies[0].vs, {kFamilies[1].vs, kFamilies[2].vs, kFamilies[1].vs},
                  "vertex shader remember cap reached"};
    const Kind ps{true, "pixel", "ps_", kFamilies[0].ps[0], {kFamilies[0].ps[3], kFamilies[0].ps[4], kFamilies[0].ps[3]},
                  "pixel shader remember cap reached"};

    size_t mark = g_log.size();
    oneKind(h, vs, vsBytes, vsBlob.Get());
    // R5: the vertex kind's drop wrote no pixel line; the pixel kind then writes its own, and the vertex kind's stays one.
    h.check(lines(ps.fragment, mark) == 0, "cap R5: a vertex drop writes no pixel line");
    oneKind(h, ps, psBytes, psBlob.Get());
    h.check(lines(vs.fragment, mark) == 1 && lines(ps.fragment, mark) == 1,
            "cap R5: each kind printed its line once, the pixel drop did not repeat the vertex line");
    std::printf("  remember cap: filling a table to the cap is silent; the first drop past it logs one line per kind "
                "naming the kind, the cap and the dropped hash; later drops and re-offers stay silent; the cap is unchanged\n");
}
}  // namespace remember_cap_tests
