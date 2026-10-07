// fix.ui_quality's orbit lines (src/d3d11/orbital_width.h, orbital_width_patch.h; docs/ui-layer-2026-09-23.md, "2026-10-06:
// the orbit lines"). The game's own vertex shader (vs C7FA0C0F5DD49180, saved from the Frontier install, 2412 bytes) and the
// production copy of it with the half-width scaled run on WARP over a captured draw's constants and vertex streams, and the
// strip they draw is measured:
//
//   * at f = 1 the patched shader's every output, stream-out byte for byte, is the game's;
//   * at f = 0.5 and 0.4 (and 0.7 and the 0.25 cap) each instance's strip is 2 x w x f pixels wide -- the orange orbit's
//     w = 2.0 gives 4.000 / 2.000 / 1.600, the cyan rings' w = 1.5 give 3.000 / 1.500 / 1.200 -- measured as the distance
//     between a pair's two edge vertices and as the width perpendicular to the line (the centre line's own tangent);
//   * only SV_POSITION's x and y move: the colour, the signed distance, z and w are the game's bytes at every f;
//   * the coverage twin (stellar_coverage.h, the transcription ui_depth.cpp binds after the game's draw), compiled from the
//     production text and fed the production constant buffer, lays its strip on the patched game's within 0.03 px at every f
//     (and on the original's at f = 1, the criterion tools/stellar_motion_test holds the transcription to);
//   * the mutants -- the factor not applied, applied twice, the twin left at 1 or scaled twice, a constant 1 ulp off at f = 1 --
//     are each caught by the very comparator that passes the real thing;
//
// and the production objects themselves run on the same device: the DXBC edit and every refusal it makes (the b13 already
// declared, an instruction other than the one written for, a changed byte, a linked creation), the cache and its identity tag,
// the binding that swaps the shader and slot 13 for one draw and puts the game's back (and the restore that fails, the settle
// at the boundary, the slot the game rebound), the constant buffer, the half-width read-back, and the frame boundary's one
// decision. --wiring scans the draw hook's source (ui_depth, vscreen, device_hook, ui_layer, build.bat) with controls that
// edit a copy and must trip the pin.
//
// FIXTURES (tools/orbital_width_test/fixtures; game assets, kept as the other rigs keep theirs):
//   vs_C7FA0C0F5DD49180.dxbc       edvr_logs/shaders of the Frontier install, FNV-1a 64 C7FA0C0F5DD49180
//   draw_180540_23650.bin          the draw of frame 23650, ordinal 346 of the 2026-10-06 18:05:40 eye run (the file layout is
//                                  in the scratch script that made it, summarised at loadDraw): VS b1 rows 0..332, vertex
//                                  buffer 0 (8194 x float4), vertex buffer 1 (5 instances x 15 floats), the 2016x1949 target.
//                                  Instance 0 is the orange orbit (w 2.0, 12 pairs on screen), 1 a ring off screen (w 1.5),
//                                  2..4 rings (w 1.5, about 2000 pairs on screen each).
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

#include "../../src/d3d11/orbital_width_patch.h"
#include "../../src/d3d11/stellar_coverage.h"
#include "../../src/common/log.h"
#include "../../src/common/system_d3d11.h"

using Microsoft::WRL::ComPtr;
using namespace edvr::orbital_width;

namespace edvr {
Log& Log::get() {
    static Log log;
    return log;
}
Log::~Log() {}
void Log::note(const char*, ...) {}
void breadcrumb(const char*) {}
}  // namespace edvr

static unsigned checks = 0;
static void check(bool ok, const char* why) {
    ++checks;
    if (!ok) throw std::runtime_error(why);
}
static void ck(HRESULT h, const char* what = "D3D call") {
    if (FAILED(h)) {
        std::printf("HRESULT=%08lx at %s\n", static_cast<unsigned long>(h), what);
        throw std::runtime_error(what);
    }
}

static std::vector<BYTE> readBytes(const char* path) {
    std::ifstream f(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

// ---------------------------------------------------------------- the captured draw

struct Draw {
    uint32_t vertices = 0, instances = 0, floatsPerInstance = 0, width = 0, height = 0;
    std::vector<float> cb1, vb0, vb1;
};

// EDVROW01, then u32 version(1), cb1 rows (333), vertices (8194), instances (5), floats per instance (15), target width,
// target height; then cb1 (rows x 4 floats), vb0 (vertices x 4 floats), vb1 (instances x 15 floats).
static Draw loadDraw(const char* path) {
    const auto bytes = readBytes(path);
    check(bytes.size() > 8 + 7 * 4 && !std::memcmp(bytes.data(), "EDVROW01", 8), "the draw fixture is readable and EDVROW01");
    uint32_t h[7];
    std::memcpy(h, bytes.data() + 8, sizeof(h));
    check(h[0] == 1 && h[1] == 333 && h[2] == 8194 && h[3] == 5 && h[4] == 15, "the draw fixture's header is the captured draw's shape");
    Draw d;
    d.vertices = h[2];
    d.instances = h[3];
    d.floatsPerInstance = h[4];
    d.width = h[5];
    d.height = h[6];
    const size_t floats = size_t(h[1]) * 4 + size_t(h[2]) * 4 + size_t(h[3]) * h[4];
    check(bytes.size() == 8 + sizeof(h) + floats * 4, "the draw fixture is complete");
    const float* p = reinterpret_cast<const float*>(bytes.data() + 8 + sizeof(h));
    d.cb1.assign(p, p + size_t(h[1]) * 4);
    p += size_t(h[1]) * 4;
    d.vb0.assign(p, p + size_t(h[2]) * 4);
    p += size_t(h[2]) * 4;
    d.vb1.assign(p, p + size_t(h[3]) * h[4]);
    return d;
}

// ---------------------------------------------------------------- the device and the runs

static ComPtr<ID3D11Device> g_dev;
static ComPtr<ID3D11DeviceContext> g_ctx;
static ComPtr<ID3D11InfoQueue> g_messages;

static ComPtr<ID3D11Buffer> makeBuffer(UINT bytes, UINT bind, const void* data, D3D11_USAGE usage = D3D11_USAGE_DEFAULT,
                                       UINT cpu = 0) {
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = bytes;
    bd.BindFlags = bind;
    bd.Usage = usage;
    bd.CPUAccessFlags = cpu;
    const D3D11_SUBRESOURCE_DATA init{data, 0, 0};
    ComPtr<ID3D11Buffer> b;
    ck(g_dev->CreateBuffer(&bd, data ? &init : nullptr, &b), "CreateBuffer");
    return b;
}

static std::vector<float> readBuffer(ID3D11Buffer* source) {
    D3D11_BUFFER_DESC bd{};
    source->GetDesc(&bd);
    bd.BindFlags = bd.MiscFlags = bd.StructureByteStride = 0;
    bd.Usage = D3D11_USAGE_STAGING;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Buffer> stage;
    ck(g_dev->CreateBuffer(&bd, nullptr, &stage), "staging");
    g_ctx->CopyResource(stage.Get(), source);
    D3D11_MAPPED_SUBRESOURCE m{};
    ck(g_ctx->Map(stage.Get(), 0, D3D11_MAP_READ, 0, &m), "Map");
    std::vector<float> v(bd.ByteWidth / 4);
    std::memcpy(v.data(), m.pData, bd.ByteWidth);
    g_ctx->Unmap(stage.Get(), 0);
    return v;
}

static ComPtr<ID3DBlob> compileHlsl(const char* source, const char* profile) {
    ComPtr<ID3DBlob> code, error;
    const HRESULT h = D3DCompile(source, std::strlen(source), "orbital coverage", nullptr, nullptr, "main", profile,
                                 D3DCOMPILE_ENABLE_STRICTNESS, 0, &code, &error);
    if (FAILED(h) && error) std::puts(static_cast<const char*>(error->GetBufferPointer()));
    ck(h, "D3DCompile");
    return code;
}

// What the stream-out stage hands back for each vertex: the colour (4), the signed distance (1) and SV_POSITION (4).
constexpr size_t kOut = 9;
struct Out {
    std::vector<float> v;
    unsigned vertices = 0;
    const float* at(unsigned instance, unsigned vertex) const { return v.data() + (size_t(instance) * vertices + vertex) * kOut; }
};

struct Runner {
    const Draw& d;
    ComPtr<ID3D11Buffer> cb1, vb0, vb1, out, motion;
    ComPtr<ID3D11InputLayout> layout;
    explicit Runner(const Draw& draw, const std::vector<BYTE>& originalVs) : d(draw) {
        cb1 = makeBuffer(UINT(d.cb1.size() * 4), D3D11_BIND_CONSTANT_BUFFER, d.cb1.data());
        vb0 = makeBuffer(UINT(d.vb0.size() * 4), D3D11_BIND_VERTEX_BUFFER, d.vb0.data());
        vb1 = makeBuffer(UINT(d.vb1.size() * 4), D3D11_BIND_VERTEX_BUFFER, d.vb1.data());
        out = makeBuffer(UINT(d.vertices * d.instances * kOut * 4), D3D11_BIND_STREAM_OUTPUT, nullptr);
        const UINT zero[4] = {0, 0, 2, d.instances};
        motion = makeBuffer(16, D3D11_BIND_CONSTANT_BUFFER, zero);
        const D3D11_INPUT_ELEMENT_DESC elements[] = {
            {"POSTANGENT", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"OSTOWST", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 0, D3D11_INPUT_PER_INSTANCE_DATA, 1},
            {"OSTOWSR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 16, D3D11_INPUT_PER_INSTANCE_DATA, 1},
            {"OSTOWSS", 0, DXGI_FORMAT_R32G32B32_FLOAT, 1, 32, D3D11_INPUT_PER_INSTANCE_DATA, 1},
            {"COLOUR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 44, D3D11_INPUT_PER_INSTANCE_DATA, 1}};
        ck(g_dev->CreateInputLayout(elements, 5, originalVs.data(), originalVs.size(), &layout), "input layout");
    }
    static ComPtr<ID3D11GeometryShader> streamOut(const void* bytes, size_t n) {
        const D3D11_SO_DECLARATION_ENTRY decl[] = {{0, "__USER_STELLARVERTEX_COLOUR", 0, 0, 4, 0},
                                                   {0, "__USER_STELLARVERTEX_STABLESIGNEDUNITDISTANCEPERSPECTIVE", 0, 0, 1, 0},
                                                   {0, "SV_POSITION", 0, 0, 4, 0}};
        const UINT stride = UINT(kOut * 4);
        ComPtr<ID3D11GeometryShader> gs;
        ck(g_dev->CreateGeometryShaderWithStreamOutput(bytes, n, decl, 3, &stride, 1, D3D11_SO_NO_RASTERIZED_STREAM, nullptr, &gs),
           "stream-out shader");
        return gs;
    }
    // Everything but the vertex shader and b13: input assembler, VS b1 and b12, the stream-out stage.
    void setup(ID3D11GeometryShader* gs) const {
        g_ctx->ClearState();
        g_ctx->IASetInputLayout(layout.Get());
        g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
        ID3D11Buffer* vbs[] = {vb0.Get(), vb1.Get()};
        const UINT strides[] = {16, 60}, offsets[] = {0, 0};
        g_ctx->IASetVertexBuffers(0, 2, vbs, strides, offsets);
        g_ctx->VSSetConstantBuffers(1, 1, cb1.GetAddressOf());
        g_ctx->VSSetConstantBuffers(12, 1, motion.GetAddressOf());
        g_ctx->GSSetShader(gs, nullptr, 0);
        const UINT zero = 0;
        g_ctx->SOSetTargets(1, out.GetAddressOf(), &zero);
    }
    Out issue() const {
        g_ctx->DrawInstanced(d.vertices, d.instances, 0, 0);
        g_ctx->SOSetTargets(0, nullptr, nullptr);  // a buffer is not read back while it is still the stream-out target
        Out o;
        o.vertices = d.vertices;
        o.v = readBuffer(out.Get());
        return o;
    }
    Out run(ID3D11VertexShader* vs, ID3D11GeometryShader* gs, ID3D11Buffer* b13) const {
        setup(gs);
        g_ctx->VSSetShader(vs, nullptr, 0);
        if (b13) g_ctx->VSSetConstantBuffers(13, 1, &b13);
        return issue();
    }
};

static bool identical(const Out& a, const Out& b) {
    return a.v.size() == b.v.size() && !std::memcmp(a.v.data(), b.v.data(), a.v.size() * sizeof(float));
}

// ---------------------------------------------------------------- measuring the strip

struct Pixel {
    double x, y;
    bool front;
};
static Pixel toPixel(const float* o, const Draw& d) {
    const double w = o[8];
    return {(o[5] / w * 0.5 + 0.5) * d.width, (-o[6] / w * 0.5 + 0.5) * d.height, w > 0.0 && std::isfinite(w)};
}

struct Strip {
    std::vector<double> edge, perp;
};
// The strip's pairs (vertices 2k and 2k+1 share one clip position and sit on opposite sides of the line) with their
// neighbours in front of the camera and their centre on the target: the edge length between the pair, and the width
// perpendicular to the line, taken against the centre line's tangent from the neighbouring pairs.
static Strip measure(const Out& o, const Draw& d, unsigned instance) {
    Strip s;
    const unsigned pairs = d.vertices / 2;
    auto centre = [&](unsigned k, bool* front) {
        const Pixel a = toPixel(o.at(instance, 2 * k), d), b = toPixel(o.at(instance, 2 * k + 1), d);
        *front = a.front && b.front && std::isfinite(a.x + a.y + b.x + b.y);
        return Pixel{(a.x + b.x) / 2, (a.y + b.y) / 2, *front};
    };
    for (unsigned k = 1; k + 1 < pairs; ++k) {
        bool f0, f1, f2;
        const Pixel c0 = centre(k - 1, &f0), c1 = centre(k, &f1), c2 = centre(k + 1, &f2);
        if (!f0 || !f1 || !f2) continue;
        if (!(c1.x >= 0 && c1.x <= d.width && c1.y >= 0 && c1.y <= d.height)) continue;
        double tx = c2.x - c0.x, ty = c2.y - c0.y;
        const double tl = std::hypot(tx, ty);
        if (tl < 1e-6) continue;
        tx /= tl;
        ty /= tl;
        const Pixel a = toPixel(o.at(instance, 2 * k), d), b = toPixel(o.at(instance, 2 * k + 1), d);
        const double ex = a.x - b.x, ey = a.y - b.y;
        s.edge.push_back(std::hypot(ex, ey));
        s.perp.push_back(std::fabs(ex * ty - ey * tx));
    }
    return s;
}

static double lowest(const std::vector<double>& v) { return *std::min_element(v.begin(), v.end()); }
static double highest(const std::vector<double>& v) { return *std::max_element(v.begin(), v.end()); }

// The comparator the real thing passes and the mutants must fail: every pair of the instance is `expected` pixels wide,
// by the edge length to 0.002 px and perpendicular to the line to 0.01 px, and there are enough pairs to mean something.
static bool widthIs(const Strip& s, double expected, unsigned atLeast) {
    if (s.edge.size() < atLeast) return false;
    return lowest(s.edge) >= expected - 0.002 && highest(s.edge) <= expected + 0.002 && lowest(s.perp) >= expected - 0.01 &&
           highest(s.perp) <= expected + 0.01;
}

// Where the vertices that matter sit (in front of the camera, near the target): the largest distance in pixels between
// two runs' positions for the same vertex.
static double footprintError(const Out& a, const Out& b, const Draw& d, unsigned* compared = nullptr) {
    double worst = 0;
    unsigned n = 0;
    for (unsigned i = 0; i < d.instances; ++i)
        for (unsigned v = 0; v < d.vertices; ++v) {
            const Pixel pa = toPixel(a.at(i, v), d), pb = toPixel(b.at(i, v), d);
            if (!pa.front || !pb.front || !(pa.x > -50 && pa.x < d.width + 50 && pa.y > -50 && pa.y < d.height + 50)) continue;
            worst = (std::max)(worst, std::hypot(pa.x - pb.x, pa.y - pb.y));
            ++n;
        }
    if (compared) *compared = n;
    return worst;
}

// The vertex outputs a width change must not touch: colour, signed distance, SV_POSITION z and w.
static bool sameBesidesXy(const Out& a, const Out& b) {
    if (a.v.size() != b.v.size()) return false;
    for (size_t i = 0; i < a.v.size(); i += kOut) {
        if (std::memcmp(&a.v[i], &b.v[i], 5 * sizeof(float)) || std::memcmp(&a.v[i + 7], &b.v[i + 7], 2 * sizeof(float))) return false;
    }
    return true;
}

// ---------------------------------------------------------------- the DXBC edit

static std::vector<uint32_t> programTokens(const std::vector<BYTE>& container) {
    auto chunks = edvr::dxbc_container::parseContainer(container.data(), container.size(), kProgramType);
    for (const auto& c : chunks)
        if (c.tag == 0x58454853u) {
            std::vector<uint32_t> t(c.bytes.size() / 4);
            std::memcpy(t.data(), c.bytes.data(), c.bytes.size());
            return t;
        }
    throw std::runtime_error("no program chunk");
}

static Why refusalOf(const std::vector<uint32_t>& tokens) {
    try {
        patchProgram(tokens);
        return Why::kNone;
    } catch (const Refusal& r) {
        return r.why;
    } catch (const std::exception&) {
        return Why::kBytes;  // a program that does not even walk: patch() answers the same
    }
}

static void editCases(const std::vector<BYTE>& game) {
    const auto tokens = programTokens(game);
    check(tokens.size() == 489 && tokens[0] == kProgramType && tokens[1] == 489, "the game's program is 489 tokens, vs_5_0");
    const auto edited = patchProgram(tokens);
    check(edited.size() == 491 && edited[1] == 491, "the edit adds one declaration (4 tokens) and removes two from the instruction");
    // The one instruction and the one declaration are the whole difference.
    size_t first = 0;
    while (first < tokens.size() && tokens[first] == edited[first]) ++first;
    check(first == 1, "the length word is the first token that differs");
    size_t same = 2;
    while (same < tokens.size() && tokens[same] == edited[same]) ++same;
    check(same == 7, "the program is the game's up to and including CB1's declaration (token 7 starts the added one)");
    check(!std::memcmp(&edited[7], kCb13Declaration, sizeof(kCb13Declaration)), "dcl_constantbuffer cb13[1] follows CB1's");
    size_t at = 0;
    for (size_t i = 0; i + 11 <= tokens.size(); ++i)
        if (!std::memcmp(&tokens[i], kTarget, sizeof(kTarget))) at = i;
    check(at == 451, "the target instruction is where the disassembly put it (token 451)");
    check(!std::memcmp(&edited[at + 4], kReplacement, sizeof(kReplacement)) && edited.size() - (at + 4 + 9) == tokens.size() - (at + 11),
          "the replacement sits where the target was, and every token after it is the game's");
    check(std::equal(tokens.begin() + at + 11, tokens.end(), edited.begin() + at + 4 + 9), "the tail is the game's tokens");
    check(std::equal(tokens.begin() + 7, tokens.begin() + at, edited.begin() + 11), "the middle is the game's tokens");

    // Refusals, each by its reason.
    auto mutated = [&](std::function<void(std::vector<uint32_t>&)> f) {
        auto t = tokens;
        f(t);
        return t;
    };
    check(refusalOf(tokens) == Why::kNone, "the game's program is accepted");
    check(refusalOf(mutated([&](auto& t) { t[at + 9] = t[at + 10] = 0x40400000; })) == Why::kPatch, "a literal of 3 for 2: the target is not in the program");
    check(refusalOf(mutated([&](auto& t) { t[at - 1] = 2; })) == Why::kPatch, "the instruction before the target changed");
    check(refusalOf(mutated([&](auto& t) { t[at + 11 + 3] = 0x00100AE6; })) == Why::kPatch, "the instruction after the target changed");
    check(refusalOf(mutated([&](auto& t) { t[0] = 0x50; })) == Why::kPatch, "a pixel program is refused");
    check(refusalOf(mutated([&](auto& t) { t.resize(t.size() - 3); t[1] = uint32_t(t.size()); })) == Why::kBytes, "a program cut inside an instruction does not walk: refused");
    check(refusalOf(mutated([&](auto& t) { t[5] = 2; })) == Why::kPatch, "CB1 declared as CB2 is not the program the edit was written for");
    check(refusalOf(mutated([&](auto& t) {
              t.insert(t.begin() + 7, kCb13Declaration, kCb13Declaration + 4);
              t[1] = uint32_t(t.size());
          })) == Why::kSlot, "a program that already declares constant buffer 13: slot in use");
    check(refusalOf(mutated([&](auto& t) {
              t.insert(t.begin() + 7, kTarget, kTarget + 11);
              t[1] = uint32_t(t.size());
          })) == Why::kPatch, "the target twice: refused, never edited twice");
    check(refusalOf(mutated([&](auto& t) {
              const uint32_t extra[4] = {0x04000059, 0x00208e46, 2, 7};
              t.insert(t.begin() + 7, extra, extra + 4);
              t[1] = uint32_t(t.size());
          })) == Why::kPatch, "a second constant buffer declaration: refused");

    // The container: the checksum is the game's, the edit's is fresh; wrong sizes and bytes are the bytecode's mismatch.
    std::vector<BYTE> out;
    const uint64_t hash = hashOf(game.data(), game.size());
    check(hash == kVs, "the fixture hashes to the game's shader (FNV-1a 64 C7FA0C0F5DD49180)");
    Result r = patch(game.data(), game.size(), hash, out);
    check(r.ok() && out.size() == game.size() + 8, "the container patches: one declaration added, two tokens fewer in the instruction, 8 bytes in all");
    auto reparsed = edvr::dxbc_container::parseContainer(out.data(), out.size(), kProgramType);
    check(reparsed.size() == 3, "the patched container still has the three chunks (ISGN, OSGN, SHEX)");
    check(patch(game.data(), game.size(), hash ^ 1, out).why == Why::kBytes, "another hash: bytecode mismatch");
    auto longer = game;
    longer.push_back(0);
    check(patch(longer.data(), longer.size(), hash, out).why == Why::kBytes, "another size: bytecode mismatch");
    auto flipped = game;
    flipped[flipped.size() / 2] ^= 1;
    check(patch(flipped.data(), flipped.size(), hash, out).why == Why::kBytes, "a changed byte: bytecode mismatch");
    check(patch(nullptr, 0, hash, out).why == Why::kBytes, "no bytes: bytecode mismatch");
}

// ---------------------------------------------------------------- the production objects on a real context

static bool boundIs(ID3D11VertexShader* vs, ID3D11Buffer* b13) {
    ComPtr<ID3D11VertexShader> v;
    ComPtr<ID3D11Buffer> b;
    g_ctx->VSGetShader(&v, nullptr, nullptr);
    g_ctx->VSGetConstantBuffers(kSlot, 1, &b);
    return v.Get() == vs && b.Get() == b13;
}
static ULONG refsOf(IUnknown* u) {
    u->AddRef();
    return u->Release();
}
static void failingSetVs(ID3D11DeviceContext*, ID3D11VertexShader*, ID3D11ClassInstance* const*, uint32_t) {
    RaiseException(0xe0421313, 0, 0, nullptr);
}

static void bindingCases(const std::vector<BYTE>& game, ID3D11VertexShader* original, ID3D11VertexShader* patched) {
    ComPtr<ID3D11VertexShader> other;
    {
        // Any other vertex shader: the twin's.
        const auto code = compileHlsl(edvr::kOrbitalCoverageVs, "vs_5_0");
        ck(g_dev->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &other), "another vertex shader");
    }
    const auto sentinelData = std::vector<float>{7, 8, 9, 10};
    auto sentinel = makeBuffer(16, D3D11_BIND_CONSTANT_BUFFER, sentinelData.data());
    auto constants = makeBuffer(16, D3D11_BIND_CONSTANT_BUFFER, sentinelData.data());
    auto gameState = [&](ID3D11VertexShader* vs, ID3D11Buffer* b) {
        g_ctx->VSSetShader(vs, nullptr, 0);
        g_ctx->VSSetConstantBuffers(kSlot, 1, &b);
    };
    const auto acceptAll = [](ID3D11VertexShader*) { return true; };
    const auto refuseAll = [](ID3D11VertexShader*) { return false; };
    g_ctx->ClearState();
    const ULONG vsRefs = refsOf(original), cbRefs = refsOf(sentinel.Get()), patchedRefs = refsOf(patched), constantsRefs = refsOf(constants.Get());
    auto quiet = [&] {
        return refsOf(original) == vsRefs && refsOf(sentinel.Get()) == cbRefs && refsOf(patched) == patchedRefs &&
               refsOf(constants.Get()) == constantsRefs;
    };
    {   // the ordinary draw: in, and out again
        gameState(original, sentinel.Get());
        Binding b;
        check(!b.needsRestore(), "a fresh binding owes nothing");
        check(b.begin(g_ctx.Get(), patched, constants.Get(), acceptAll), "begin binds the patched shader and the factor's buffer");
        check(b.needsRestore() && boundIs(patched, constants.Get()), "the patched shader and b13 are bound, the game's owed back");
        check(!b.begin(g_ctx.Get(), patched, constants.Get(), acceptAll), "a second begin while one is open is refused");
        check(b.restore(g_ctx.Get()) && !b.needsRestore(), "restore puts the game's back");
        check(boundIs(original, sentinel.Get()), "the game's shader and slot 13 are exactly as they were");
        check(quiet(), "no reference leaked or kept");
        check(b.restore(g_ctx.Get()), "restore with nothing owed is a no-op");
    }
    {   // a bound shader the accept test refuses is left alone
        gameState(other.Get(), sentinel.Get());
        Binding b;
        check(!b.begin(g_ctx.Get(), patched, constants.Get(), refuseAll), "a bound shader that is not the exact program is refused");
        check(!b.needsRestore() && boundIs(other.Get(), sentinel.Get()), "...and nothing of the game's state moved");
        check(!b.begin(g_ctx.Get(), nullptr, constants.Get(), acceptAll) && !b.begin(g_ctx.Get(), patched, nullptr, acceptAll) &&
                  !b.begin(nullptr, patched, constants.Get(), acceptAll),
              "no patched shader, no buffer or no context: refused");
        gameState(original, sentinel.Get());
        check(!b.begin(g_ctx.Get(), patched, constants.Get(), [&](ID3D11VertexShader* now) { return now == other.Get(); }) && quiet(),
              "the accept test sees the shader that is bound, and a refusal leaks nothing");
    }
    {   // a restore that fails twice: the references are kept, the boundary settles it
        gameState(original, sentinel.Get());
        Binding b;
        check(b.begin(g_ctx.Get(), patched, constants.Get(), acceptAll), "settle scene: bound");
        const auto result = b.finish(g_ctx.Get(), failingSetVs);
        check(result.retried && !result.restored && b.needsRestore(), "a restore that failed twice is retried once and still owed");
        ComPtr<ID3D11VertexShader> stuck;
        g_ctx->VSGetShader(&stuck, nullptr, nullptr);
        check(stuck.Get() == patched, "...and EDVR's shader is still bound until it is settled (the buffer slot, whose setter worked, is the game's again)");
        stuck.Reset();  // a reference of the test's own would read as a leak below
        check(!b.begin(g_ctx.Get(), patched, constants.Get(), acceptAll), "no new draw begins while one is owed");
        check(!b.settle(g_ctx.Get(), failingSetVs) && b.needsRestore(), "a settle whose setter faults changes nothing and is still owed");
        check(b.settle(g_ctx.Get()) && !b.needsRestore(), "the next settle puts the game's back");
        check(boundIs(original, sentinel.Get()), "settled: the game's shader and slot 13 are bound again");
        check(quiet(), "settled: no reference kept");
        check(b.settle(g_ctx.Get()), "a settled binding settles idempotently");
    }
    {   // the game rebound the slots meanwhile: its own state stays
        gameState(original, sentinel.Get());
        Binding b;
        check(b.begin(g_ctx.Get(), patched, constants.Get(), acceptAll), "rebound scene: bound");
        b.finish(g_ctx.Get(), failingSetVs);
        auto theirs = makeBuffer(16, D3D11_BIND_CONSTANT_BUFFER, sentinelData.data());
        gameState(other.Get(), theirs.Get());
        check(b.settle(g_ctx.Get()) && !b.needsRestore(), "settle with both slots rebound by the game succeeds");
        check(boundIs(other.Get(), theirs.Get()), "...and leaves the game's own newer state, not the saved one");
        gameState(original, sentinel.Get());
        check(quiet(), "no reference of the saved state is kept");
    }
    {   // a null context cannot settle; a clear lets go
        gameState(original, sentinel.Get());
        Binding b;
        check(b.begin(g_ctx.Get(), patched, constants.Get(), acceptAll), "null scene: bound");
        b.finish(g_ctx.Get(), failingSetVs);
        check(!b.settle(nullptr) && b.needsRestore(), "no context: the settle reports failure and keeps the state");
        b.settle(g_ctx.Get());
        check(quiet(), "settled");
    }
    (void)game;
    g_ctx->ClearState();
}

// ---------------------------------------------------------------- the constant buffer

static void constantsCases() {
    check(Constants::value(1.0) == 2.0f && Constants::value(0.5) == 1.0f && Constants::value(0.25) == 0.5f,
          "the buffer's x is exactly the game's literal 2 at f = 1, and 2f for the exactly representable factors");
    check(std::fabs(Constants::value(0.4) - 0.8f) < 1e-7f, "0.4 gives 0.8");
    Constants c;
    check(c.get(nullptr, 0.5) == nullptr && c.get(g_ctx.Get(), 0.0) == nullptr && c.get(g_ctx.Get(), -1.0) == nullptr &&
              c.get(g_ctx.Get(), std::nan("")) == nullptr,
          "no context, or a factor that is not positive and finite, gets no buffer");
    ID3D11Buffer* first = c.get(g_ctx.Get(), 1.0);
    check(first && c.buffer() == first && !c.failed(), "the first use makes the buffer");
    auto v = readBuffer(first);
    check(v.size() == 4 && v[0] == 2.0f && v[1] == 2.0f && v[2] == 2.0f && v[3] == 2.0f, "it holds 2 in every lane at f = 1");
    ID3D11Buffer* second = c.get(g_ctx.Get(), 0.5);
    check(second == first, "a new factor rewrites the same buffer");
    v = readBuffer(second);
    check(v[0] == 1.0f && v[3] == 1.0f, "and holds 2f = 1 at f = 0.5");
    check(c.get(g_ctx.Get(), 0.5) == first && readBuffer(first)[0] == 1.0f, "the same factor is a lookup");
    c.reset();
    check(c.buffer() == nullptr, "reset lets it go");
    // A destroyed device's buffer is never reused: a second device gets a buffer of its own.
    ComPtr<ID3D11Device> d2;
    ComPtr<ID3D11DeviceContext> c2;
    ck(edvr::systemD3D11CreateDevice()(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &d2, nullptr, &c2), "second device");
    Constants two;
    ID3D11Buffer* onFirst = two.get(g_ctx.Get(), 0.5);
    ID3D11Buffer* onSecond = two.get(c2.Get(), 0.5);
    ComPtr<ID3D11Device> owner;
    onSecond->GetDevice(&owner);
    check(onFirst && onSecond && owner.Get() == d2.Get(), "a context of another device gets a buffer made on that device");
}

// ---------------------------------------------------------------- the cache

static void cacheCases(const std::vector<BYTE>& game, ComPtr<ID3D11VertexShader>& original, ComPtr<ID3D11VertexShader>& patched) {
    ck(g_dev->CreateVertexShader(game.data(), game.size(), nullptr, &original), "the game's vertex shader");
    ComPtr<ID3D11VertexShader> untagged;
    ck(g_dev->CreateVertexShader(game.data(), game.size(), nullptr, &untagged), "a second creation of the same bytes");
    Cache cache;
    const uint64_t hash = hashOf(game.data(), game.size());
    check(!cache.remembered() && cache.shader() == nullptr && !cache.attempted(), "an empty cache has nothing");
    check(cache.remember(original.Get(), hash ^ 2, game.data(), game.size(), false).why == Why::kNotOurs, "another shader's hash is not ours (one compare)");
    check(cache.remember(original.Get(), hash, game.data(), game.size(), true).why == Why::kLinked, "a creation with class linkage is refused");
    auto corrupt = game;
    corrupt[100] ^= 0x40;
    check(cache.remember(original.Get(), hash, corrupt.data(), corrupt.size(), false).why == Why::kBytes, "bytes that are not the game's are refused");
    check(cache.remember(nullptr, hash, game.data(), game.size(), false).why == Why::kLinked && !cache.remembered(), "nothing refused was remembered");
    check(cache.prepare(g_ctx.Get(), nullptr) == nullptr, "nothing is made before something is remembered");
    check(!Cache::isOriginal(original.Get(), g_ctx.Get()), "an untagged creation is not the original");
    check(cache.remember(original.Get(), hash, game.data(), game.size(), false).ok() && cache.remembered(), "the exact program is remembered");
    check(Cache::isOriginal(original.Get(), g_ctx.Get()), "...and its creation is tagged");
    check(!Cache::isOriginal(untagged.Get(), g_ctx.Get()) && !Cache::isOriginal(nullptr, g_ctx.Get()) && !Cache::isOriginal(original.Get(), nullptr),
          "another creation of the same bytes, null and no context are not the tagged original");
    std::vector<BYTE> expected;
    check(patch(game.data(), game.size(), hash, expected).ok() && cache.bytes() == expected, "what is kept is the patched container");
    check(cache.remember(original.Get(), hash, game.data(), game.size(), false).ok(), "a second creation of the same bytes is remembered again");
    Result why;
    patched = cache.prepare(g_ctx.Get(), &why);
    check(patched && why.ok() && cache.attempted() && cache.shader() == patched.Get(), "the patched shader is made on the device");
    check(cache.prepare(g_ctx.Get(), &why) == patched.Get(), "and kept: prepared once");
    check(!Cache::isOriginal(patched.Get(), g_ctx.Get()), "the patched shader is not mistaken for the original");
    cache.reset();  // `patched` holds a reference of its own
    check(cache.shader() == nullptr && !cache.attempted() && cache.remembered(), "reset lets the cache's shader go; what was remembered stays");
    check(cache.prepare(g_ctx.Get(), &why) != nullptr, "and a prepare after a reset makes it again");
}

// ---------------------------------------------------------------- the half-widths read back

static void probeCases() {
    float records[5 * 15] = {};
    const float widths[5] = {2.0f, 1.5f, 1.5f, 1.25f, 0.5f};
    for (int i = 0; i < 5; ++i) records[i * 15 + 3] = widths[i];
    // Slot 1 with an offset into a larger buffer, and a start instance, as a ring buffer would hand them.
    std::vector<float> big(100 * 15, 0.0f);
    for (int i = 0; i < 100; ++i) big[i * 15 + 3] = 100.0f + float(i);
    std::memcpy(&big[7 * 15], records, sizeof(records));
    auto vb = makeBuffer(UINT(big.size() * 4), D3D11_BIND_VERTEX_BUFFER, big.data());
    ID3D11Buffer* bufs[] = {vb.Get()};
    const UINT stride = 60, offset = 5 * 60;  // 5 records in, then start instance 2: record 7 is the draw's first
    auto poll = [&](InstanceProbe& p, uint32_t firstFrame, float* out, uint32_t* n) {
        auto r = InstanceProbe::Poll::kPending;
        for (uint32_t frame = firstFrame; frame < firstFrame + 400 && r == InstanceProbe::Poll::kPending; ++frame) {
            g_ctx->Flush();
            r = p.poll(g_ctx.Get(), frame, out, 64, n);
            if (r == InstanceProbe::Poll::kPending) Sleep(1);
        }
        return r;
    };
    g_ctx->ClearState();
    g_ctx->IASetVertexBuffers(1, 1, bufs, &stride, &offset);
    {
        InstanceProbe p;
        check(p.poll(g_ctx.Get(), 100, nullptr, 0, nullptr) == InstanceProbe::Poll::kIdle, "nothing issued: idle");
        check(p.begin(g_ctx.Get(), 5, 2, 10) && p.pending(), "begin copies the draw's records to a staging buffer");
        check(!p.begin(g_ctx.Get(), 5, 2, 10), "a second begin while one is pending is refused");
        float w[64] = {};
        uint32_t n = 0;
        check(p.poll(g_ctx.Get(), 10, w, 64, &n) == InstanceProbe::Poll::kPending, "the frame it was issued in: pending, never mapped");
        check(poll(p, 12, w, &n) == InstanceProbe::Poll::kDone && n == 5, "mapped without waiting once the frames have passed");
        check(w[0] == 2.0f && w[1] == 1.5f && w[2] == 1.5f && w[3] == 1.25f && w[4] == 0.5f, "the half-widths are the fourth float of each 60-byte record, from the offset and start instance");
        check(!p.pending(), "done: nothing pending");
        float few[2] = {};
        check(p.begin(g_ctx.Get(), 5, 2, 20), "a second read can be made");
        uint32_t two = 0;
        InstanceProbe::Poll r;
        for (uint32_t frame = 22; frame < 500; ++frame) {
            g_ctx->Flush();
            r = p.poll(g_ctx.Get(), frame, few, 2, &two);
            if (r != InstanceProbe::Poll::kPending) break;
            Sleep(1);
        }
        check(r == InstanceProbe::Poll::kDone && two == 2 && few[0] == 2.0f && few[1] == 1.5f, "a smaller output buffer takes the first records only");
    }
    {
        InstanceProbe p;
        check(!p.begin(g_ctx.Get(), 0, 0, 1) && !p.begin(g_ctx.Get(), InstanceProbe::kMaxInstances + 1, 0, 1) && !p.begin(nullptr, 5, 0, 1),
              "no instances, too many, or no context: refused");
        check(!p.begin(g_ctx.Get(), 5, 200, 1), "a range past the buffer is refused");
        const UINT other = 48;
        g_ctx->IASetVertexBuffers(1, 1, bufs, &other, &offset);
        check(!p.begin(g_ctx.Get(), 5, 0, 1), "a buffer of another stride is not the records the shader reads");
        g_ctx->IASetVertexBuffers(1, 1, bufs, &stride, &offset);
        ID3D11Buffer* none[] = {nullptr};
        const UINT zero = 0;
        g_ctx->IASetVertexBuffers(1, 1, none, &zero, &zero);
        check(!p.begin(g_ctx.Get(), 5, 0, 1), "no vertex buffer in slot 1: refused");
    }
    {
        // The copy that never arrives: the frames run out and it says failed, releasing everything.
        InstanceProbe p;
        g_ctx->IASetVertexBuffers(1, 1, bufs, &stride, &offset);
        check(p.begin(g_ctx.Get(), 3, 0, 1), "a read issued");
        float w[64];
        uint32_t n = 0;
        auto r = p.poll(g_ctx.Get(), 1 + InstanceProbe::kGiveUpFrames + 5, w, 64, &n);
        check(r == InstanceProbe::Poll::kDone || r == InstanceProbe::Poll::kPending || r == InstanceProbe::Poll::kFailed, "a very late poll answers");
        p.reset();
        check(!p.pending(), "reset ends a pending read");
    }
    g_ctx->ClearState();
}

// ---------------------------------------------------------------- the frame boundary's decision

static void targetCases() {
    Inputs in;
    in.vr = true;
    in.ready = true;
    in.panelLive = true;
    in.panelFactor = 0.5;
    check(target(in) == 0.5, "VR, ready, the panel patch live at 0.5: the draws take 0.5");
    in.panelFactor = 0.4;
    check(target(in) == 0.4, "and 0.4 at ui_quality 125");
    in.panelFactor = 0.25;
    check(target(in) == 0.25, "the panel patch's cap");
    for (double f : {1.0, 0.9999999, 1.5, 0.0, -0.5, std::nan(""), HUGE_VAL}) {
        in.panelFactor = f;
        check(target(in) == 1.0, "a factor at or above 1, not positive or not finite: exactly 1");
    }
    in.panelFactor = 0.5;
    Inputs flat = in;
    flat.vr = false;
    check(target(flat) == 1.0, "the flat build: 1");
    Inputs refused = in;
    refused.refused = true;
    check(target(refused) == 1.0, "a refused copy: 1");
    Inputs notReady = in;
    notReady.ready = false;
    check(target(notReady) == 1.0, "a copy that is not made yet: 1");
    Inputs off = in;
    off.panelLive = false;
    check(target(off) == 1.0, "the panel patch not live (ui_quality off, or stood down): 1");
    check(factor() == 1.0 && !active(), "the shared factor starts at 1");
    detail::g_factor.store(0.5);
    check(factor() == 0.5 && active(), "and is read back as stored");
    detail::g_factor.store(1.0);
}

// ---------------------------------------------------------------- the shader runs, end to end

struct Scenario {
    const char* name;
    double f;
};

static void shaderCases(const Draw& draw, const std::vector<BYTE>& game, ID3D11VertexShader* original, ID3D11VertexShader* patched) {
    Runner run(draw, game);
    auto gsGame = Runner::streamOut(game.data(), game.size());
    const Out stock = run.run(original, gsGame.Get(), nullptr);
    check(stock.v.size() == size_t(draw.vertices) * draw.instances * kOut, "the stream-out holds every vertex of every instance");

    // The game's own strip, as the investigation measured it: 2w pixels wide, w = 2.0 and 1.5.
    const unsigned atLeast[5] = {10, 0, 1000, 1000, 1000};
    const double halfWidth[5] = {2.0, 1.5, 1.5, 1.5, 1.5};
    for (unsigned i = 0; i < 5; ++i) {
        check(draw.vb1[i * 15 + 3] == float(halfWidth[i]), "the fixture's instance half-widths are 2.0, 1.5, 1.5, 1.5, 1.5");
        if (!atLeast[i]) continue;
        check(widthIs(measure(stock, draw, i), 2 * halfWidth[i], atLeast[i]), "the game's own shader draws the strip 2w pixels wide (4.000 and 3.000)");
    }

    // The patched shader through the production objects: the cache's copy, the constants and the binding, at each f.
    Constants constants;
    Binding binding;
    const auto accept = [&](ID3D11VertexShader* now) { return Cache::isOriginal(now, g_ctx.Get()); };
    auto patchedRun = [&](double f, double bufferFactor) {
        ID3D11Buffer* cb = constants.get(g_ctx.Get(), bufferFactor);
        check(cb != nullptr, "the factor's buffer");
        run.setup(gsGame.Get());
        g_ctx->VSSetShader(original, nullptr, 0);
        check(binding.begin(g_ctx.Get(), patched, cb, accept), "the binding admits the tagged original and binds the copy");
        (void)f;
        const Out o = run.issue();
        check(binding.finish(g_ctx.Get()).restored, "and gives the game's shader back");
        ComPtr<ID3D11VertexShader> after;
        g_ctx->VSGetShader(&after, nullptr, nullptr);
        check(after.Get() == original, "the game's shader is what is bound after the draw");
        return o;
    };

    const Out atOne = patchedRun(1.0, 1.0);
    check(identical(atOne, stock), "f = 1: every stream-out byte of the patched shader is the game's");
    std::printf("orbital width: f = 1 byte-identical to the game's shader over %zu stream-out bytes\n", atOne.v.size() * sizeof(float));

    const Scenario scenarios[] = {{"f 0.5 (ui_quality 100 at HMD Quality 0.50)", 0.5},
                                  {"f 0.4 (ui_quality 125 at HMD Quality 0.50)", 0.4},
                                  {"f 0.7", 0.7},
                                  {"f 0.25 (the cap)", 0.25}};
    std::vector<Out> scaled;
    for (const Scenario& s : scenarios) {
        const Out o = patchedRun(s.f, s.f);
        for (unsigned i = 0; i < 5; ++i) {
            if (!atLeast[i]) continue;
            const Strip strip = measure(o, draw, i);
            check(widthIs(strip, 2 * halfWidth[i] * s.f, atLeast[i]), "every pair of the strip is 2 x w x f pixels wide, by edge length and perpendicular to the line");
            if (i == 0 || i == 2)
                std::printf("orbital width: %-42s instance %u (w %.1f): %zu pairs, edge %.4f..%.4f px, perpendicular %.4f..%.4f px (2wf = %.4f)\n",
                            s.name, i, halfWidth[i], strip.edge.size(), lowest(strip.edge), highest(strip.edge), lowest(strip.perp),
                            highest(strip.perp), 2 * halfWidth[i] * s.f);
        }
        check(sameBesidesXy(o, stock), "only SV_POSITION's x and y move: the colour, the signed distance, z and w are the game's bytes");
        check(!identical(o, stock), "and the positions did move");
        scaled.push_back(o);
    }
    const Out& half = scaled[0];
    const Out& fortyPercent = scaled[1];

    // The coverage twin: the production text, the production constant buffer, the production binding of it. Its footprint is
    // the visible line's at every f.
    const auto twinCode = compileHlsl(edvr::kOrbitalCoverageVs, "vs_5_0");
    ComPtr<ID3D11VertexShader> twin;
    ck(g_dev->CreateVertexShader(twinCode->GetBufferPointer(), twinCode->GetBufferSize(), nullptr, &twin), "the twin");
    auto gsTwin = Runner::streamOut(twinCode->GetBufferPointer(), twinCode->GetBufferSize());
    Constants twinCb;
    auto twinRun = [&](double f) {
        ID3D11Buffer* cb = twinCb.get(g_ctx.Get(), f);
        check(cb != nullptr, "the twin's buffer");
        return run.run(twin.Get(), gsTwin.Get(), cb);
    };
    unsigned compared = 0;
    const double atOneError = footprintError(twinRun(1.0), stock, draw, &compared);
    std::printf("orbital width: the coverage twin at f = 1 lies within %.5f px of the game's shader over %u vertices\n", atOneError, compared);
    check(compared > 4000 && atOneError < 0.03, "the twin at f = 1 is the game's shader to the transcription's 0.03 px");
    for (size_t k = 0; k < sizeof(scenarios) / sizeof(scenarios[0]); ++k) {
        const double e = footprintError(twinRun(scenarios[k].f), scaled[k], draw, &compared);
        std::printf("orbital width: the coverage twin at %-42s lies within %.5f px of the patched game's draw over %u vertices\n", scenarios[k].name, e, compared);
        check(compared > 4000 && e < 0.03, "the twin's footprint lies on the patched draw's within 0.03 px");
        for (unsigned i = 0; i < 5; ++i)
            if (atLeast[i]) check(widthIs(measure(twinRun(scenarios[k].f), draw, i), 2 * halfWidth[i] * scenarios[k].f, atLeast[i]),
                                  "the twin's strip is the same 2wf pixels wide");
    }

    // THE MUTANTS. Each wrong thing is run through the same comparators, and each must be caught.
    // (1) the factor not applied: the copy bound with the game's own 2 at f = 0.5.
    {
        const Out o = patchedRun(0.5, 1.0);
        bool allMatch = true;
        for (unsigned i = 0; i < 5; ++i)
            if (atLeast[i] && !widthIs(measure(o, draw, i), 2 * halfWidth[i] * 0.5, atLeast[i])) allMatch = false;
        check(!allMatch, "MUTANT, the factor not applied: the width check fails");
        check(widthIs(measure(o, draw, 0), 4.0, 10), "...because the strip is still the game's 4 px");
    }
    // (2) the factor applied twice: f x f in the buffer.
    {
        const Out o = patchedRun(0.5, 0.25);
        bool allMatch = true;
        for (unsigned i = 0; i < 5; ++i)
            if (atLeast[i] && !widthIs(measure(o, draw, i), 2 * halfWidth[i] * 0.5, atLeast[i])) allMatch = false;
        check(!allMatch, "MUTANT, the factor applied twice: the width check fails");
        check(widthIs(measure(o, draw, 0), 1.0, 10), "...because the strip is 1 px, not 2");
    }
    // (3) a constant one ulp off at f = 1: the byte-identity check fails.
    {
        const float off = std::nextafter(2.0f, 3.0f);
        const float lanes[4] = {off, off, off, off};
        auto cb = makeBuffer(16, D3D11_BIND_CONSTANT_BUFFER, lanes);
        run.setup(gsGame.Get());
        g_ctx->VSSetShader(patched, nullptr, 0);
        ID3D11Buffer* b = cb.Get();
        g_ctx->VSSetConstantBuffers(kSlot, 1, &b);
        check(!identical(run.issue(), stock), "MUTANT, a constant one ulp off the game's 2: the byte-identity check fails");
    }
    // (4) the twin left at 1, or scaled twice, against the patched draw at f = 0.5.
    {
        const double unscaled = footprintError(twinRun(1.0), half, draw);
        const double twice = footprintError(twinRun(0.25), half, draw);
        std::printf("orbital width: twin mutants against the patched draw at f 0.5: left at 1 -> %.4f px, scaled twice -> %.4f px (limit 0.03)\n", unscaled, twice);
        check(!(unscaled < 0.03), "MUTANT, the twin left at 1: the footprint check fails");
        check(!(twice < 0.03), "MUTANT, the twin scaled twice: the footprint check fails");
        check(!(footprintError(twinRun(0.5), fortyPercent, draw) < 0.03), "MUTANT, the twin at 0.5 against a draw made at 0.4: the footprint check fails");
        check(footprintError(twinRun(0.4), fortyPercent, draw) < 0.03, "...and at the factor the draw was made with it passes");
    }
    // (5) the patched copy reading zeros from its buffer (a buffer never written): no strip at all, caught.
    {
        const float zeros[4] = {};
        auto cb = makeBuffer(16, D3D11_BIND_CONSTANT_BUFFER, zeros);
        run.setup(gsGame.Get());
        g_ctx->VSSetShader(patched, nullptr, 0);
        ID3D11Buffer* b = cb.Get();
        g_ctx->VSSetConstantBuffers(kSlot, 1, &b);
        const Out o = run.issue();
        check(!widthIs(measure(o, draw, 0), 4.0, 10) && !widthIs(measure(o, draw, 0), 2.0, 10), "MUTANT, a zero in the factor's buffer: a zero-width strip is caught");
    }
    g_ctx->ClearState();
}

// ---------------------------------------------------------------- the wiring pins

#include "wiring_cases.h"

int main(int argc, char** argv) {
    try {
        if (argc == 2 && !std::strcmp(argv[1], "--dry-run")) {
            std::puts("orbital_width_test: dry-run (no files)");
            return 0;
        }
        if (argc == 2 && !std::strcmp(argv[1], "--wiring")) {
            wiringCases();
            std::printf("PASS orbital_width_test --wiring: %u checks\n", checks);
            return 0;
        }
        // --hardware-self-test runs the same checks on the machine's own adapter, where the driver compiles the game's program and the
        // patched copy itself: not a gate (a build machine may have no adapter), the evidence that a real driver takes the edit and
        // keeps f = 1 byte for byte.
        const bool hardware = argc == 2 && !std::strcmp(argv[1], "--hardware-self-test");
        check(argc == 2 && (hardware || !std::strcmp(argv[1], "--self-test")),
              "usage: orbital_width_test --self-test | --hardware-self-test | --wiring | --dry-run");
        const D3D_DRIVER_TYPE driver = hardware ? D3D_DRIVER_TYPE_HARDWARE : D3D_DRIVER_TYPE_WARP;
        const HRESULT made = edvr::systemD3D11CreateDevice()(nullptr, driver, nullptr, D3D11_CREATE_DEVICE_DEBUG, nullptr, 0,
                                                              D3D11_SDK_VERSION, &g_dev, nullptr, &g_ctx);
        if (FAILED(made))
            ck(edvr::systemD3D11CreateDevice()(nullptr, driver, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &g_dev, nullptr, &g_ctx), "device");
        g_dev.As(&g_messages);
        std::printf("orbital_width_test: %s%s\n", hardware ? "hardware adapter" : "WARP", SUCCEEDED(made) ? " with the debug layer" : "");
        {
            ComPtr<IDXGIDevice> dxgi;
            ComPtr<IDXGIAdapter> adapter;
            DXGI_ADAPTER_DESC desc{};
            if (SUCCEEDED(g_dev.As(&dxgi)) && SUCCEEDED(dxgi->GetAdapter(&adapter)) && SUCCEEDED(adapter->GetDesc(&desc)))
                std::printf("adapter: %ls (vendor=%04X device=%04X)\n", desc.Description, desc.VendorId, desc.DeviceId);
        }

        const auto game = readBytes("tools/orbital_width_test/fixtures/vs_C7FA0C0F5DD49180.dxbc");
        check(game.size() == kBytes, "the game's shader fixture is 2412 bytes");
        const Draw draw = loadDraw("tools/orbital_width_test/fixtures/draw_180540_23650.bin");
        check(draw.width == 2016 && draw.height == 1949 && draw.cb1[332 * 4] == 2016.0f && draw.cb1[332 * 4 + 1] == 1949.0f &&
                  std::fabs(draw.cb1[332 * 4 + 2] - 1.0f / 2016.0f) < 1e-9f && std::fabs(draw.cb1[332 * 4 + 3] - 1.0f / 1949.0f) < 1e-9f,
              "cb1[332] is (2016, 1949, 1/2016, 1/1949), as in all 38 captured draws");

        editCases(game);
        constantsCases();
        ComPtr<ID3D11VertexShader> original, patched;
        cacheCases(game, original, patched);
        bindingCases(game, original.Get(), patched.Get());
        probeCases();
        targetCases();
        shaderCases(draw, game, original.Get(), patched.Get());
        wiringCases();

        if (g_messages) {
            for (UINT64 i = 0; i < g_messages->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i) {
                SIZE_T size = 0;
                g_messages->GetMessage(i, nullptr, &size);
                std::vector<char> data(size);
                auto* m = reinterpret_cast<D3D11_MESSAGE*>(data.data());
                ck(g_messages->GetMessage(i, m, &size), "debug message");
                if (m->Severity <= D3D11_MESSAGE_SEVERITY_ERROR) {
                    std::puts(m->pDescription);
                    check(false, "D3D debug layer");
                }
            }
        }
        std::printf("PASS orbital_width_test: %u checks; the game's shader and the production patched copy, cache, binding, constants, read-back and twin on %s\n", checks,
                    hardware ? "the hardware adapter" : "WARP");
        return 0;
    } catch (const std::exception& e) {
        std::printf("FAIL %s (%u checks)\n", e.what(), checks);
        return 1;
    }
}
