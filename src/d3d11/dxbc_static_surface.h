#pragma once

// Restricted SM5 pixel-shader augmentation for exact rigid-surface ownership.
// The original program and its MRT0..5 exports are retained byte-for-byte.  A
// guarded instruction tail hashes the draw key and the six raw rigid-pose
// words from the model pool into MRT7.xyz; MRT7.w receives SV_Position.z bits.
// Unsupported containers, signatures, resources and control flow decline
// without producing bytecode.

#include <windows.h>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

void ComputeHashRetail(const BYTE*, UINT, BYTE*);

namespace edvr {

struct StaticSurfaceShaderInputs {
    uint32_t identityRegister = ~0u;
    uint32_t identityComponent = ~0u;
    uint32_t positionRegister = ~0u;
};

namespace dxbc_static_surface_detail {

struct SignatureElement {
    std::string name;
    uint32_t semanticIndex = 0;
    uint32_t systemValue = 0;
    uint32_t componentType = 0;
    uint32_t registerIndex = 0;
    uint32_t masks = 0;
};

struct Chunk {
    uint32_t tag = 0;
    std::vector<BYTE> bytes;
};

inline uint32_t word(const std::vector<BYTE>& bytes, size_t offset) {
    if (offset > bytes.size() || bytes.size() - offset < sizeof(uint32_t))
        throw std::runtime_error("bounds");
    uint32_t value = 0;
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return value;
}

inline void put(std::vector<BYTE>& bytes, uint32_t value) {
    const BYTE* p = reinterpret_cast<const BYTE*>(&value);
    bytes.insert(bytes.end(), p, p + sizeof(value));
}

inline void setWord(std::vector<BYTE>& bytes, size_t offset, uint32_t value) {
    if (offset > bytes.size() || bytes.size() - offset < sizeof(value))
        throw std::runtime_error("bounds");
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

inline bool equalName(const std::string& a, const char* b) {
    return _stricmp(a.c_str(), b) == 0;
}

inline bool startsWithName(const std::string& a, const char* b) {
    return _strnicmp(a.c_str(), b, std::strlen(b)) == 0;
}

inline uint32_t instructionLength(const std::vector<uint32_t>& tokens, size_t at) {
    if (at >= tokens.size()) throw std::runtime_error("instruction bounds");
    uint32_t length = (tokens[at] >> 24) & 127u;
    // Immediate constant buffers carry their length in the following word.
    if ((tokens[at] & 0x7ffu) == 53u) {
        if ((tokens[at] >> 11) != 3u || at + 1 >= tokens.size())
            throw std::runtime_error("unsupported custom data");
        length = tokens[at + 1];
        if (length < 2 || (length - 2) % 4) throw std::runtime_error("constant buffer length");
    }
    if (!length || length > tokens.size() - at) throw std::runtime_error("instruction length");
    return length;
}

inline std::vector<SignatureElement> parseSignature(const std::vector<BYTE>& bytes) {
    if (bytes.size() < 8 || word(bytes, 4) != 8) throw std::runtime_error("signature header");
    const uint32_t count = word(bytes, 0);
    if (count > 128 || uint64_t(8) + uint64_t(count) * 24 > bytes.size())
        throw std::runtime_error("signature entries");
    std::vector<SignatureElement> result;
    result.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        const size_t at = 8 + size_t(i) * 24;
        const uint32_t nameOffset = word(bytes, at);
        if (nameOffset >= bytes.size()) throw std::runtime_error("signature name");
        const char* first = reinterpret_cast<const char*>(bytes.data() + nameOffset);
        const char* last = reinterpret_cast<const char*>(bytes.data() + bytes.size());
        const char* end = first;
        while (end != last && *end) ++end;
        if (end == last || end == first) throw std::runtime_error("signature name");
        SignatureElement e;
        e.name.assign(first, end);
        e.semanticIndex = word(bytes, at + 4);
        e.systemValue = word(bytes, at + 8);
        e.componentType = word(bytes, at + 12);
        e.registerIndex = word(bytes, at + 16);
        e.masks = word(bytes, at + 20);
        result.push_back(std::move(e));
    }
    return result;
}

inline std::vector<BYTE> makeSignature(const std::vector<SignatureElement>& elements) {
    if (elements.empty() || elements.size() > 128) throw std::runtime_error("signature count");
    std::vector<BYTE> result;
    put(result, static_cast<uint32_t>(elements.size()));
    put(result, 8);
    result.resize(8 + elements.size() * 24);
    for (size_t i = 0; i < elements.size(); ++i) {
        const auto& e = elements[i];
        if (e.name.empty() || e.name.size() > 255 || e.registerIndex >= 32)
            throw std::runtime_error("signature element");
        const uint32_t nameOffset = static_cast<uint32_t>(result.size());
        result.insert(result.end(), e.name.begin(), e.name.end());
        result.push_back(0);
        while (result.size() & 3) result.push_back(0xab);
        const size_t at = 8 + i * 24;
        setWord(result, at, nameOffset);
        setWord(result, at + 4, e.semanticIndex);
        setWord(result, at + 8, e.systemValue);
        setWord(result, at + 12, e.componentType);
        setWord(result, at + 16, e.registerIndex);
        setWord(result, at + 20, e.masks);
    }
    return result;
}

inline std::vector<Chunk> parseContainer(const void* data, size_t bytes, uint32_t expectedProgramType) {
    if (!data || bytes < 32 || bytes > 1024 * 1024) throw std::runtime_error("bytecode size");
    const BYTE* p = static_cast<const BYTE*>(data);
    std::vector<BYTE> input(p, p + bytes);
    if (std::memcmp(input.data(), "DXBC", 4) || word(input, 24) != bytes || word(input, 28) > 128)
        throw std::runtime_error("container");
    BYTE digest[16];
    ComputeHashRetail(input.data() + 20, static_cast<UINT>(input.size() - 20), digest);
    if (std::memcmp(digest, input.data() + 4, sizeof(digest))) throw std::runtime_error("input checksum");
    std::vector<Chunk> chunks;
    const uint32_t count = word(input, 28);
    if (uint64_t(32) + uint64_t(count) * 4 > input.size()) throw std::runtime_error("chunk table");
    bool program = false;
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t offset = word(input, 32 + i * 4);
        if (offset > input.size() || input.size() - offset < 8) throw std::runtime_error("chunk bounds");
        const uint32_t tag = word(input, offset);
        const uint32_t size = word(input, offset + 4);
        if (size > input.size() - offset - 8) throw std::runtime_error("chunk bounds");
        Chunk c;
        c.tag = tag;
        c.bytes.assign(input.begin() + offset + 8, input.begin() + offset + 8 + size);
        if (tag == 0x58454853u || tag == 0x52444853u) { // SHEX / SHDR
            if (program || c.bytes.size() < 8 || (c.bytes.size() & 3) ||
                word(c.bytes, 0) != expectedProgramType || word(c.bytes, 4) != c.bytes.size() / 4)
                throw std::runtime_error("program header");
            program = true;
        }
        chunks.push_back(std::move(c));
    }
    if (!program) throw std::runtime_error("missing program");
    return chunks;
}

inline std::vector<BYTE> makeContainer(const std::vector<Chunk>& chunks) {
    if (chunks.empty() || chunks.size() > 128) throw std::runtime_error("chunk count");
    std::vector<BYTE> result(32 + chunks.size() * 4, 0);
    std::memcpy(result.data(), "DXBC", 4);
    setWord(result, 20, 1);
    setWord(result, 28, static_cast<uint32_t>(chunks.size()));
    for (size_t i = 0; i < chunks.size(); ++i) {
        while (result.size() & 3) result.push_back(0xab);
        const uint32_t offset = static_cast<uint32_t>(result.size());
        setWord(result, 32 + i * 4, offset);
        put(result, chunks[i].tag);
        put(result, static_cast<uint32_t>(chunks[i].bytes.size()));
        result.insert(result.end(), chunks[i].bytes.begin(), chunks[i].bytes.end());
    }
    setWord(result, 24, static_cast<uint32_t>(result.size()));
    ComputeHashRetail(result.data() + 20, static_cast<UINT>(result.size() - 20), result.data() + 4);
    return result;
}

inline bool immediateRegister(const std::vector<uint32_t>& tokens, size_t at,
                              uint32_t length, uint32_t operandType, uint32_t wanted) {
    for (size_t i = at + 1; i + 1 < at + length; ++i) {
        const uint32_t token = tokens[i];
        if ((token & 0x00100000u) && ((token >> 12) & 255u) == operandType &&
            tokens[i + 1] == wanted) return true;
    }
    return false;
}

// Instructions emitted by FXC 10.1 /O3 for
// tools/static_surface_test/static_surface_helper.hlsl:
//   fxc /nologo /T ps_5_0 /E main /O3 /Ges /Fo helper.dxbc /Fc helper.asm \
//       static_surface_helper.hlsl
// Declarations, the unrelated Target0 write and RET are excluded. The first
// 204 words compute the key; the final ten move position.z and the key to
// Target7. Temp and input operands are relocated before insertion. The source
// file is the auditable/regenerable definition of the hash and bone guard;
// this array is its restricted token form for injection into an existing PS.
inline std::vector<uint32_t> helperInstructions(uint32_t tempBase,
                                                const StaticSurfaceShaderInputs& inputs) {
    static const uint32_t source[] = {
        0x07000001u,0x00100012u,0x00000000u,0x0010100Au,0x00000000u,0x00004001u,0x7FFFFFFFu,0x87000079u,
        0x800A8302u,0x00199983u,0x00100022u,0x00000000u,0x00107E16u,0x0000007Fu,0x0700004Fu,0x00100022u,
        0x00000000u,0x0010000Au,0x00000000u,0x0010001Au,0x00000000u,0x0304001Fu,0x0010001Au,0x00000000u,
        0x8B0000A7u,0x800A8302u,0x00199983u,0x001000F2u,0x00000001u,0x0010000Au,0x00000000u,0x00004001u,
        0x00000000u,0x00107E46u,0x0000007Fu,0x8B0000A7u,0x800A8302u,0x00199983u,0x00100072u,0x00000000u,
        0x0010000Au,0x00000000u,0x00004001u,0x00000010u,0x00107246u,0x0000007Fu,0x08000057u,0x00100072u,
        0x00000002u,0x00100556u,0x00000001u,0x00208246u,0x0000000Du,0x00000000u,0x0A000057u,0x00100072u,
        0x00000002u,0x00100246u,0x00000002u,0x00004002u,0x9E3779B9u,0x85EBCA6Bu,0xC2B2AE35u,0x00000000u,
        0x0B000026u,0x0000D000u,0x00100072u,0x00000002u,0x00100246u,0x00000002u,0x00004002u,0x85EBCA6Bu,
        0xC2B2AE35u,0x27D4EB2Fu,0x00000000u,0x07000057u,0x00100072u,0x00000002u,0x00100AA6u,0x00000001u,
        0x00100246u,0x00000002u,0x0B000026u,0x0000D000u,0x00100072u,0x00000002u,0x00100246u,0x00000002u,
        0x00004002u,0x85EBCA6Bu,0xC2B2AE35u,0x27D4EB2Fu,0x00000000u,0x07000057u,0x001000E2u,0x00000001u,
        0x00100FF6u,0x00000001u,0x00100906u,0x00000002u,0x0B000026u,0x0000D000u,0x001000E2u,0x00000001u,
        0x00100E56u,0x00000001u,0x00004002u,0x00000000u,0x85EBCA6Bu,0xC2B2AE35u,0x27D4EB2Fu,0x07000057u,
        0x001000E2u,0x00000001u,0x00100006u,0x00000000u,0x00100E56u,0x00000001u,0x0B000026u,0x0000D000u,
        0x001000E2u,0x00000001u,0x00100E56u,0x00000001u,0x00004002u,0x00000000u,0x85EBCA6Bu,0xC2B2AE35u,
        0x27D4EB2Fu,0x07000057u,0x001000B2u,0x00000000u,0x00100556u,0x00000000u,0x00100D96u,0x00000001u,
        0x0B000026u,0x0000D000u,0x001000B2u,0x00000000u,0x00100C46u,0x00000000u,0x00004002u,0x85EBCA6Bu,
        0xC2B2AE35u,0x00000000u,0x27D4EB2Fu,0x07000057u,0x00100072u,0x00000000u,0x00100AA6u,0x00000000u,
        0x00100346u,0x00000000u,0x0B000026u,0x0000D000u,0x00100072u,0x00000000u,0x00100246u,0x00000000u,
        0x00004002u,0x85EBCA6Bu,0xC2B2AE35u,0x27D4EB2Fu,0x00000000u,0x0A000055u,0x001000E2u,0x00000001u,
        0x00100906u,0x00000000u,0x00004002u,0x00000000u,0x00000010u,0x0000000Du,0x0000000Fu,0x07000057u,
        0x00100072u,0x00000000u,0x00100246u,0x00000000u,0x00100796u,0x00000001u,0x0C000037u,0x00100072u,
        0x00000000u,0x00100006u,0x00000001u,0x00004002u,0x00000000u,0x00000000u,0x00000000u,0x00000000u,
        0x00100246u,0x00000000u,0x01000012u,0x08000036u,0x00100072u,0x00000000u,0x00004002u,0x00000000u,
        0x00000000u,0x00000000u,0x00000000u,0x01000015u,0x05000036u,0x00100082u,0x00000000u,0x0010102Au,
        0x00000001u,0x05000036u,0x001020F2u,0x00000007u,0x00100E46u,0x00000000u
    };
    std::vector<uint32_t> result(source, source + sizeof(source) / sizeof(source[0]));
    for (size_t i = 0; i + 1 < result.size(); ++i) {
        const uint32_t token = result[i];
        if (!(token & 0x00100000u)) continue;
        const uint32_t type = (token >> 12) & 255u;
        if (type == 0) { // TEMP
            if (result[i + 1] > 2) throw std::runtime_error("helper temp");
            result[i + 1] += tempBase;
            ++i;
        } else if (type == 1) { // INPUT
            if (result[i + 1] == 0) {
                result[i] = (result[i] & ~0x30u) | (inputs.identityComponent << 4);
                result[i + 1] = inputs.identityRegister;
            } else if (result[i + 1] == 1) {
                result[i + 1] = inputs.positionRegister;
            } else throw std::runtime_error("helper input");
            ++i;
        }
    }
    return result;
}

inline void appendWords(std::vector<uint32_t>& output, const uint32_t* first, size_t count) {
    output.insert(output.end(), first, first + count);
}

inline std::vector<BYTE> patchProgram(const std::vector<BYTE>& bytes,
                                      const StaticSurfaceShaderInputs& inputs) {
    std::vector<uint32_t> tokens(bytes.size() / 4);
    std::memcpy(tokens.data(), bytes.data(), bytes.size());
    size_t firstNonCb = 0, firstInput = 0, firstOutput = 0, tempAt = 0;
    uint32_t tempCount = 0, returns = 0;
    bool identityDeclared = false, positionDeclared = false;
    for (size_t at = 2; at < tokens.size();) {
        const uint32_t opcode = tokens[at] & 0x7ffu;
        const uint32_t length = instructionLength(tokens, at);
        if (!firstNonCb && opcode != 106 && opcode != 89) firstNonCb = at;
        if (!firstInput && opcode >= 95 && opcode <= 100) firstInput = at;
        if (!firstOutput && opcode >= 101 && opcode <= 103) firstOutput = at;
        if (opcode == 89 && length >= 3 && tokens[at + 2] == 13)
            throw std::runtime_error("constant buffer b13 occupied");
        if ((opcode == 88 || opcode == 161 || opcode == 162) && length >= 3 &&
            tokens[at + 2] == 127)
            throw std::runtime_error("resource t127 occupied");
        if (opcode >= 156 && opcode <= 160) throw std::runtime_error("UAV or thread-group declaration");
        if (opcode == 120 || (opcode >= 144 && opcode <= 146))
            throw std::runtime_error("dynamic linkage");
        if (opcode >= 98 && opcode <= 100 && length >= 3 &&
            ((tokens[at + 1] >> 12) & 255u) == 1) {
            const uint32_t reg = tokens[at + 2];
            const uint32_t mask = (tokens[at + 1] >> 4) & 15u;
            if (reg == inputs.identityRegister && (mask & (1u << inputs.identityComponent)))
                identityDeclared = true;
            if (reg == inputs.positionRegister) {
                positionDeclared = true;
                tokens[at + 1] |= 0x40u; // make position.z live
            }
        }
        if (opcode >= 101 && opcode <= 103 && length >= 3 &&
            ((tokens[at + 1] >> 12) & 255u) == 2 && tokens[at + 2] >= 6)
            throw std::runtime_error("output target 6 or 7 occupied");
        if (opcode == 104) {
            if (tempAt || length != 2 || tokens[at + 1] > 4092)
                throw std::runtime_error("temps");
            tempAt = at;
            tempCount = tokens[at + 1];
        }
        if (opcode == 62) {
            ++returns;
            if (length != 1 || at + 1 != tokens.size()) throw std::runtime_error("early return");
        }
        if (opcode == 63) throw std::runtime_error("conditional return");
        at += length;
    }
    if (!firstNonCb || !firstInput || !firstOutput || !identityDeclared || returns != 1)
        throw std::runtime_error(!identityDeclared ? "identity input not declared" : "program declarations");

    std::vector<uint32_t> output;
    output.reserve(tokens.size() + 230);
    output.push_back(tokens[0]);
    output.push_back(0);
    const uint32_t cb[] = {0x04000059u, 0x00208E46u, 13u, 1u};
    const uint32_t pool[] = {0x040000A2u, 0x00107000u, 127u, 336u};
    const uint32_t pos[] = {0x04002064u, 0x00101042u, inputs.positionRegister, 1u};
    const uint32_t owner[] = {0x03000065u, 0x001020F2u, 7u};
    for (size_t at = 2; at < tokens.size();) {
        const uint32_t opcode = tokens[at] & 0x7ffu;
        const uint32_t length = instructionLength(tokens, at);
        if (at == firstNonCb) appendWords(output, cb, 4);
        if (at == firstInput) appendWords(output, pool, 4);
        if (at == firstOutput && !positionDeclared) appendWords(output, pos, 4);
        if ((tempAt && at == tempAt) || (!tempAt && opcode < 88)) appendWords(output, owner, 3);
        if (opcode == 104) {
            output.push_back(tokens[at]);
            output.push_back(tempCount + 3);
        } else if (opcode == 62) {
            const auto helper = helperInstructions(tempCount, inputs);
            output.insert(output.end(), helper.begin(), helper.end());
            output.push_back(tokens[at]);
        } else {
            output.insert(output.end(), tokens.begin() + at, tokens.begin() + at + length);
        }
        at += length;
    }
    if (!tempAt) {
        // Every accepted real material shader has temps.  Keep the no-temp
        // path explicit and fail closed rather than placing dcl_temps after an
        // executable instruction.
        throw std::runtime_error("missing temps declaration");
    }
    output[1] = static_cast<uint32_t>(output.size());
    std::vector<BYTE> result(output.size() * 4);
    std::memcpy(result.data(), output.data(), result.size());
    return result;
}

} // namespace dxbc_static_surface_detail

inline bool staticSurfaceDeriveShaderInputs(const void* data, size_t bytes,
                                            StaticSurfaceShaderInputs& output,
                                            std::string& reason) {
    output = {};
    reason.clear();
    try {
        auto chunks = dxbc_static_surface_detail::parseContainer(data, bytes, 0x00010050u); // vs_5_0
        bool signature = false;
        for (const auto& chunk : chunks) if (chunk.tag == 0x4e47534fu) { // OSGN
            if (signature) throw std::runtime_error("duplicate output signature");
            signature = true;
            const auto elements = dxbc_static_surface_detail::parseSignature(chunk.bytes);
            for (const auto& e : elements) {
                if ((dxbc_static_surface_detail::equalName(e.name, "__USER_MATERIALMODULATION_DATAID") &&
                     (e.masks & 2u)) ||
                    (dxbc_static_surface_detail::equalName(e.name, "__USER_VERTEX_FACEINVARIANT") &&
                     (e.masks & 1u))) {
                    if (e.componentType != 1 || output.identityRegister != ~0u)
                        throw std::runtime_error("ambiguous identity output");
                    output.identityRegister = e.registerIndex;
                    output.identityComponent = dxbc_static_surface_detail::equalName(
                        e.name, "__USER_MATERIALMODULATION_DATAID") ? 1u : 0u;
                }
                if ((e.systemValue == 1 || dxbc_static_surface_detail::equalName(e.name, "SV_POSITION")) &&
                    (e.masks & 4u)) {
                    if (e.componentType != 3 || output.positionRegister != ~0u)
                        throw std::runtime_error("ambiguous position output");
                    output.positionRegister = e.registerIndex;
                }
            }
        }
        if (!signature || output.identityRegister >= 32 || output.identityComponent >= 4 ||
            output.positionRegister >= 32)
            throw std::runtime_error("supported rigid identity/position outputs absent");
        return true;
    } catch (const std::exception& e) {
        reason = e.what();
        output = {};
        return false;
    }
}

inline bool staticSurfacePatch(const void* data, size_t bytes,
                               const StaticSurfaceShaderInputs& inputs,
                               std::vector<BYTE>& output, std::string& reason) {
    output.clear();
    reason.clear();
    if (inputs.identityRegister >= 32 || inputs.identityComponent >= 4 || inputs.positionRegister >= 32) {
        reason = "invalid shader inputs";
        return false;
    }
    try {
        auto chunks = dxbc_static_surface_detail::parseContainer(data, bytes, 0x00000050u); // ps_5_0
        bool inputSignature = false, outputSignature = false, program = false;
        for (auto& chunk : chunks) {
            if (chunk.tag == 0x4e475349u) { // ISGN
                if (inputSignature) throw std::runtime_error("duplicate input signature");
                inputSignature = true;
                auto elements = dxbc_static_surface_detail::parseSignature(chunk.bytes);
                bool identity = false, position = false;
                for (auto& e : elements) {
                    if (e.registerIndex == inputs.identityRegister && e.componentType == 1 &&
                        (e.masks & (1u << inputs.identityComponent))) identity = true;
                    if ((e.systemValue == 1 || dxbc_static_surface_detail::equalName(e.name, "SV_POSITION")) &&
                        e.registerIndex == inputs.positionRegister) {
                        if (e.componentType != 3) throw std::runtime_error("position input type");
                        e.masks |= 4u | 0x0400u;
                        position = true;
                    }
                }
                if (!identity) throw std::runtime_error("identity input absent");
                if (!position) {
                    dxbc_static_surface_detail::SignatureElement p;
                    p.name = "SV_Position";
                    p.systemValue = 1;
                    p.componentType = 3;
                    p.registerIndex = inputs.positionRegister;
                    p.masks = 0x040fu;
                    elements.push_back(std::move(p));
                }
                chunk.bytes = dxbc_static_surface_detail::makeSignature(elements);
            } else if (chunk.tag == 0x4e47534fu) { // OSGN
                if (outputSignature) throw std::runtime_error("duplicate output signature");
                outputSignature = true;
                auto elements = dxbc_static_surface_detail::parseSignature(chunk.bytes);
                bool target = false;
                for (const auto& e : elements) {
                    if (dxbc_static_surface_detail::startsWithName(e.name, "SV_DEPTH"))
                        throw std::runtime_error("depth output");
                    if (dxbc_static_surface_detail::equalName(e.name, "SV_TARGET")) {
                        target = true;
                        if (e.registerIndex >= 6) throw std::runtime_error("output target 6 or 7 occupied");
                    }
                }
                if (!target) throw std::runtime_error("no colour output");
                dxbc_static_surface_detail::SignatureElement owner;
                owner.name = "SV_Target";
                owner.semanticIndex = 7;
                owner.componentType = 1;
                owner.registerIndex = 7;
                owner.masks = 0x0fu;
                elements.push_back(std::move(owner));
                chunk.bytes = dxbc_static_surface_detail::makeSignature(elements);
            } else if (chunk.tag == 0x58454853u || chunk.tag == 0x52444853u) {
                if (program) throw std::runtime_error("duplicate program");
                program = true;
                chunk.bytes = dxbc_static_surface_detail::patchProgram(chunk.bytes, inputs);
            }
        }
        if (!inputSignature || !outputSignature || !program) throw std::runtime_error("missing shader chunks");
        output = dxbc_static_surface_detail::makeContainer(chunks);
        return true;
    } catch (const std::exception& e) {
        reason = e.what();
        output.clear();
        return false;
    }
}

} // namespace edvr
