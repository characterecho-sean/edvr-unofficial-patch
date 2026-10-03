#pragma once

// Add one private raster-coverage export to a pure SM5 pixel shader. The
// original instructions and outputs are copied byte for byte. A fragment
// discarded by the original shader never reaches the added MOV before RET.
#include "dxbc_container.h"
#include "flat_shader_classifier.h" // shared bounds-checked DXBC operand decoder
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
                // A read-only structured/raw SRV may feed conditional colour
                // math. Keep that control flow intact and add the coverage
                // write only at one unconditional, top-level terminal RET.
                std::vector<bool> elseSeen;
                for (size_t at = 2; at < in.size();) {
                    const uint32_t op = in[at] & 0x7ffu;
                    const uint32_t length = instructionLength(in, at);
                    if (returned) throw std::runtime_error("instructions after terminal PS return");
                    // Only balanced IF/ELSE/ENDIF is admitted. Loops, jumps,
                    // calls and conditional returns cannot establish that
                    // every surviving fragment reaches the coverage write.
                    if ((op >= 2 && op <= 9) || (op >= 19 && op <= 20) ||
                        (op >= 22 && op <= 23) || op == 48 || op == 58 ||
                        op == 63 || op == 76 || op == 120)
                        throw std::runtime_error("unsupported PS control flow or declaration");
                    if (op == 31) {
                        if (elseSeen.size() >= 64) throw std::runtime_error("PS IF nesting limit");
                        elseSeen.push_back(false);
                    } else if (op == 18) {
                        if (elseSeen.empty() || elseSeen.back())
                            throw std::runtime_error("unbalanced PS ELSE");
                        elseSeen.back() = true;
                    } else if (op == 21) {
                        if (elseSeen.empty()) throw std::runtime_error("unbalanced PS ENDIF");
                        elseSeen.pop_back();
                    }
                    // Shader Model 5: DCL_RESOURCE_RAW/STRUCTURED are SRV
                    // declarations; LD_RAW/STRUCTURED read them. All UAV
                    // declarations, stores, atomics and other SM5 opcodes
                    // retain the original fail-closed path.
                    const bool readOnlySm5 = op == 161 || op == 162 || op == 165 || op == 167;
                    if (op >= 143 && !readOnlySm5)
                        throw std::runtime_error("unsupported PS UAV/structured operation");
                    if (op == 165 || op == 167) {
                        // Both LD_RAW and LD_STRUCTURED can name an SRV (t#)
                        // or a UAV (u#). Inspect the actual final resource
                        // operand, including extended opcode/operand tokens
                        // and relative-index payloads, before admitting it.
                        size_t operandAt = at + 1;
                        uint32_t extended = in[at];
                        while (extended & 0x80000000u) {
                            if (operandAt >= at + length)
                                throw std::runtime_error("structured load opcode extension");
                            extended = in[operandAt++];
                        }
                        const unsigned operandCount = op == 165 ? 3u : 4u;
                        flat_shader_classifier_detail::Operand resource;
                        for (unsigned i = 0; i < operandCount; ++i) {
                            flat_shader_classifier_detail::Operand parsed;
                            if (!flat_shader_classifier_detail::parseOperand(in, operandAt, parsed) ||
                                operandAt > at + length)
                                throw std::runtime_error("structured load operand bounds");
                            if (i + 1 == operandCount) resource = parsed;
                        }
                        if (operandAt != at + length ||
                            resource.type != flat_shader_classifier_detail::kOperandResource)
                            throw std::runtime_error("structured load is not SRV read");
                    }
                    const bool declaration = (op >= 88 && op <= 106) || op == 53 || op == 161 || op == 162;
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
                        if (!elseSeen.empty() || at + length != in.size())
                            throw std::runtime_error("PS return not terminal and top-level");
                        const uint32_t mark[] = {0x05000036u, 0x00102012u, kFlatOverlayTarget,
                                                 0x00004001u, 0x3f800000u};
                        out.insert(out.end(), mark, mark + 5);
                        returned = true;
                    }
                    out.insert(out.end(), in.begin() + at, in.begin() + at + length);
                    at += length;
                }
                if (!declared || !returned || !elseSeen.empty())
                    throw std::runtime_error("PS has no terminal unconditional return");
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
