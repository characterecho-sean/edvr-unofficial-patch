#pragma once

// Add one private raster-coverage export to a pure SM5 pixel shader. The
// original instructions and outputs are copied byte for byte. A fragment
// discarded by the original shader never reaches the added MOV before RET.
#include "dxbc_container.h"
#include <cstdint>
#include <string>
#include <vector>

namespace edvr {
constexpr uint32_t kFlatOverlayTarget = 7;

inline bool flatOverlayPatchPs(const void* bytecode, size_t bytes,
                               std::vector<BYTE>& patched, std::string& reason) {
    using namespace dxbc_container;
    patched.clear();
    reason.clear();
    try {
        auto chunks = parseContainer(bytecode, bytes, 0x00000050u);
        bool outputSignature = false, program = false, colorOutput = false;
        for (auto& chunk : chunks) {
            if (chunk.tag == 0x4e47534fu) { // OSGN
                if (outputSignature) throw std::runtime_error("duplicate output signature");
                outputSignature = true;
                auto elements = parseSignature(chunk.bytes);
                uint32_t targetSystemValue = 0;
                for (const auto& e : elements) {
                    if (!equalName(e.name, "SV_TARGET"))
                        throw std::runtime_error("non-colour PS output (depth/coverage/stencil)");
                    if (e.semanticIndex >= kFlatOverlayTarget || e.registerIndex >= kFlatOverlayTarget)
                        throw std::runtime_error("private MRT7 occupied");
                    if (e.componentType != 3) throw std::runtime_error("non-float colour output");
                    targetSystemValue = e.systemValue;
                    colorOutput = true;
                }
                if (!colorOutput) throw std::runtime_error("no colour output");
                SignatureElement coverage;
                coverage.name = "SV_TARGET";
                coverage.semanticIndex = kFlatOverlayTarget;
                coverage.systemValue = targetSystemValue;
                coverage.componentType = 3;
                coverage.registerIndex = kFlatOverlayTarget;
                coverage.masks = 0x0e01u; // x written, yzw unused
                elements.push_back(std::move(coverage));
                chunk.bytes = makeSignature(elements);
            } else if (chunk.tag == 0x58454853u || chunk.tag == 0x52444853u) { // SHEX / SHDR
                if (program) throw std::runtime_error("duplicate program");
                program = true;
                if (chunk.bytes.size() & 3u) throw std::runtime_error("program alignment");
                std::vector<uint32_t> in(chunk.bytes.size() / 4);
                std::memcpy(in.data(), chunk.bytes.data(), chunk.bytes.size());
                std::vector<uint32_t> out;
                out.reserve(in.size() + 16);
                out.push_back(in[0]);
                out.push_back(0);
                bool declared = false, returned = false, executable = false;
                for (size_t at = 2; at < in.size();) {
                    const uint32_t op = in[at] & 0x7ffu;
                    const uint32_t length = instructionLength(in, at);
                    // Branches, calls and conditional returns need a separate
                    // control-flow proof. Keep this patch deliberately narrow.
                    if ((op >= 2 && op <= 9) || (op >= 18 && op <= 23) ||
                        op == 31 || op == 48 || op == 58 || op == 63 || op == 76 ||
                        op == 120)
                        throw std::runtime_error("unsupported PS control flow or declaration");
                    if (op >= 143) throw std::runtime_error("unsupported PS UAV/structured operation");
                    const bool declaration = (op >= 88 && op <= 106) || op == 53;
                    if (!declaration) executable = true;
                    if (op >= 101 && op <= 103) {
                        if (length < 3) throw std::runtime_error("output declaration");
                        const uint32_t type = (in[at + 1] >> 12) & 255u;
                        if (type != 2) throw std::runtime_error("depth/coverage output declaration");
                        if (in[at + 2] >= kFlatOverlayTarget)
                            throw std::runtime_error("private MRT7 declared by original PS");
                    }
                    if (!declared && executable) {
                        const uint32_t decl[] = {0x03000065u, 0x00102012u, kFlatOverlayTarget};
                        out.insert(out.end(), decl, decl + 3);
                        declared = true;
                    }
                    if (op == 62) { // RET: only surviving fragments execute this tail.
                        const uint32_t mark[] = {0x05000036u, 0x00102012u, kFlatOverlayTarget,
                                                 0x00004001u, 0x3f800000u};
                        out.insert(out.end(), mark, mark + 5);
                        returned = true;
                    }
                    out.insert(out.end(), in.begin() + at, in.begin() + at + length);
                    at += length;
                }
                if (!declared || !returned) throw std::runtime_error("PS has no unconditional return");
                out[1] = static_cast<uint32_t>(out.size());
                chunk.bytes.resize(out.size() * 4);
                std::memcpy(chunk.bytes.data(), out.data(), chunk.bytes.size());
            }
        }
        if (!outputSignature || !program) throw std::runtime_error("missing PS signature or program");
        patched = makeContainer(chunks);
        return true;
    } catch (const std::exception& e) {
        reason = e.what();
        patched.clear();
        return false;
    }
}
} // namespace edvr
