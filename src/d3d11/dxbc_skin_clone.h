#pragma once

// F2: the second skin. A skinned character's vertex shader computes the
// vertex's camera-relative position from the CURRENT bone palette (t38) and the
// CURRENT pose record (t33). This patcher appends, after that computation, a
// CLONE of the game's own instructions that does the same from the PREVIOUS
// frame's palette and pose, and exports the difference as one extra float4
// output (E.xyz = previous - current in centimetres, E.w = valid). The clone is
// the game's code, token for token, with its temporaries renamed and four
// resources redirected, so the previous position is computed by the same
// operations in the same order as the current one. The identity test
// (previous state == current state gives E == 0 exactly) therefore proves the
// clone reproduces the game's chain, on the real shaders, bit for bit.
//
// What the patch does, exactly (nothing else in the program changes):
//   * finds the pose chain: the one immediate-offset-0 and the one
//     immediate-offset-16 load of t33 at v0.x, the viewport-matrix multiplies
//     (cb1[270..272]) that name the position register and its components, and
//     the ANCHOR, the last top-level `add` that writes exactly those
//     components before the first top-level branch that follows the pose load
//     (the cosmetic displacement block) or the matrix multiplies;
//   * after the anchor: a preamble (current base from t33, previous base from
//     the join table t109), then the clone of every instruction from the first
//     executable one through the anchor, in a fresh bank of temporaries, with
//     output writes dropped, t38 -> t108 (previous palette), the two pose
//     loads -> t110 (previous pose, indexed by the joined previous base), then
//     E from the two position registers;
//   * before the single `ret`: mov o<skinRegister>, E.
// Every opcode in the cloned range must be one of the 28 the five measured
// shaders use; anything else, any relative addressing, any resource at t108..t110
// already, any second ret, declines with a reason and the shader stays
// unpatched (no E, so the compose treats its pixels as no-history).
//
// Pure token logic (no D3D); the container and signature work is the caller's.

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include "dxbc_container.h"

namespace edvr {

constexpr uint32_t kSkinPrevPaletteSlot = 108;  // previous frame's palette, 48-byte rows
constexpr uint32_t kSkinJoinSlot = 109;         // current base -> previous base (0 = no history), 4-byte elements
constexpr uint32_t kSkinPoseSlot = 110;         // previous pose by previous base, 32-byte elements
constexpr uint32_t kSkinPoseStrideBytes = 32;   // record bytes 0..31: words 0..3, position
constexpr uint32_t kSkinTarget = 7;             // the render target the pixel shader exports E to
constexpr char kSkinSemantic[] = "EDVRSKINPREV";
constexpr float kSkinCentimetres = 100.0f;      // E is previous - current, metres to centimetres
constexpr float kSkinLimitSquared = 3.6e9f;     // |E| above 60000 cm is not a motion; it is invalid
// The engine path binds the three views with one VSSetShaderResources call (engine_velocity.cpp), and the patch declares them in this order.
static_assert(kSkinJoinSlot == kSkinPrevPaletteSlot + 1 && kSkinPoseSlot == kSkinPrevPaletteSlot + 2, "t108, t109 and t110 are bound with one call: they must be contiguous");

namespace dxbc_skin_detail {

using dxbc_container::instructionLength;

constexpr uint32_t kAdd = 0, kAnd = 1, kBreakc = 3, kDp3 = 16, kDp4 = 17, kElse = 18, kEndif = 21, kEndloop = 22,
                   kExp = 25, kGe = 29, kIadd = 30, kIf = 31, kIeq = 32, kImad = 35, kIne = 39, kIshl = 41,
                   kItof = 43, kLoop = 48, kMad = 50, kMov = 54, kMovc = 55, kMul = 56, kRet = 62,
                   kUlt = 79, kUshr = 85, kUtof = 86, kRcp = 129, kUbfe = 138, kBfi = 140, kLdStructured = 167;
constexpr uint32_t kDclResourceStructured = 162, kDclInput = 95, kDclInputSiv = 97, kDclInputPs = 98,
                   kDclOutput = 101, kDclOutputSiv = 103, kDclTemps = 104, kDclIndexableTemp = 105,
                   kDclResource = 88, kCustomData = 53;
constexpr uint32_t kTypeTemp = 0, kTypeInput = 1, kTypeOutput = 2, kTypeImm32 = 4, kTypeResource = 7, kTypeCb = 8;

inline uint32_t opcodeOf(uint32_t token) { return token & 0x7ffu; }
inline uint32_t typeOf(uint32_t token) { return (token >> 12) & 255u; }

struct OpcodeShape {
    uint32_t opcode;
    bool dst;           // first operand is a destination
    unsigned operands;  // total operand count
};
// The 28 opcodes the five measured skinned shaders use between their first
// instruction and the anchor. Anything else declines.
constexpr OpcodeShape kShapes[] = {
    {kAdd, true, 3},  {kAnd, true, 3},  {kBreakc, false, 1}, {kDp3, true, 3}, {kDp4, true, 3},
    {kElse, false, 0}, {kEndif, false, 0}, {kEndloop, false, 0}, {kExp, true, 2}, {kIadd, true, 3},
    {kIeq, true, 3},  {kIf, false, 1},  {kImad, true, 4},  {kIne, true, 3},   {kIshl, true, 3},
    {kItof, true, 2}, {kLoop, false, 0}, {kMad, true, 4},  {kMov, true, 2},   {kMovc, true, 4},
    {kMul, true, 3},  {kUlt, true, 3},  {kUshr, true, 3},  {kUtof, true, 2},  {kRcp, true, 2},
    {kUbfe, true, 4}, {kBfi, true, 5},  {kLdStructured, true, 4},
};
inline const OpcodeShape* shapeOf(uint32_t opcode) {
    for (const auto& s : kShapes) if (s.opcode == opcode) return &s;
    return nullptr;
}

// The length in tokens of the operand that starts at `at` (extended tokens,
// index words, immediates). Relative addressing and 64-bit forms decline.
inline size_t operandLength(const std::vector<uint32_t>& t, size_t at, size_t limit) {
    if (at >= limit) throw std::runtime_error("operand bounds");
    const uint32_t tok = t[at];
    size_t n = 1;
    bool more = (tok >> 31) != 0;
    while (more) {
        if (at + n >= limit) throw std::runtime_error("operand extension bounds");
        more = (t[at + n] >> 31) != 0;
        ++n;
    }
    const uint32_t dim = (tok >> 20) & 3u;
    for (uint32_t i = 0; i < dim; ++i) {
        const uint32_t repr = (tok >> (22 + 3 * i)) & 7u;
        if (repr != 0) throw std::runtime_error("relative or 64-bit operand index");
        ++n;
    }
    const uint32_t type = typeOf(tok);
    if (type == kTypeImm32) {
        const uint32_t comps = tok & 3u;
        if (comps == 1) n += 1;
        else if (comps == 2) n += 4;
        else throw std::runtime_error("immediate operand shape");
    } else if (type == 5) {
        throw std::runtime_error("64-bit immediate");
    }
    if (at + n > limit) throw std::runtime_error("operand overruns instruction");
    return n;
}
// Where the first index word of the operand at `at` sits (after any extended tokens).
inline size_t indexWordAt(const std::vector<uint32_t>& t, size_t at) {
    size_t pos = at + 1;
    bool more = (t[at] >> 31) != 0;
    while (more) { more = (t[pos] >> 31) != 0; ++pos; }
    return pos;
}

struct Instr {
    size_t at = 0, length = 0;
    uint32_t opcode = 0;
    std::vector<size_t> operand;      // token index of each operand
    std::vector<size_t> operandLen;
};
inline Instr parseInstr(const std::vector<uint32_t>& t, size_t at) {
    Instr in;
    in.at = at;
    in.length = instructionLength(t, at);
    in.opcode = opcodeOf(t[at]);
    size_t pos = at + 1;
    bool more = (t[at] >> 31) != 0;
    while (more) {
        if (pos >= at + in.length) throw std::runtime_error("opcode extension bounds");
        more = (t[pos] >> 31) != 0;
        ++pos;
    }
    while (pos < at + in.length) {
        const size_t n = operandLength(t, pos, at + in.length);
        in.operand.push_back(pos);
        in.operandLen.push_back(n);
        pos += n;
    }
    return in;
}

inline bool isDeclaration(uint32_t opcode) {
    return (opcode >= 88 && opcode <= 106) || opcode == 143 || (opcode >= 147 && opcode <= 163) || opcode == kCustomData;
}

struct Plan {
    uint32_t temps = 0;                 // N, the program's own temporary count
    size_t tempsAt = 0;                 // dcl_temps token index
    size_t firstExec = 0;               // first executable instruction
    size_t anchorEnd = 0;               // token index just past the anchor
    size_t firstDclInput = 0;           // where the resource declarations go
    size_t lastOutputEnd = 0;           // where the E output declaration goes
    size_t baseLoadAt = 0, posLoadAt = 0;
    uint32_t posReg = 0;
    uint32_t comp[3]{0, 0, 0};
    bool skinPaletteSeen = false;
};

inline Plan analyse(const std::vector<uint32_t>& t) {
    Plan plan;
    bool tempsSeen = false, t33 = false, t38 = false, v0x = false;
    std::vector<Instr> body;
    size_t retCount = 0;
    for (size_t at = 2; at < t.size();) {
        const uint32_t op = opcodeOf(t[at]);
        const uint32_t len = instructionLength(t, at);
        if (isDeclaration(op)) {
            if (!body.empty()) throw std::runtime_error("declaration after code");
            if (op == kDclIndexableTemp) throw std::runtime_error("indexable temporaries");
            if (op == kDclTemps) {
                if (len != 2 || tempsSeen) throw std::runtime_error("temps declaration");
                tempsSeen = true;
                plan.temps = t[at + 1];
                plan.tempsAt = at;
            }
            if (op == kDclResourceStructured && len == 4 && typeOf(t[at + 1]) == kTypeResource) {
                if (t[at + 2] == 33 && t[at + 3] == 336) t33 = true;
                if (t[at + 2] == 38 && t[at + 3] == 48) t38 = true;
            }
            if ((op == kDclResourceStructured || op == kDclResource) && len >= 3 && typeOf(t[at + 1]) == kTypeResource &&
                t[at + 2] >= kSkinPrevPaletteSlot && t[at + 2] <= kSkinPoseSlot)
                throw std::runtime_error("skin resource slot occupied");
            if (op == kDclInput && len == 3 && typeOf(t[at + 1]) == kTypeInput && t[at + 2] == 0 &&
                (((t[at + 1] >> 4) & 15u) & 1u)) v0x = true;
            if (op >= kDclInput && op <= kDclInputPs + 2 && !plan.firstDclInput) plan.firstDclInput = at;
            if (op >= kDclOutput && op <= kDclOutputSiv) plan.lastOutputEnd = at + len;
        } else {
            if (!plan.firstExec) plan.firstExec = at;
            body.push_back(parseInstr(t, at));
            if (op == kRet) ++retCount;
        }
        at += len;
    }
    if (!tempsSeen || plan.temps == 0 || plan.temps > 2000) throw std::runtime_error("temp count");
    if (!t33 || !t38) throw std::runtime_error("t33 or t38 declaration missing");
    if (!v0x) throw std::runtime_error("v0.x not declared");
    if (!plan.firstDclInput || !plan.lastOutputEnd) throw std::runtime_error("no input or output declarations");
    if (retCount != 1 || body.empty() || body.back().opcode != kRet) throw std::runtime_error("not exactly one trailing return");

    // Control-flow depth before each instruction (top level is 0).
    std::vector<int> depth(body.size());
    int d = 0;
    for (size_t i = 0; i < body.size(); ++i) {
        const uint32_t op = body[i].opcode;
        if (op == kEndif || op == kEndloop) --d;
        depth[i] = (op == kElse) ? d - 1 : d;
        if (op == kIf || op == kLoop) ++d;
        if (d < 0) throw std::runtime_error("unbalanced control flow");
    }
    if (d != 0) throw std::runtime_error("unbalanced control flow");

    // The two pose loads: t33 at v0.x, immediate byte offsets 0 and 16.
    const auto isV0x = [&](const Instr& in, size_t k) {
        return in.operandLen[k] == 2 && t[in.operand[k]] == 0x0010100Au && t[in.operand[k] + 1] == 0;
    };
    size_t baseLoad = ~size_t(0), posLoad = ~size_t(0);
    unsigned baseLoads = 0, posLoads = 0, paletteLoads = 0;
    for (size_t i = 0; i < body.size(); ++i) {
        const Instr& in = body[i];
        if (in.opcode != kLdStructured) continue;
        const size_t res = in.operand[3];
        if (typeOf(t[res]) != kTypeResource || ((t[res] >> 20) & 3u) != 1) throw std::runtime_error("structured load resource shape");
        const uint32_t slot = t[indexWordAt(t, res)];
        if (slot == 38) ++paletteLoads;
        if (slot != 33) continue;
        const size_t off = in.operand[2];
        if (typeOf(t[off]) == kTypeImm32 && in.operandLen[2] == 2 && isV0x(in, 1)) {
            if (t[off + 1] == 0) { ++baseLoads; baseLoad = i; }
            if (t[off + 1] == 16) { ++posLoads; posLoad = i; }
        }
    }
    if (baseLoads != 1 || posLoads != 1) throw std::runtime_error("pose loads are not exactly one at offset 0 and one at 16");
    if (!paletteLoads) throw std::runtime_error("no t38 loads");
    plan.skinPaletteSeen = true;

    // The matrix multiplies after the pose load name the position register.
    size_t consumer[3] = {~size_t(0), ~size_t(0), ~size_t(0)};
    uint32_t reg = ~0u;
    for (size_t i = posLoad + 1; i < body.size(); ++i) {
        const Instr& in = body[i];
        if (in.opcode != kMul || in.operand.size() != 3) continue;
        const size_t cb = in.operand[2];
        if (typeOf(t[cb]) != kTypeCb || in.operandLen[2] != 3 || t[cb + 1] != 1) continue;
        const uint32_t row = t[cb + 2];
        if (row < 270 || row > 272 || consumer[row - 270] != ~size_t(0)) continue;
        const size_t src = in.operand[1];
        if (typeOf(t[src]) != kTypeTemp || ((t[src] >> 2) & 3u) != 1) throw std::runtime_error("matrix multiply source shape");
        const uint32_t sw = (t[src] >> 4) & 255u;
        const uint32_t c = sw & 3u;
        if (sw != (c | c << 2 | c << 4 | c << 6)) throw std::runtime_error("matrix multiply swizzle not replicated");
        const uint32_t r = t[indexWordAt(t, src)];
        if (reg != ~0u && r != reg) throw std::runtime_error("matrix multiplies read different registers");
        reg = r;
        plan.comp[row - 270] = c;
        consumer[row - 270] = i;
    }
    if (reg == ~0u || consumer[0] == ~size_t(0) || consumer[1] == ~size_t(0) || consumer[2] == ~size_t(0))
        throw std::runtime_error("matrix multiplies not found");
    plan.posReg = reg;
    // The end of the pose chain: the first top-level branch after the pose load,
    // else the first matrix multiply.
    size_t limit = consumer[0];
    for (size_t i = posLoad + 1; i < limit; ++i)
        if (depth[i] == 0 && (body[i].opcode == kIf || body[i].opcode == kLoop)) { limit = i; break; }
    size_t anchor = ~size_t(0);
    const uint32_t wantMask = (1u << plan.comp[0]) | (1u << plan.comp[1]) | (1u << plan.comp[2]);
    for (size_t i = limit; i-- > posLoad + 1;) {
        const Instr& in = body[i];
        if (depth[i] != 0 || in.opcode != kAdd || in.operand.size() != 3) continue;
        const size_t dst = in.operand[0];
        if (typeOf(t[dst]) != kTypeTemp || ((t[dst] >> 2) & 3u) != 0) continue;
        if (t[indexWordAt(t, dst)] != reg) continue;
        if (((t[dst] >> 4) & 15u) != wantMask) continue;
        anchor = i;
        break;
    }
    if (anchor == ~size_t(0)) throw std::runtime_error("anchor not found");
    if (baseLoad >= anchor) throw std::runtime_error("the base load follows the anchor");
    plan.baseLoadAt = body[baseLoad].at;
    plan.posLoadAt = body[posLoad].at;
    plan.anchorEnd = body[anchor].at + body[anchor].length;

    // Everything cloned must be a known opcode with the expected operand shape.
    for (size_t i = 0; i < body.size() && body[i].at < plan.anchorEnd; ++i) {
        const Instr& in = body[i];
        const OpcodeShape* shape = shapeOf(in.opcode);
        if (!shape) throw std::runtime_error("opcode " + std::to_string(in.opcode) + " in the cloned range");
        if (in.operand.size() != shape->operands) throw std::runtime_error("operand count in the cloned range");
        for (size_t k = 0; k < in.operand.size(); ++k) {
            const uint32_t type = typeOf(t[in.operand[k]]);
            if (type != kTypeTemp && type != kTypeInput && type != kTypeOutput && type != kTypeImm32 &&
                type != kTypeResource && type != kTypeCb) throw std::runtime_error("operand type in the cloned range");
            if (type == kTypeOutput && !(k == 0 && shape->dst)) throw std::runtime_error("output read in the cloned range");
            if (type == kTypeTemp && t[indexWordAt(t, in.operand[k])] >= plan.temps) throw std::runtime_error("temporary beyond the declaration");
            if (type == kTypeResource) {
                const uint32_t slot = t[indexWordAt(t, in.operand[k])];
                if (slot >= kSkinPrevPaletteSlot && slot <= kSkinPoseSlot) throw std::runtime_error("skin resource slot used");
            }
        }
        if (shape->dst && typeOf(t[in.operand[0]]) == kTypeImm32) throw std::runtime_error("immediate destination");
    }
    if (plan.temps * 2 + 2 > 4000) throw std::runtime_error("too many temporaries");
    return plan;
}

// The program's own temporary registers used after the anchor and the output
// registers it writes are untouched; the patch needs only the temp bank.
inline std::vector<uint32_t> patchProgram(const std::vector<uint32_t>& t, uint32_t skinRegister) {
    if (skinRegister >= 32) throw std::runtime_error("skin output register");
    const Plan plan = analyse(t);
    const uint32_t N = plan.temps;
    const uint32_t S = 2 * N;      // scratch: x = current base, y = previous base, z = |E|^2 test, w = valid mask
    const uint32_t E = 2 * N + 1;  // E.xyz, E.w = valid
    const uint32_t cloneReg = plan.posReg + N;

    std::vector<uint32_t> out;
    out.reserve(t.size() * 2 + 128);
    out.push_back(t[0]);
    out.push_back(0);
    const auto put = [&](std::initializer_list<uint32_t> words) { out.insert(out.end(), words.begin(), words.end()); };

    const uint32_t kTempMask[4] = {0x00100012u, 0x00100022u, 0x00100042u, 0x00100082u};      // r.x r.y r.z r.w (write mask)
    const uint32_t kTempSelect[4] = {0x0010000Au, 0x0010001Au, 0x0010002Au, 0x0010003Au};   // r.x r.y r.z r.w (select 1)
    const uint32_t kXyzw = 0x00100E46u;                                                      // r.xyzw swizzle
    const auto resource = [](uint32_t swizzle) { return 0x00107006u | (swizzle << 4); };      // t#.swizzle (index word follows)

    // 1. Declarations.
    for (size_t at = 2; at < plan.firstExec;) {
        const uint32_t len = instructionLength(t, at);
        if (at == plan.firstDclInput) {
            put({0x040000A2u, 0x00107000u, kSkinPrevPaletteSlot, 48u});
            put({0x040000A2u, 0x00107000u, kSkinJoinSlot, 4u});
            put({0x040000A2u, 0x00107000u, kSkinPoseSlot, kSkinPoseStrideBytes});
        }
        if (at == plan.tempsAt) {
            out.push_back(t[at]);
            out.push_back(2 * N + 2);
        } else {
            out.insert(out.end(), t.begin() + at, t.begin() + at + len);
        }
        if (at + len == plan.lastOutputEnd) put({0x03000065u, 0x001020F2u, skinRegister});   // dcl_output o<skin>.xyzw
        at += len;
    }
    // 2. The program up to and including the anchor, untouched.
    out.insert(out.end(), t.begin() + plan.firstExec, t.begin() + plan.anchorEnd);

    // 3. Preamble: S.x = current base (t33 word 0); S.y = previous base (join, 0 = none).
    // (the compiler's own form: ld_structured_indexable, an extended token naming the stride, one naming the
    // return type; 0x00199983 is the "mixed" return type the game's loads carry)
    const auto stride = [](uint32_t bytes) { return 0x80000302u | (bytes << 11); };
    put({0x8B0000A7u, stride(336), 0x00199983u, kTempMask[0], S, 0x0010100Au, 0u, 0x00004001u, 0u, resource(0x00), 33u});
    put({0x8B0000A7u, stride(4), 0x00199983u, kTempMask[1], S, kTempSelect[0], S, 0x00004001u, 0u, resource(0x00), kSkinJoinSlot});

    // 4. The clone: the game's instructions, temporaries in the bank above its own.
    for (size_t at = plan.firstExec; at < plan.anchorEnd;) {
        const Instr in = parseInstr(t, at);
        at += in.length;
        const OpcodeShape* shape = shapeOf(in.opcode);
        if (shape->dst && typeOf(t[in.operand[0]]) == kTypeOutput) continue;   // output writes are not cloned
        const bool poseLoad = in.at == plan.baseLoadAt || in.at == plan.posLoadAt;
        std::vector<uint32_t> copy(t.begin() + in.at, t.begin() + in.at + in.length);
        for (size_t k = 0; k < in.operand.size(); ++k) {
            const size_t o = in.operand[k] - in.at;
            const uint32_t type = typeOf(copy[o]);
            const size_t idx = indexWordAt(copy, o);
            if (type == kTypeTemp) copy[idx] += N;
            if (type == kTypeResource && in.opcode == kLdStructured) {
                if (copy[idx] == 38) copy[idx] = kSkinPrevPaletteSlot;
                else if (copy[idx] == 33 && poseLoad) copy[idx] = kSkinPoseSlot;
            }
        }
        if (poseLoad) {
            // address: v0.x -> S.y (the previous base); the pose table is indexed by it
            const size_t a = in.operand[1] - in.at;
            copy[a] = kTempSelect[1];
            copy[a + 1] = S;
            // the extended opcode token that names the resource's stride follows the new declaration
            size_t pos = 1;
            bool more = (copy[0] >> 31) != 0;
            while (more) {
                if ((copy[pos] & 0x3Fu) == 2u) copy[pos] = (copy[pos] & ~(0xFFFu << 11)) | (kSkinPoseStrideBytes << 11);
                more = (copy[pos] >> 31) != 0;
                ++pos;
            }
        }
        out.insert(out.end(), copy.begin(), copy.end());
    }

    // 5. E = (clone - original) * 100 at the anchor's three components; zero and
    //    invalid when there is no previous base or the difference is not finite
    //    and below 60000 cm.
    // add E.xyz, clone.swz, -original.swz: the original is negated through an extended operand
    // token (operand token with bit 31, the modifier token 0x41, then the index word).
    const uint32_t swz = 0x00100006u | ((plan.comp[0] | plan.comp[1] << 2 | plan.comp[2] << 4 | plan.comp[2] << 6) << 4);
    put({0x08000000u, 0x00100072u, E, swz, cloneReg, swz | 0x80000000u, 0x00000041u, plan.posReg});
    // mul E.xyz, E.xyzw, l(100)
    put({0x0A000038u, 0x00100072u, E, kXyzw, E, 0x00004002u, 0x42C80000u, 0x42C80000u, 0x42C80000u, 0x42C80000u});
    // dp3 S.z, E, E
    put({0x07000010u, kTempMask[2], S, kXyzw, E, kXyzw, E});
    // ine S.w, S.y, l(0)
    put({0x07000027u, kTempMask[3], S, kTempSelect[1], S, 0x00004001u, 0u});
    // ge S.z, l(limit), S.z
    uint32_t limitBits = 0;
    std::memcpy(&limitBits, &kSkinLimitSquared, sizeof(limitBits));
    put({0x0700001Du, kTempMask[2], S, 0x00004001u, limitBits, kTempSelect[2], S});
    // and S.w, S.w, S.z
    put({0x07000001u, kTempMask[3], S, kTempSelect[3], S, kTempSelect[2], S});
    // movc E.xyz, S.wwww, E.xyzw, l(0,0,0,0)
    put({0x0C000037u, 0x00100072u, E, 0x00100FF6u, S, kXyzw, E, 0x00004002u, 0u, 0u, 0u, 0u});
    // movc E.w, S.w, l(1.0), l(0)
    put({0x09000037u, kTempMask[3], E, kTempSelect[3], S, 0x00004001u, 0x3F800000u, 0x00004001u, 0u});

    // 6. The rest of the program; the export before the single ret.
    for (size_t at = plan.anchorEnd; at < t.size();) {
        const uint32_t len = instructionLength(t, at);
        if (opcodeOf(t[at]) == kRet) put({0x05000036u, 0x001020F2u, skinRegister, kXyzw, E});
        out.insert(out.end(), t.begin() + at, t.begin() + at + len);
        at += len;
    }
    out[1] = static_cast<uint32_t>(out.size());
    return out;
}

} // namespace dxbc_skin_detail

// Analysis only: can this vertex shader take the second skin? The reason is
// empty on success. Never throws.
inline bool engineVelocitySkinCapable(const void* data, size_t bytes, std::string& reason) {
    using namespace dxbc_skin_detail;
    reason.clear();
    try {
        const auto chunks = dxbc_container::parseContainer(data, bytes, 0x00010050u);
        for (const auto& chunk : chunks) {
            if (chunk.tag != 0x58454853u && chunk.tag != 0x52444853u) continue;
            std::vector<uint32_t> words(chunk.bytes.size() / 4);
            std::memcpy(words.data(), chunk.bytes.data(), words.size() * 4);
            analyse(words);
            return true;
        }
        throw std::runtime_error("no program chunk");
    } catch (const std::exception& e) {
        reason = e.what();
        return false;
    }
}

// The patched container: the program, and the output signature with the new
// float4 element at `skinRegister`. `skinRegister` must be free.
inline bool engineVelocityPatchVsSkin(const void* data, size_t bytes, uint32_t skinRegister,
                                      std::vector<BYTE>& output, std::string& reason) {
    using namespace dxbc_skin_detail;
    output.clear();
    reason.clear();
    try {
        auto chunks = dxbc_container::parseContainer(data, bytes, 0x00010050u);
        bool osgn = false, program = false;
        for (auto& chunk : chunks) {
            if (chunk.tag == 0x4e47534fu) {   // OSGN
                if (osgn) throw std::runtime_error("duplicate output signature");
                osgn = true;
                auto elements = dxbc_container::parseSignature(chunk.bytes);
                for (const auto& e : elements)
                    if (e.registerIndex == skinRegister) throw std::runtime_error("skin output register occupied");
                dxbc_container::SignatureElement skin;
                skin.name = kSkinSemantic;
                skin.componentType = 3;                   // float
                skin.registerIndex = skinRegister;
                skin.masks = 0x000Fu;                     // xyzw, all written
                elements.push_back(std::move(skin));
                std::stable_sort(elements.begin(), elements.end(),
                    [](const dxbc_container::SignatureElement& a, const dxbc_container::SignatureElement& b) {
                        return a.registerIndex < b.registerIndex;
                    });
                chunk.bytes = dxbc_container::makeSignature(elements);
            } else if (chunk.tag == 0x58454853u || chunk.tag == 0x52444853u) {
                if (program) throw std::runtime_error("duplicate program");
                program = true;
                std::vector<uint32_t> words(chunk.bytes.size() / 4);
                std::memcpy(words.data(), chunk.bytes.data(), words.size() * 4);
                words = patchProgram(words, skinRegister);
                chunk.bytes.resize(words.size() * 4);
                std::memcpy(chunk.bytes.data(), words.data(), chunk.bytes.size());
            }
        }
        if (!osgn || !program) throw std::runtime_error("missing vertex shader chunks");
        output = dxbc_container::makeContainer(chunks);
        return true;
    } catch (const std::exception& e) {
        reason = e.what();
        output.clear();
        return false;
    }
}

} // namespace edvr
