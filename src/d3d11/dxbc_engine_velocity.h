#pragma once

// Engine-record velocity, phase 1 (with fix.temporal_aa): restricted SM5
// patches that make the game's OWN pool draws record, per pixel, which pool
// record drew the pixel and at what depth. The temporal pass then turns that
// record's engine poses (current, and the previous one EDVR wrote into the
// record's unread second pose block) into exact motion for the pixel.
//
// The pixel shader keeps every original instruction and export and gains one
// float2 export at MRT6:
//   x = 2 * slot + 1, the t33 slot the draw's vertex shader indexed (the low
//       23 bits: a pool of 2^23 records would be 2.8 GB), written odd and
//       converted to float (exact below 2^24) so that no blend, sum or clear
//       can pass for a slot;
//   y = SV_Position.z, the depth this fragment wrote (the consumer's exact
//       equality test against the scene depth rejects a slot a later,
//       unpatched draw covered).
// Six of the seven pool vertex-shader families already export the slot to
// the pixel shader (__USER_MATERIALMODULATION_DATAID.y or
// __USER_VERTEX_FACEINVARIANT.x, written as bfi(31,0,v0.x,flag)). The
// UV-only family (vs_5B4D8E894EEDA8B4 -> ps_4375B72964F386CD) does not, so
// its vertex shader gains one output, EDVRPOOLSLOT = v0.x, after its last
// register, and its pixel shader reads that instead. Nothing else in either
// program changes: the position math, the depth bias and the G-buffer
// exports stay byte-for-byte. SV_Position is rasterizer-generated for a PS;
// its input register need not equal the VS output register. A PS can use that
// numeric register for SV_IsFrontFace, so the patch chooses a free PS input.
//
// Unsupported containers, signatures, declarations or control flow decline
// with a reason and produce no bytecode.

#include "dxbc_container.h"

#include <algorithm>

namespace edvr {

// The render-target slot the patched pixel shaders export to. The game's
// G-buffer uses 0..3 (7 was the retired static owner's export).
constexpr uint32_t kEngineVelocityTarget = 6;
// The guarded flat decal variant alone reads a private copy of MRT6 here.
constexpr uint32_t kEngineVelocityOverlaySnapshotSlot = 3;
// The slot bits kept before the odd encoding (2 * slot + 1 stays below 2^24,
// where float is still exact).
constexpr uint32_t kEngineVelocitySlotMask = 0x007fffffu;
// The semantic the patched UV-only vertex shader exports and its patched
// pixel shader reads. This user-defined varying preserves its matching
// semantic and component/register layout on both sides.
constexpr char kEngineVelocitySlotSemantic[] = "EDVRPOOLSLOT";

struct EngineVelocityInputs {
    uint32_t identityRegister = ~0u;   // PS input register carrying the slot
    uint32_t identityComponent = ~0u;  // ...and its component
    uint32_t positionRegister = ~0u;   // VS SV_POSITION output; PS may use a different free input register
    bool slotFromVsPatch = false;      // the VS must be patched to export EDVRPOOLSLOT there
};

namespace dxbc_engine_velocity_detail {

using dxbc_container::Chunk;
using dxbc_container::SignatureElement;
using dxbc_container::equalName;
using dxbc_container::instructionLength;
using dxbc_container::makeContainer;
using dxbc_container::makeSignature;
using dxbc_container::parseContainer;
using dxbc_container::parseSignature;

constexpr uint32_t kTagIsgn = 0x4e475349u, kTagOsgn = 0x4e47534fu;
constexpr uint32_t kTagShex = 0x58454853u, kTagShdr = 0x52444853u;
constexpr uint32_t kVs50 = 0x00010050u, kPs50 = 0x00000050u;

// Opcodes (d3d11TokenizedProgramFormat.hpp).
constexpr uint32_t kOpRet = 62, kOpRetc = 63;
constexpr uint32_t kOpDclResourceStructured = 162, kOpLdStructured = 167;
constexpr uint32_t kOpDclInput = 95, kOpDclInputPs = 98, kOpDclInputPsSgv = 99, kOpDclInputPsSiv = 100;
constexpr uint32_t kOpDclOutput = 101, kOpDclOutputSgv = 102, kOpDclOutputSiv = 103, kOpDclTemps = 104;
constexpr uint32_t kOpDclGlobalFlags = 106, kOpDclConstantBuffer = 89;

inline uint32_t operandType(uint32_t token) { return (token >> 12) & 255u; }
inline uint32_t operandMask(uint32_t token) { return (token >> 4) & 15u; }
inline bool isProgram(uint32_t tag) { return tag == kTagShex || tag == kTagShdr; }

inline std::vector<uint32_t> programWords(const std::vector<BYTE>& bytes) {
    std::vector<uint32_t> words(bytes.size() / 4);
    std::memcpy(words.data(), bytes.data(), words.size() * 4);
    return words;
}
inline std::vector<BYTE> programBytes(std::vector<uint32_t> words) {
    words[1] = static_cast<uint32_t>(words.size());
    std::vector<BYTE> bytes(words.size() * 4);
    std::memcpy(bytes.data(), words.data(), bytes.size());
    return bytes;
}

// Declarations the patch cannot reason about: UAVs, stream output, thread
// groups, class linkage, hull/domain/geometry constructs, conditional return.
inline void declineUnsupported(uint32_t opcode) {
    if (opcode >= 156 && opcode <= 160) throw std::runtime_error("UAV or thread-group declaration");
    if (opcode == 120 || (opcode >= 144 && opcode <= 146)) throw std::runtime_error("dynamic linkage");
    if (opcode == kOpRetc) throw std::runtime_error("conditional return");
}

// Does the program read t33 (a 336-byte structured pool) indexed by v0.x?
// The UV-only family's slot export is v0.x; this refuses a vertex shader that
// does not index the pool with exactly that operand.
inline bool readsPoolAtV0x(const std::vector<uint32_t>& t) {
    bool declared = false, indexed = false;
    for (size_t at = 2; at < t.size();) {
        const uint32_t opcode = t[at] & 0x7ffu;
        const uint32_t length = instructionLength(t, at);
        if (opcode == kOpDclResourceStructured && length == 4 && operandType(t[at + 1]) == 7 &&
            t[at + 2] == 33 && t[at + 3] == 336) declared = true;
        if (opcode == kOpLdStructured) {
            bool v0x = false, pool = false;
            for (size_t i = at + 1; i + 1 < at + length; ++i) {
                if (t[i] == 0x0010100Au && t[i + 1] == 0) v0x = true;          // v0.x (select-1)
                if ((t[i] & 0x00100000u) && operandType(t[i]) == 7 && t[i + 1] == 33) pool = true;
            }
            indexed = indexed || (v0x && pool);
        }
        at += length;
    }
    return declared && indexed;
}

inline void sortByRegister(std::vector<SignatureElement>& elements) {
    std::stable_sort(elements.begin(), elements.end(),
        [](const SignatureElement& a, const SignatureElement& b) { return a.registerIndex < b.registerIndex; });
}

// VS: declare oS.x after the last output declaration; mov oS.x, v0.x before
// every ret. v0.x must already be declared (the pool index input).
inline std::vector<BYTE> patchVsProgram(const std::vector<BYTE>& bytes, uint32_t slotRegister) {
    const auto t = programWords(bytes);
    size_t lastOutputEnd = 0;
    bool v0x = false, rets = false;
    for (size_t at = 2; at < t.size();) {
        const uint32_t opcode = t[at] & 0x7ffu;
        const uint32_t length = instructionLength(t, at);
        declineUnsupported(opcode);
        if (opcode >= kOpDclOutput && opcode <= kOpDclOutputSiv) {
            if (length >= 3 && operandType(t[at + 1]) == 2 && t[at + 2] == slotRegister)
                throw std::runtime_error("slot output register occupied");
            lastOutputEnd = at + length;
        }
        if (opcode == kOpDclInput && length == 3 && operandType(t[at + 1]) == 1 && t[at + 2] == 0 &&
            (operandMask(t[at + 1]) & 1u)) v0x = true;
        if (opcode == kOpRet) rets = true;
        at += length;
    }
    if (!lastOutputEnd) throw std::runtime_error("no output declarations");
    if (!v0x) throw std::runtime_error("v0.x not declared");
    if (!rets) throw std::runtime_error("no return");
    std::vector<uint32_t> out;
    out.reserve(t.size() + 16);
    out.push_back(t[0]);
    out.push_back(0);
    const uint32_t decl[] = {0x03000065u, 0x00102012u, slotRegister};              // dcl_output oS.x
    const uint32_t move[] = {0x05000036u, 0x00102012u, slotRegister, 0x0010100Au, 0}; // mov oS.x, v0.x
    for (size_t at = 2; at < t.size();) {
        const uint32_t opcode = t[at] & 0x7ffu;
        const uint32_t length = instructionLength(t, at);
        if (opcode == kOpRet) out.insert(out.end(), move, move + 5);
        out.insert(out.end(), t.begin() + at, t.begin() + at + length);
        if (at + length == lastOutputEnd) out.insert(out.end(), decl, decl + 3);
        at += length;
    }
    return programBytes(std::move(out));
}

// PS: declare the position input (or make its z live), the slot input when
// it comes from the patched VS, oT.xy and one more temp; before every ret:
//   and  rN.x, v<id>.<c>, l(0x007fffff)
//   imad rN.x, rN.x, l(2), l(1)
//   utof oT.x, rN.x
//   mov  oT.y, v<pos>.z
// The slot is written ODD (2 * slot + 1, exact in float below 2^24): a
// cleared target (-1), an untouched one and any sum or blend of two writes is
// never an odd whole number, so arithmetic that reached MRT6 is declined by
// the compose instead of naming another record (the 2026-09-23 review, item 4).
inline std::vector<BYTE> patchPsProgram(const std::vector<BYTE>& bytes, const EngineVelocityInputs& in,
                                        bool guardOverlayDepth = false) {
    auto t = programWords(bytes);
    size_t firstOutput = 0, tempAt = 0, firstExecutable = 0;
    uint32_t tempCount = 0, returns = 0;
    bool identityDeclared = false, positionDeclared = false;
    for (size_t at = 2; at < t.size();) {
        const uint32_t opcode = t[at] & 0x7ffu;
        const uint32_t length = instructionLength(t, at);
        declineUnsupported(opcode);
        // The admitted SM5 shader has direct t0..t2 references. Refuse any
        // existing direct t3 operand or declaration rather than rebinding a
        // game resource that this exact shader could read.
        if (guardOverlayDepth) {
            for (size_t i = at + 1; i + 1 < at + length; ++i)
                if (operandType(t[i]) == 7 && t[i + 1] == kEngineVelocityOverlaySnapshotSlot)
                    throw std::runtime_error("overlay snapshot t3 occupied");
        }
        const bool declaration = (opcode >= 88 && opcode <= 106) || opcode == 143 ||
                                 (opcode >= 147 && opcode <= 163) || opcode == 53;
        if (!firstExecutable && !declaration) firstExecutable = at;
        if (!firstOutput && opcode >= kOpDclOutput && opcode <= kOpDclOutputSiv) firstOutput = at;
        if (opcode >= kOpDclOutput && opcode <= kOpDclOutputSiv && length >= 3 &&
            operandType(t[at + 1]) == 2 && t[at + 2] >= kEngineVelocityTarget)
            throw std::runtime_error("output target 6 or above occupied");
        if (opcode >= kOpDclInputPs && opcode <= kOpDclInputPsSiv && length >= 3 &&
            operandType(t[at + 1]) == 1) {
            const uint32_t reg = t[at + 2];
            const uint32_t mask = operandMask(t[at + 1]);
            if (reg == in.identityRegister) {
                if (in.slotFromVsPatch) throw std::runtime_error("slot input register occupied");
                if (mask & (1u << in.identityComponent)) identityDeclared = true;
            }
            if (reg == in.positionRegister) {
                if (opcode != kOpDclInputPsSiv) throw std::runtime_error("position register not SV_Position");
                positionDeclared = true;
                t[at + 1] |= guardOverlayDepth ? 0x70u : 0x40u; // guarded read also needs xy
            }
        }
        if (opcode == kOpDclTemps) {
            if (tempAt || length != 2 || t[at + 1] > 4092) throw std::runtime_error("temps");
            tempAt = at;
            tempCount = t[at + 1];
        }
        if (opcode == kOpRet) ++returns;
        at += length;
    }
    if (!firstOutput) throw std::runtime_error("no output declarations");
    if (!returns) throw std::runtime_error("no return");
    if (!in.slotFromVsPatch && !identityDeclared) throw std::runtime_error("identity input not declared");
    if (!tempAt && !firstExecutable) throw std::runtime_error("no executable instructions");

    const uint32_t temp = tempCount;   // the new temp's index
    const uint32_t identitySelect = 0x0010100Au | (in.identityComponent << 4); // v<id>.<c>
    const uint32_t posDecl[] = {0x04002064u, guardOverlayDepth ? 0x00101072u : 0x00101042u,
                                in.positionRegister, 1u}; // dcl_input_ps_siv linear noperspective v.xyz, position
    const uint32_t slotDecl[] = {0x03000862u, 0x00101012u, in.identityRegister};    // dcl_input_ps constant v.x
    const uint32_t outDecl[] = {0x03000065u, 0x00102032u, kEngineVelocityTarget};   // dcl_output o6.xy
    const uint32_t tail[] = {
        0x07000001u, 0x00100012u, temp, identitySelect, in.identityRegister, 0x00004001u, kEngineVelocitySlotMask,
        0x09000023u, 0x00100012u, temp, 0x0010000Au, temp, 0x00004001u, 2u, 0x00004001u, 1u,
        0x05000056u, 0x00102012u, kEngineVelocityTarget, 0x0010000Au, temp,
        0x05000036u, 0x00102022u, kEngineVelocityTarget, 0x0010102Au, in.positionRegister,
    };
    // This block is the exact token form of SM5 ftoi/ld_indexable/eq/movc,
    // checked against D3DCompile and WARP by engine_velocity_test. The guard
    // chooses the substrate depth only for the same odd pool-record code.
    const uint32_t overlayResource[] = {0x04001858u, 0x00107000u, kEngineVelocityOverlaySnapshotSlot, 0x00005555u};
    const uint32_t guardedTail[] = {
        0x07000001u, 0x00100012u, temp, identitySelect, in.identityRegister, 0x00004001u, kEngineVelocitySlotMask,
        0x09000023u, 0x00100012u, temp, 0x0010000Au, temp, 0x00004001u, 2u, 0x00004001u, 1u,
        0x05000056u, 0x00100012u, temp, 0x0010000Au, temp,
        0x05000036u, 0x00102012u, kEngineVelocityTarget, 0x0010000Au, temp,
        0x0500001Bu, 0x00100032u, temp + 1, 0x00101046u, in.positionRegister,
        0x08000036u, 0x001000C2u, temp + 1, 0x00004002u, 0u, 0u, 0u, 0u,
        0x8900002Du, 0x800000C2u, 0x00155543u, 0x00100032u, temp + 1,
        0x00100E46u, temp + 1, 0x00107E46u, kEngineVelocityOverlaySnapshotSlot,
        0x07000018u, 0x00100042u, temp + 1, 0x0010000Au, temp, 0x0010000Au, temp + 1,
        0x09000037u, 0x00102022u, kEngineVelocityTarget, 0x0010002Au, temp + 1,
        0x0010001Au, temp + 1, 0x0010102Au, in.positionRegister,
    };
    std::vector<uint32_t> out;
    out.reserve(t.size() + 64);
    out.push_back(t[0]);
    out.push_back(0);
    bool outputDeclared = false;
    for (size_t at = 2; at < t.size();) {
        const uint32_t opcode = t[at] & 0x7ffu;
        const uint32_t length = instructionLength(t, at);
        if (at == firstOutput) {
            if (guardOverlayDepth) out.insert(out.end(), std::begin(overlayResource), std::end(overlayResource));
            if (!positionDeclared) out.insert(out.end(), posDecl, posDecl + 4);
            if (in.slotFromVsPatch) out.insert(out.end(), slotDecl, slotDecl + 3);
        }
        if (tempAt && at == tempAt) {
            out.insert(out.end(), outDecl, outDecl + 3);
            outputDeclared = true;
            out.push_back(t[at]);
            out.push_back(tempCount + (guardOverlayDepth ? 2u : 1u));
        } else {
            if (!tempAt && at == firstExecutable) {
                out.insert(out.end(), outDecl, outDecl + 3);
                out.push_back(0x02000068u);
                out.push_back(guardOverlayDepth ? 2u : 1u);
                outputDeclared = true;
            }
            if (opcode == kOpRet) {
                if (guardOverlayDepth) out.insert(out.end(), std::begin(guardedTail), std::end(guardedTail));
                else out.insert(out.end(), std::begin(tail), std::end(tail));
            }
            out.insert(out.end(), t.begin() + at, t.begin() + at + length);
        }
        at += length;
    }
    if (!outputDeclared) throw std::runtime_error("output declaration not placed");
    return programBytes(std::move(out));
}

} // namespace dxbc_engine_velocity_detail

// From the VERTEX shader's signatures: where the slot and SV_POSITION reach the
// pixel shader. A family that exports no slot gets slotFromVsPatch and the
// register after its last output, provided it indexes the t33 pool by v0.x.
inline bool engineVelocityDeriveInputs(const void* data, size_t bytes, EngineVelocityInputs& output,
                                       std::string& reason) {
    using namespace dxbc_engine_velocity_detail;
    output = {};
    reason.clear();
    try {
        auto chunks = parseContainer(data, bytes, kVs50);
        const std::vector<BYTE>* program = nullptr;
        bool isgn = false, osgn = false, instanceIndex = false;
        uint32_t maxOutput = 0;
        for (const auto& chunk : chunks) {
            if (isProgram(chunk.tag)) program = &chunk.bytes;
            if (chunk.tag == kTagIsgn) {
                if (isgn) throw std::runtime_error("duplicate input signature");
                isgn = true;
                for (const auto& e : parseSignature(chunk.bytes))
                    if (equalName(e.name, "INSTANCEANDMODELDATAINDEX") && e.semanticIndex == 0 &&
                        e.registerIndex == 0 && e.componentType == 1 && (e.masks & 1u)) instanceIndex = true;
            }
            if (chunk.tag == kTagOsgn) {
                if (osgn) throw std::runtime_error("duplicate output signature");
                osgn = true;
                for (const auto& e : parseSignature(chunk.bytes)) {
                    maxOutput = std::max(maxOutput, e.registerIndex);
                    if (e.componentType == 1 &&
                        ((equalName(e.name, "__USER_MATERIALMODULATION_DATAID") && (e.masks & 2u)) ||
                         (equalName(e.name, "__USER_VERTEX_FACEINVARIANT") && (e.masks & 1u)))) {
                        if (output.identityRegister != ~0u) throw std::runtime_error("ambiguous identity output");
                        output.identityRegister = e.registerIndex;
                        output.identityComponent = equalName(e.name, "__USER_MATERIALMODULATION_DATAID") ? 1u : 0u;
                    }
                    if ((e.systemValue == 1 || equalName(e.name, "SV_POSITION")) && (e.masks & 4u)) {
                        if (e.componentType != 3 || output.positionRegister != ~0u)
                            throw std::runtime_error("ambiguous position output");
                        output.positionRegister = e.registerIndex;
                    }
                }
            }
        }
        if (!isgn || !osgn || !program) throw std::runtime_error("missing vertex shader chunks");
        if (!instanceIndex) throw std::runtime_error("no INSTANCEANDMODELDATAINDEX.x at v0");
        if (output.positionRegister >= 32) throw std::runtime_error("SV_POSITION output absent");
        if (!readsPoolAtV0x(programWords(*program))) throw std::runtime_error("does not index the t33 pool by v0.x");
        if (output.identityRegister == ~0u) {
            if (maxOutput + 1 >= 32) throw std::runtime_error("no free output register for the slot");
            output.identityRegister = maxOutput + 1;
            output.identityComponent = 0;
            output.slotFromVsPatch = true;
        }
        return true;
    } catch (const std::exception& e) {
        reason = e.what();
        output = {};
        return false;
    }
}

inline bool engineVelocityPatchVs(const void* data, size_t bytes, const EngineVelocityInputs& inputs,
                                  std::vector<BYTE>& output, std::string& reason) {
    using namespace dxbc_engine_velocity_detail;
    output.clear();
    reason.clear();
    if (!inputs.slotFromVsPatch || inputs.identityRegister >= 32 || inputs.identityComponent != 0) {
        reason = "vertex shader needs no slot export";
        return false;
    }
    try {
        auto chunks = parseContainer(data, bytes, kVs50);
        bool osgn = false, program = false;
        for (auto& chunk : chunks) {
            if (chunk.tag == kTagOsgn) {
                if (osgn) throw std::runtime_error("duplicate output signature");
                osgn = true;
                auto elements = parseSignature(chunk.bytes);
                for (const auto& e : elements)
                    if (e.registerIndex == inputs.identityRegister) throw std::runtime_error("slot output register occupied");
                SignatureElement slot;
                slot.name = kEngineVelocitySlotSemantic;
                slot.componentType = 1;                      // uint
                slot.registerIndex = inputs.identityRegister;
                slot.masks = 0x0E01u;                        // x; y,z,w never written
                elements.push_back(std::move(slot));
                sortByRegister(elements);
                chunk.bytes = makeSignature(elements);
            } else if (isProgram(chunk.tag)) {
                if (program) throw std::runtime_error("duplicate program");
                program = true;
                chunk.bytes = patchVsProgram(chunk.bytes, inputs.identityRegister);
            }
        }
        if (!osgn || !program) throw std::runtime_error("missing vertex shader chunks");
        output = makeContainer(chunks);
        return true;
    } catch (const std::exception& e) {
        reason = e.what();
        output.clear();
        return false;
    }
}

inline bool engineVelocityPatchPs(const void* data, size_t bytes, const EngineVelocityInputs& inputs,
                                  std::vector<BYTE>& output, std::string& reason, bool guardOverlayDepth = false) {
    using namespace dxbc_engine_velocity_detail;
    output.clear();
    reason.clear();
    if (inputs.identityRegister >= 32 || inputs.identityComponent >= 4 || inputs.positionRegister >= 32 ||
        inputs.identityRegister == inputs.positionRegister) {
        reason = "invalid shader inputs";
        return false;
    }
    try {
        auto chunks = parseContainer(data, bytes, kPs50);
        EngineVelocityInputs psInputs = inputs;
        bool usedInput[32]{};
        uint32_t declaredPosition = ~0u;
        for (const auto& chunk : chunks) if (chunk.tag == kTagIsgn) {
            for (const auto& e : parseSignature(chunk.bytes)) {
                if (e.registerIndex >= 32) throw std::runtime_error("input register out of range");
                usedInput[e.registerIndex] = true;
                if (e.systemValue == 1 || equalName(e.name, "SV_POSITION")) {
                    if (declaredPosition != ~0u || e.componentType != 3)
                        throw std::runtime_error("ambiguous position input");
                    declaredPosition = e.registerIndex;
                }
            }
        }
        if (declaredPosition != ~0u) psInputs.positionRegister = declaredPosition;
        else if (usedInput[psInputs.positionRegister]) {
            uint32_t freeRegister = 0;
            while (freeRegister < 32 &&
                   (usedInput[freeRegister] || freeRegister == psInputs.identityRegister)) ++freeRegister;
            if (freeRegister == 32) throw std::runtime_error("no free position input register");
            psInputs.positionRegister = freeRegister;
        }
        if (psInputs.positionRegister == psInputs.identityRegister)
            throw std::runtime_error("position and identity input overlap");
        bool isgn = false, osgn = false, program = false;
        for (auto& chunk : chunks) {
            if (chunk.tag == kTagIsgn) {
                if (isgn) throw std::runtime_error("duplicate input signature");
                isgn = true;
                auto elements = parseSignature(chunk.bytes);
                bool identity = false, position = false;
                for (auto& e : elements) {
                    if (e.registerIndex == psInputs.identityRegister) {
                        if (psInputs.slotFromVsPatch) throw std::runtime_error("slot input register occupied");
                        if (e.componentType == 1 && (e.masks & (1u << psInputs.identityComponent))) identity = true;
                    }
                    if (e.registerIndex == psInputs.positionRegister) {
                        if (!(e.systemValue == 1 || equalName(e.name, "SV_POSITION")) || e.componentType != 3)
                            throw std::runtime_error("position input register holds another semantic");
                        e.masks |= guardOverlayDepth ? (7u | 0x0700u) : (4u | 0x0400u);
                        position = true;
                    }
                }
                if (psInputs.slotFromVsPatch) {
                    SignatureElement slot;
                    slot.name = kEngineVelocitySlotSemantic;
                    slot.componentType = 1;
                    slot.registerIndex = psInputs.identityRegister;
                    slot.masks = 0x0101u;                    // x, used
                    elements.push_back(std::move(slot));
                } else if (!identity) {
                    throw std::runtime_error("identity input absent");
                }
                if (!position) {
                    SignatureElement p;
                    p.name = "SV_Position";
                    p.systemValue = 1;
                    p.componentType = 3;
                    p.registerIndex = psInputs.positionRegister;
                    p.masks = guardOverlayDepth ? 0x070Fu : 0x040Fu;
                    elements.push_back(std::move(p));
                }
                sortByRegister(elements);
                for (size_t i = 1; i < elements.size(); ++i)
                    if (elements[i].registerIndex == elements[i - 1].registerIndex &&
                        (elements[i].masks & elements[i - 1].masks & 15u))
                        throw std::runtime_error("overlapping input registers");
                chunk.bytes = makeSignature(elements);
            } else if (chunk.tag == kTagOsgn) {
                if (osgn) throw std::runtime_error("duplicate output signature");
                osgn = true;
                auto elements = parseSignature(chunk.bytes);
                bool target = false;
                uint32_t targetSystemValue = 0;
                for (const auto& e : elements) {
                    if (dxbc_container::startsWithName(e.name, "SV_DEPTH") ||
                        dxbc_container::startsWithName(e.name, "SV_COVERAGE"))
                        throw std::runtime_error("depth or coverage output");
                    if (equalName(e.name, "SV_TARGET")) {
                        target = true;
                        targetSystemValue = e.systemValue;
                        if (e.registerIndex >= kEngineVelocityTarget) throw std::runtime_error("output target 6 or above occupied");
                    }
                }
                if (!target) throw std::runtime_error("no colour output");
                SignatureElement velocity;
                velocity.name = "SV_TARGET";
                velocity.semanticIndex = kEngineVelocityTarget;
                velocity.systemValue = targetSystemValue;
                velocity.componentType = 3;                  // float
                velocity.registerIndex = kEngineVelocityTarget;
                velocity.masks = 0x0C03u;                    // xy; z,w never written
                elements.push_back(std::move(velocity));
                chunk.bytes = makeSignature(elements);
            } else if (isProgram(chunk.tag)) {
                if (program) throw std::runtime_error("duplicate program");
                program = true;
                chunk.bytes = patchPsProgram(chunk.bytes, psInputs, guardOverlayDepth);
            }
        }
        if (!isgn || !osgn || !program) throw std::runtime_error("missing pixel shader chunks");
        output = makeContainer(chunks);
        return true;
    } catch (const std::exception& e) {
        reason = e.what();
        output.clear();
        return false;
    }
}

} // namespace edvr
