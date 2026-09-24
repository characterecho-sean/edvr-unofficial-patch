#pragma once

// DXBC container and signature helpers for the restricted SM5 patchers
// (engine-record velocity, dxbc_engine_velocity.h): parse a container and
// check its retail checksum, rebuild one with a fresh checksum, parse and
// rebuild an input/output signature, and measure an instruction. Moved here
// on 2026-09-23 from the retired static owner's patcher, whose shader tail
// went with it; nothing here patches a program by itself.

#include <windows.h>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

void ComputeHashRetail(const BYTE*, UINT, BYTE*);

namespace edvr {
namespace dxbc_container {

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

} // namespace dxbc_container
} // namespace edvr
