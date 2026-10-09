// skin_clone_test: the second skin (src/d3d11/dxbc_skin_clone.h and the pixel half in dxbc_engine_velocity.h), on WARP.
//
//   --dry-run          the same run (the rig never writes a file), for the gate's --dry-run convention
//   --self-test [root] every check below
//   --corpus DIR       also the game's own five skinned pairs from an edvr_logs dump (DIR\shaders\*.dxbc), local only: the game's
//                      shaders are not in the repository. Case K9.
//
// There is no oracle flight for this patch: these properties are the proof (tools/engine_velocity_test/skin_clone_tests.h states them).
// Cases (each check is labelled "K<case>.<what>"; mutants.py names the case that must catch each mutation):
//   K1  the token walker: operands, instructions, extended tokens, what it refuses
//   K2  what the patch makes: signatures, declarations, the untouched prefix, one return, the pixel half
//   K3  what it declines, and why
//   K4  the properties on a vertex shader written to the game's shape
//   K5  the same with the displacement block the game's shaders have after the pose chain
//   K9  the same on the game's own shaders (--corpus only)
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#include "../engine_velocity_test/skin_clone_tests.h"
#include "synthetic_skin.h"
#include "../../third_party/dxbc_hash/DxilHash.cpp"

using Microsoft::WRL::ComPtr;
using edvr::dxbc_skin_detail::analyse;

namespace {
unsigned g_checks = 0, g_failures = 0;
void check(bool ok, const char* label) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("FAIL: %s\n", label);
        std::fflush(stdout);
    }
}
bool has(const std::string& text, const char* needle) { return text.find(needle) != std::string::npos; }

std::vector<BYTE> compile(const std::string& source, const char* profile) {
    ComPtr<ID3DBlob> code, errors;
    if (FAILED(D3DCompile(source.data(), source.size(), "synthetic", nullptr, nullptr, "main", profile, 0, 0, &code, &errors))) {
        std::printf("FAIL: K2.compile -- the synthetic shader did not compile: %s\n", errors ? static_cast<const char*>(errors->GetBufferPointer()) : "?");
        ++g_failures;
        return {};
    }
    return std::vector<BYTE>(static_cast<BYTE*>(code->GetBufferPointer()), static_cast<BYTE*>(code->GetBufferPointer()) + code->GetBufferSize());
}
std::string disassemble(const std::vector<BYTE>& code) {
    ComPtr<ID3DBlob> text;
    if (FAILED(D3DDisassemble(code.data(), code.size(), 0, nullptr, &text))) return {};
    return std::string(static_cast<const char*>(text->GetBufferPointer()), text->GetBufferSize());
}
std::vector<uint32_t> programOf(const std::vector<BYTE>& container) {
    for (const auto& chunk : edvr::dxbc_container::parseContainer(container.data(), container.size(), 0x00010050u))
        if (chunk.tag == 0x58454853u || chunk.tag == 0x52444853u) {
            std::vector<uint32_t> words(chunk.bytes.size() / 4);
            std::memcpy(words.data(), chunk.bytes.data(), words.size() * 4);
            return words;
        }
    return {};
}
std::string reasonOf(const std::vector<BYTE>& vs) {
    std::string why;
    if (edvr::engineVelocitySkinCapable(vs.data(), vs.size(), why)) return "";
    return why.empty() ? "?" : why;
}

// ---- K1 ------------------------------------------------------------------------------------------------------------------
void caseTokens() {
    using namespace edvr::dxbc_skin_detail;
    {
        const std::vector<uint32_t> t = {0x00100012u, 7u, 0xAAu};     // r7.x, then a stray word
        check(operandLength(t, 0, 2) == 2, "K1.a a temporary is a token and an index");
    }
    {
        const std::vector<uint32_t> t = {0x00004002u, 1u, 2u, 3u, 4u};
        check(operandLength(t, 0, 5) == 5, "K1.b a four-component immediate is a token and four words");
    }
    {
        const std::vector<uint32_t> t = {0x00004001u, 9u};
        check(operandLength(t, 0, 2) == 2, "K1.c a one-component immediate is a token and a word");
    }
    {
        const std::vector<uint32_t> t = {0x00208E46u, 1u, 275u};      // cb1[275].xyzw: two index words
        check(operandLength(t, 0, 3) == 3, "K1.d a constant buffer slot and row are two index words");
    }
    {
        const std::vector<uint32_t> t = {0x80100E46u, 0x41u, 5u};     // -r5.xyzw: an extended token between
        check(operandLength(t, 0, 3) == 3 && indexWordAt(t, 0) == 2, "K1.e a negated operand has an extended token before its index");
    }
    {
        const std::vector<uint32_t> t = {0x00D0E946u, 1u, 3u, 0u};    // index representation 2 in the second dimension: relative
        bool refused = false;
        try { operandLength(t, 0, 4); } catch (const std::exception&) { refused = true; }
        check(refused, "K1.f a computed index is refused");
    }
    {
        const std::vector<uint32_t> t = {0x00004001u};
        bool refused = false;
        try { operandLength(t, 0, 1); } catch (const std::exception&) { refused = true; }
        check(refused, "K1.g an operand that runs past its instruction is refused");
    }
    {
        // add r1.xyz, r2.xyzw, -r3.xyzw  = opcode token, three operands (the last with an extended token)
        const std::vector<uint32_t> t = {0x08000000u, 0x00100072u, 1u, 0x00100E46u, 2u, 0x80100E46u, 0x41u, 3u};
        const Instr in = parseInstr(t, 0);
        check(in.opcode == 0 && in.length == 8 && in.operand.size() == 3 && in.operand[0] == 1 && in.operand[1] == 3 && in.operand[2] == 5 && in.operandLen[2] == 3,
              "K1.h an instruction splits into the operands the walker expects, a negated one included");
    }
    {
        check(shapeOf(kLdStructured) && shapeOf(kLdStructured)->operands == 4 && shapeOf(kMovc) && shapeOf(kMovc)->operands == 4 && !shapeOf(75), "K1.i the opcodes the clone accepts, and sqrt (75) not among them");
        unsigned known = 0;
        for (const auto& s : kShapes) known += s.opcode < 256 ? 1u : 0u;
        check(known == 28, "K1.j there are 28 accepted opcodes");
    }
    {
        const auto vs = compile(skin_clone_synthetic::vertexSource(false), "vs_5_0");
        const auto words = programOf(vs);
        Plan plan;
        bool analysed = true;
        try { plan = analyse(words); } catch (const std::exception&) { analysed = false; }
        check(analysed, "K1.k the plain shape is analysed without a refusal");
        check(analysed && plan.temps > 0 && plan.baseLoadAt != plan.posLoadAt && plan.anchorEnd > plan.posLoadAt && plan.firstExec < plan.baseLoadAt && plan.firstDclInput && plan.lastOutputEnd,
              "K1.k the analysis finds the declarations, the two pose loads and an anchor after them");
        check(analysed && plan.comp[0] == 0 && plan.comp[1] == 1 && plan.comp[2] == 2, "K1.l the position components are named from the matrix multiplies (x, y, z)");
        check(isDeclaration(104) && isDeclaration(162) && !isDeclaration(0) && !isDeclaration(167), "K1.m declarations are told from instructions");
    }
}

// ---- K2 ------------------------------------------------------------------------------------------------------------------
void caseStructure() {
    const auto vs = compile(skin_clone_synthetic::vertexSource(true), "vs_5_0");
    const auto ps = compile(skin_clone_synthetic::pixelSource(), "ps_5_0");
    edvr::EngineVelocityInputs in;
    std::string why;
    check(edvr::engineVelocityDeriveInputs(vs.data(), vs.size(), in, why), "K2.a the pair derives");
    check(in.skinRegister == 3 && !in.slotFromVsPatch && in.identityRegister == 0, "K2.b the skin register is the one after the last output; the game's own identity output is kept");
    std::vector<BYTE> patched;
    const bool patchedOk = edvr::engineVelocityPatchVsSkin(vs.data(), vs.size(), in.skinRegister, patched, why);
    check(patchedOk, ("K2.c the vertex shader is patched" + (patchedOk ? std::string() : " -- declined: " + why)).c_str());
    if (!patchedOk) return;   // nothing below can be said of a shader that was not patched
    const std::string before = disassemble(vs), after = disassemble(patched);
    check(has(after, "dcl_resource_structured t108, 48") && has(after, "dcl_resource_structured t109, 4") && has(after, "dcl_resource_structured t110, 32"),
          "K2.d the three resources are declared with their strides");
    check(has(after, "dcl_output o3.xyzw") && has(after, "mov o3.xyzw"), "K2.e the new output is declared and written");
    check(!has(before, "t108") && !has(before, "dcl_output o3.xyzw"), "K2.f (the original had none of it)");
    const auto original = programOf(vs), changed = programOf(patched);
    const edvr::dxbc_skin_detail::Plan plan = analyse(original);
    // temps
    uint32_t temps = 0, newTemps = 0;
    for (size_t at = 2; at < original.size();) {
        if ((original[at] & 0x7ff) == 104) temps = original[at + 1];
        at += edvr::dxbc_container::instructionLength(original, at);
    }
    for (size_t at = 2; at < changed.size();) {
        if ((changed[at] & 0x7ff) == 104) newTemps = changed[at + 1];
        at += edvr::dxbc_container::instructionLength(changed, at);
    }
    check(temps == plan.temps && newTemps == 2 * temps + 2, "K2.g the temporary count doubles, plus the scratch pair");
    // the original instructions up to the anchor survive token for token
    size_t newExec = 0;
    for (size_t at = 2; at < changed.size();) {
        if (!edvr::dxbc_skin_detail::isDeclaration(changed[at] & 0x7ff)) { newExec = at; break; }
        at += edvr::dxbc_container::instructionLength(changed, at);
    }
    const size_t prefix = plan.anchorEnd - plan.firstExec;
    check(newExec && std::equal(original.begin() + plan.firstExec, original.begin() + plan.anchorEnd, changed.begin() + newExec), "K2.h the program up to and including the anchor is token for token the original");
    size_t rets = 0;
    for (size_t at = newExec; at < changed.size(); at += edvr::dxbc_container::instructionLength(changed, at))
        rets += (changed[at] & 0x7ff) == 62 ? 1u : 0u;
    check(rets == 1, "K2.i there is still exactly one return");
    check(changed.size() > original.size() + prefix / 2, "K2.j the clone is there (the program grew by about the cloned range)");
    // every instruction of the original after the anchor survives, in order, with only the export inserted
    const size_t tailOriginal = original.size() - plan.anchorEnd;
    check(changed.size() >= tailOriginal + 6 && std::equal(original.end() - tailOriginal, original.end() - 1, changed.end() - 1 - 5 - (tailOriginal - 1)),
          "K2.k every instruction after the anchor survives in order, the export inserted before the return");
    const size_t retAt = changed.size() - 1;
    check((changed[retAt] & 0x7ff) == 62 && changed[retAt - 5] == 0x05000036u, "K2.l the export sits immediately before the return");
    // the cloned pose loads read the previous pose table: t110, and the load's own stride token says 32 bytes
    {
        using namespace edvr::dxbc_skin_detail;
        unsigned poseLoads = 0, strideOk = 0;
        for (size_t at = newExec; at < changed.size(); at += edvr::dxbc_container::instructionLength(changed, at)) {
            if ((changed[at] & 0x7ff) != kLdStructured) continue;
            const Instr load = parseInstr(changed, at);
            if (changed[indexWordAt(changed, load.operand[3])] != edvr::kSkinPoseSlot) continue;
            ++poseLoads;
            uint32_t bytes = 0;
            size_t pos = at + 1;
            for (bool more = (changed[at] >> 31) != 0; more; ++pos) {
                if ((changed[pos] & 0x3Fu) == 2u) bytes = (changed[pos] >> 11) & 0xFFFu;
                more = (changed[pos] >> 31) != 0;
            }
            strideOk += bytes == edvr::kSkinPoseStrideBytes ? 1u : 0u;
        }
        check(poseLoads == 2 && strideOk == 2, "K2.z the two cloned pose loads read t110 and name its 32-byte stride");
    }
    // an output the game writes inside the cloned range is not written again by the clone: the patched program has that write and the export, no more
    {
        using namespace edvr::dxbc_skin_detail;
        const auto outputWrites = [](const std::vector<uint32_t>& t) {
            unsigned n = 0;
            for (size_t at = 2; at < t.size(); at += edvr::dxbc_container::instructionLength(t, at)) {
                const uint32_t op = t[at] & 0x7ffu;
                if (isDeclaration(op) || op == 62) continue;
                const Instr in = parseInstr(t, at);
                const OpcodeShape* shape = shapeOf(in.opcode);
                if (shape && shape->dst && typeOf(t[in.operand[0]]) == kTypeOutput) ++n;
            }
            return n;
        };
        auto inserted = original;
        const Plan insertedPlan = analyse(inserted);
        const uint32_t write[5] = {0x05000036u, 0x001020F2u, 1u, 0x00100E46u, 0u};   // mov o1.xyzw, r0.xyzw, ahead of the anchor
        inserted.insert(inserted.begin() + insertedPlan.firstExec, write, write + 5);
        inserted[1] += 5;
        const unsigned original1 = outputWrites(inserted);
        unsigned patchedWrites = 0;
        bool patchedOk = true;
        try { patchedWrites = outputWrites(patchProgram(inserted, 3)); } catch (const std::exception&) { patchedOk = false; }
        check(patchedOk && patchedWrites == original1 + 1, "K2.zz an output written inside the cloned range is not written again by the clone (the export is the only new write)");
    }
    // signatures
    bool osgn = false;
    for (const auto& chunk : edvr::dxbc_container::parseContainer(patched.data(), patched.size(), 0x00010050u))
        if (chunk.tag == 0x4e47534fu) {
            for (const auto& e : edvr::dxbc_container::parseSignature(chunk.bytes))
                if (edvr::dxbc_container::equalName(e.name, "EDVRSKINPREV")) osgn = e.registerIndex == 3 && e.componentType == 3 && (e.masks & 15u) == 15u;
        }
    check(osgn, "K2.m the output signature names EDVRSKINPREV: register 3, float, xyzw");
    // the pixel half
    in.skinExport = true;
    std::vector<BYTE> psPatched;
    check(edvr::engineVelocityPatchPs(ps.data(), ps.size(), in, psPatched, why), "K2.n the pixel shader is patched with the export");
    const std::string psText = disassemble(psPatched);
    check(has(psText, "dcl_input_ps linear v3.xyzw") && has(psText, "dcl_output o7.xyzw") && has(psText, "mov o7.xyzw, v3.xyzw") && has(psText, "dcl_output o6.xy"),
          "K2.o the pixel shader reads the interpolated E and writes it to target 7 beside the slot's target 6");
    bool target7 = false, input3 = false;
    for (const auto& chunk : edvr::dxbc_container::parseContainer(psPatched.data(), psPatched.size(), 0x00000050u)) {
        if (chunk.tag == 0x4e47534fu) for (const auto& e : edvr::dxbc_container::parseSignature(chunk.bytes)) target7 = target7 || (edvr::dxbc_container::equalName(e.name, "SV_TARGET") && e.registerIndex == 7);
        if (chunk.tag == 0x4e475349u) for (const auto& e : edvr::dxbc_container::parseSignature(chunk.bytes)) input3 = input3 || (edvr::dxbc_container::equalName(e.name, edvr::kSkinSemantic) && e.registerIndex == 3 && e.masks == 0x0F0Fu);
    }
    check(target7 && input3, "K2.p the pixel shader's signatures carry target 7 and the E input");
    in.skinExport = false;
    std::vector<BYTE> plain;
    check(edvr::engineVelocityPatchPs(ps.data(), ps.size(), in, plain, why) && !has(disassemble(plain), "o7"), "K2.q without the export the pixel patch is what it was: no target 7");
    in.skinExport = true;
    edvr::EngineVelocityInputs bad = in;
    bad.skinRegister = in.identityRegister;
    std::vector<BYTE> none;
    check(!edvr::engineVelocityPatchPs(ps.data(), ps.size(), bad, none, why), "K2.r the export refuses the identity register");
    bad = in;
    bad.skinRegister = 40;
    check(!edvr::engineVelocityPatchPs(ps.data(), ps.size(), bad, none, why), "K2.s the export refuses a register beyond 31");
    check(!edvr::engineVelocityPatchPs(ps.data(), ps.size(), in, none, why, true), "K2.t the export refuses the overlay depth guard");
    check(!edvr::engineVelocityPatchPs(ps.data(), ps.size(), in, none, why, false, edvr::dxbc_engine_velocity_detail::FlatMarkerKind::World), "K2.u the export refuses the flat marker kinds");
    // the patched vertex shader refuses a second patch
    check(!edvr::engineVelocityPatchVsSkin(patched.data(), patched.size(), 4, none, why) && has(why, "slot"), "K2.v a patched shader is not patched again");
    // an occupied output register
    check(!edvr::engineVelocityPatchVsSkin(vs.data(), vs.size(), 2, none, why) && has(why, "occupied"), "K2.w an output register that is taken is refused");
    check(!edvr::engineVelocityPatchVsSkin(vs.data(), vs.size(), 32, none, why), "K2.x a register beyond 31 is refused");
    std::vector<BYTE> broken = vs;
    broken[broken.size() / 2] ^= 0x5A;
    check(!edvr::engineVelocityPatchVsSkin(broken.data(), broken.size(), 3, none, why) && !edvr::engineVelocitySkinCapable(broken.data(), broken.size(), why), "K2.y a container with a bad checksum is refused by both entry points");
}

// ---- K3 ------------------------------------------------------------------------------------------------------------------
void caseDeclines() {
    using skin_clone_synthetic::Variant;
    const auto reason = [&](const Variant& v) { return reasonOf(compile(skin_clone_synthetic::vertexSource(v), "vs_5_0")); };
    check(reason(Variant{}).empty(), "K3.a the plain shape is accepted");
    {
        Variant v; v.noPose = true;
        const std::string r = reason(v);
        check(!r.empty() && has(r, "pose loads"), "K3.b a position that does not come from the record's offset 16 is declined");
    }
    {
        Variant v; v.sqrtInChain = true;
        const std::string r = reason(v);
        check(!r.empty() && has(r, "opcode"), "K3.c an opcode the clone does not know inside the range is declined, named by number");
    }
    {
        Variant v; v.relativeCb = true;
        const std::string r = reason(v);
        check(!r.empty() && has(r, "relative"), "K3.d a computed constant buffer index inside the range is declined");
    }
    {
        Variant v; v.occupied = true;
        const std::string r = reason(v);
        check(!r.empty() && has(r, "occupied"), "K3.e a shader that already uses one of the three slots is declined");
    }
    {
        Variant v; v.noMatrix = true;
        const std::string r = reason(v);
        check(!r.empty() && has(r, "matrix"), "K3.f a shader with no multiplies against cb1[270..272] is declined");
    }
    {
        // token surgery on the plain shape
        const auto vs = compile(skin_clone_synthetic::vertexSource(false), "vs_5_0");
        const auto words = programOf(vs);
        auto twice = words;
        twice.insert(twice.end() - 1, 0x01000000u | 62u);          // a second return before the last
        twice[1] = uint32_t(twice.size());
        bool refused = false;
        try { analyse(twice); } catch (const std::exception& e) { refused = has(e.what(), "return"); }
        check(refused, "K3.g a program with two returns is declined");
        auto unbalanced = words;
        size_t endif = 0;
        for (size_t at = 2; at < unbalanced.size(); at += edvr::dxbc_container::instructionLength(unbalanced, at))
            if ((unbalanced[at] & 0x7ff) == edvr::dxbc_skin_detail::kEndif) { endif = at; break; }
        check(endif != 0, "K3.h (the plain shape has an endif to remove)");
        if (endif) {
            unbalanced.erase(unbalanced.begin() + endif);
            unbalanced[1] = uint32_t(unbalanced.size());
            refused = false;
            try { analyse(unbalanced); } catch (const std::exception& e) { refused = has(e.what(), "unbalanced"); }
            check(refused, "K3.i unbalanced control flow is declined");
        }
        // a program whose t33 has the wrong stride
        auto stride = words;
        for (size_t at = 2; at < stride.size(); at += edvr::dxbc_container::instructionLength(stride, at))
            if ((stride[at] & 0x7ff) == 162 && stride[at + 2] == 33) stride[at + 3] = 320;
        refused = false;
        try { analyse(stride); } catch (const std::exception& e) { refused = has(e.what(), "t33"); }
        check(refused, "K3.j a t33 that is not the 336-byte pool is declined");
    }
    {
        // the derive step reports no skin register for a shader that cannot take the second skin
        Variant noPose;
        noPose.noPose = true;
        const auto vs = compile(skin_clone_synthetic::vertexSource(noPose), "vs_5_0");
        edvr::EngineVelocityInputs in;
        std::string why;
        const bool derived = edvr::engineVelocityDeriveInputs(vs.data(), vs.size(), in, why);
        check(!derived || in.skinRegister >= 32, "K3.k a shader that cannot take the second skin gets no skin register");
    }
}

// ---- K4, K5 --------------------------------------------------------------------------------------------------------------
void caseProperties(ID3D11Device* dev, ID3D11DeviceContext* ctx) {
    const auto ps = compile(skin_clone_synthetic::pixelSource(), "ps_5_0");
    for (int displacement = 0; displacement < 2; ++displacement) {
        const auto vs = compile(skin_clone_synthetic::vertexSource(displacement != 0), "vs_5_0");
        const unsigned examined = skin_clone_tests::run(dev, ctx, vs, ps, displacement ? "synthetic+displacement" : "synthetic", &check, nullptr, displacement ? "K5" : "K4");
        check(examined > 1000, displacement ? "K5.z the properties were exercised on covered pixels" : "K4.z the properties were exercised on covered pixels");
    }
}

// ---- K9 ------------------------------------------------------------------------------------------------------------------
std::vector<BYTE> readFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return {};
    const auto n = f.tellg();
    std::vector<BYTE> b(static_cast<size_t>(n));
    f.seekg(0);
    f.read(reinterpret_cast<char*>(b.data()), n);
    return b;
}
void caseCorpus(ID3D11Device* dev, ID3D11DeviceContext* ctx, const std::string& dir) {
    struct Pair { const char* vs; const char* ps; std::vector<std::pair<unsigned, float>> material; bool useMaterial; };
    const Pair pairs[] = {
        {"vs_D99AFDC250D19A3F", "ps_E86271E464CCDC1D", {}, false},
        {"vs_61AE8EB05FDC18DD", "ps_451A82D4DD1BA254", {}, false},
        {"vs_114AF608F86D9ED8", "ps_A17504A2627767F2", {{23, 1000.0f}}, true},   // cb2[5].w: the discard threshold
        {"vs_7B0DC42D383F694C", "ps_0DF03E64DF9DBEF1", {{23, 1000.0f}}, true},
        {"vs_8B589D25B2A0ADDC", "ps_7268762D11A610F2", {}, true},                // an all-zero b2 turns the alpha test off
    };
    unsigned present = 0;
    for (const Pair& p : pairs) {
        const auto vs = readFile(dir + "\\shaders\\" + p.vs + ".dxbc"), ps = readFile(dir + "\\shaders\\" + p.ps + ".dxbc");
        if (vs.empty() || ps.empty()) {
            std::printf("  corpus: %s + %s absent from this dump\n", p.vs, p.ps);
            continue;
        }
        ++present;
        const std::string name = std::string(p.vs) + "+" + p.ps;
        skin_clone_tests::run(dev, ctx, vs, ps, name.c_str(), &check, p.useMaterial ? &p.material : nullptr, "K9");
    }
    check(present > 0, "K9.a the dump holds at least one of the game's skinned pairs");
}
}  // namespace

int main(int argc, char** argv) {
    bool selfTest = false;
    std::string corpus;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--self-test" || a == "--dry-run") selfTest = true;
        else if (a == "--corpus" && i + 1 < argc) corpus = argv[++i];
        else if (i == 2 && argv[1] == std::string("--self-test")) continue;
        else {
            std::fprintf(stderr, "usage: skin_clone_test --self-test [repository root] | --dry-run | --corpus <edvr_logs dir>\n");
            return 2;
        }
    }
    if (!selfTest && corpus.empty()) {
        std::fprintf(stderr, "usage: skin_clone_test --self-test [repository root] | --dry-run | --corpus <edvr_logs dir>\n");
        return 2;
    }
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    D3D_FEATURE_LEVEL level{};
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev, &level, &ctx))) {
        std::printf("FAIL: K4.device -- D3D11CreateDevice WARP\n");
        return 1;
    }
    if (selfTest) {
        caseTokens();
        caseStructure();
        caseDeclines();
        caseProperties(dev.Get(), ctx.Get());
    }
    if (!corpus.empty()) caseCorpus(dev.Get(), ctx.Get(), corpus);
    if (g_failures) {
        std::printf("FAIL: skin clone: %u of %u checks failed\n", g_failures, g_checks);
        return 1;
    }
    std::printf("PASS: %u skin clone checks\n", g_checks);
    return 0;
}
