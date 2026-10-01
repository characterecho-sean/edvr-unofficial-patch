// terrain_retired_test: the A/B proof that cutting the retired terrain-motion
// path out of the temporal compute shader changed nothing for everyone who
// never turned it on.
//
// On 2026-10-01 advanced.terrain_motion was retired. The shader text
// (src/d3d11/temporal_shader_source.h, kTemporalCsHlsl) lost its t9..t11
// declarations (TI, TZ, TerrainRecord and TR), the function terrainPixel() and
// its two call sites (fetchHistoryT, and mv, where the call set decisionPath =
// 7). The path was gated by bit 8 of probe.w: with the bit clear terrainPixel()
// returned false before it read a thing, which is the state every player
// without the key was in. The claim proved here, on WARP, bit for bit:
//
//   with probe.w bit 8 CLEAR, the shader before the cut (the "reference") and
//   the shader after it produce byte-identical outputs for the same inputs,
//   for BOTH entries (mv and main), every UAV they write (O, N, Stats, MV, ZC,
//   MK, UN, ML), over scenes that carry terrain-like pixels (an index texture,
//   a matching depth and a record buffer, bound to both shaders).
//
// How it does that without keeping the old shader in the tree: the reference
// is REBUILT from the current text by re-inserting the removed fragments
// (kEdits below, hard-coded) at exact anchors, each of which must be found
// exactly once. Both texts are compiled with the production flags
// (tools/temporal_shader_build: cs_5_0, flags 0, the macro
// EDVR_TEMPORAL_DIAGNOSTICS 1 for the diagnostic variants and 0 for the FAST
// ones), all concurrently because the `main` compile alone is about 20 s in
// fxc. The default run pins no hash of the text: an unrelated shader edit does
// not break it, only a moved anchor does (the five fragment anchors in kEdits
// and the four mutation anchors in kMutations; each must be found exactly
// once, and the failure says which). Because the reference is the new text plus
// the fragments, the A/B isolates exactly what the fragments do; --verify-old
// is what ties that reference to the real old file.
//
// Non-vacuity, so a green run cannot mean "the harness cannot see terrain":
//   * reflection: the reference declares TI/TZ/TR at t9..t11, the new text
//     declares nothing there, and cbuffer P is identical in both;
//   * control: the reference with bit 8 SET and the terrain resources bound
//     moves the pixels the old gate admits (TI != 0, depth matches), to the
//     motion a CPU model of the old function computes, and no other pixel; the
//     new shader ignores the bit;
//   * mutation: a one-token break of the camera term in the new text (the sign
//     of the head path's or the world path's translation, in each entry) is
//     caught by the same A/B, in every configuration where that line runs and
//     in none where it does not;
//   * path coverage: the world path (Stats[15]) is taken exactly in the
//     configurations that ask for it;
//   * WARP determinism: the new shader is dispatched twice per configuration
//     and the two runs must agree before the A/B is read.
//
// It is the proof of the 2026-10-01 retirement of advanced.terrain_motion and
// is DELETABLE: it exists for this one edit. When a later change moves one of
// the anchors in kEdits the rig fails with "anchor ... found 0 times"; that
// means the retirement is old history and the rig can go, along with its
// build.bat line.
//
// Run from the repository root (the shader is read at a repo-relative path):
//   terrain_retired_test.exe              the A/B on WARP (about 30 s)
//   terrain_retired_test.exe --self-test  the same
//   terrain_retired_test.exe --dry-run    text only: load, rebuild the
//                                         reference, check every anchor; no
//                                         device, no compile; writes nothing
//   terrain_retired_test.exe --verify-old <old temporal_shader_source.h>
//                                         one shot: extract the HLSL from the
//                                         saved pre-retirement file the same
//                                         way and require the rebuilt
//                                         reference to be byte-identical to it
//                                         (prints both FNV-1a-64 hashes).
//                                         Take the old file from the commit
//                                         before the retirement:
//                                         git show <commit>:src/d3d11/temporal_shader_source.h
//
// Exit codes: 0 all checks passed, 1 a check failed (the message names the
// first differing pixel and texture), 2 usage.

#include <windows.h>
#include <d3d11.h>
#include <d3d11shader.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

using Clock = std::chrono::steady_clock;
double secondsSince(Clock::time_point t0) {
    return std::chrono::duration<double>(Clock::now() - t0).count();
}

unsigned g_checks = 0;

[[noreturn]] void die(const std::string& why) {
    std::fflush(stdout);
    std::fprintf(stderr, "FAIL: %s\n", why.c_str());
    std::fflush(stderr);
    std::exit(1);
}
// A counted check: one claim of the proof.
void check(bool ok, const std::string& why) {
    ++g_checks;
    if (!ok) die(why);
}
// An uncounted guard on the harness itself (a missing variable, a failed API
// call): it is not a claim, so it does not inflate the count.
void require(bool ok, const std::string& why) {
    if (!ok) die(why);
}

std::string hexOf(HRESULT h) {
    char b[16];
    std::snprintf(b, sizeof(b), "%08X", static_cast<unsigned>(h));
    return b;
}
void hr(HRESULT h, const char* what) {
    if (FAILED(h)) die(std::string(what) + " failed (0x" + hexOf(h) + ")");
}

// ---------------------------------------------------------------------------
// 1. The shader text
// ---------------------------------------------------------------------------

constexpr const char* kShaderPath = "src/d3d11/temporal_shader_source.h";

bool readFileLf(const char* path, std::string& out) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    std::stringstream text;
    text << file.rdbuf();
    out = text.str();
    // The tree is LF (.gitattributes), so this changes nothing there; a CRLF
    // checkout of either file would otherwise defeat every anchor below.
    out.erase(std::remove(out.begin(), out.end(), '\r'), out.end());
    return true;
}

// kTemporalCsHlsl as the compiler sees it: the adjacent raw literals
// concatenated, the `)HLSL"` / `R"HLSL(` boundaries gone
// (tools/engine_velocity_test/consumer_tests.h's loadTemporalHlsl, the
// same walk). The `// (adjacent literals ...)` C++ comments between literals
// are outside every literal and so are not in the result.
bool extractHlsl(const std::string& source, std::string& hlsl, std::string& why) {
    const size_t start = source.find("constexpr char kTemporalCsHlsl[]");
    if (start == std::string::npos) { why = "kTemporalCsHlsl not found"; return false; }
    const size_t end = source.find(")HLSL\";", start);
    if (end == std::string::npos) { why = "the end of kTemporalCsHlsl not found"; return false; }
    hlsl.clear();
    size_t cursor = start;
    for (;;) {
        size_t begin = source.find("R\"HLSL(", cursor);
        if (begin == std::string::npos || begin > end) break;
        begin += 7;
        const size_t close = source.find(")HLSL\"", begin);
        if (close == std::string::npos || close > end) { why = "an unterminated raw literal"; return false; }
        hlsl.append(source, begin, close - begin);
        cursor = close + 6;
    }
    if (hlsl.empty()) { why = "kTemporalCsHlsl holds no literal"; return false; }
    return true;
}

uint64_t fnv1a64(const std::string& s) {
    uint64_t h = 14695981039346656037ull;
    for (const unsigned char c : s) { h ^= c; h *= 1099511628211ull; }
    return h;
}

enum class Where { Replace, After, Before };

// What `git diff --no-index OLD NEW` shows. `anchor` is a text found exactly
// once in the NEW HLSL; `text` goes in place of it (Replace), after it
// (After) or before it (Before). `cosmetic` edits restore comment text only:
// they change no byte the compiler acts on, so the default run applies them
// when their anchor is still there and skips them when it is not, while
// --verify-old (which wants byte identity with the old file) requires them.
struct Edit {
    const char* name;
    Where where;
    const char* anchor;
    const char* text;
    bool cosmetic;
};

const Edit kEdits[] = {
    // (1) The three declarations at t9, t10 and the record struct.
    {"declarations of TI (t9), TZ (t10) and TerrainRecord", Where::Replace,
     R"TXT(// t9..t11 were the terrain patch index, its depth and its transform records (retired 2026-10-01: terrain takes the camera's motion).
)TXT",
     R"TXT(Texture2D<uint> TI : register(t9);      // terrain patch index, zero outside rasterised coverage
Texture2D<float> TZ : register(t10);    // terrain depth, compared against final scene/UI depth
struct TerrainRecord { uint4 key[12]; float4 q; float4 t; float4 r[3]; };
)TXT",
     false},

    // (2) The record buffer at t11, after Screen at t14.
    {"declaration of TR (t11)", Where::After,
     R"TXT(Texture2D<float4> Screen : register(t14);
)TXT",
     R"TXT(StructuredBuffer<TerrainRecord> TR : register(t11);
)TXT",
     false},

    // (3) terrainPixel itself, right after the end of backgroundHistoryHidden
    // (where the old text had it, before the zPred comment).
    {"function terrainPixel", Where::After,
     R"TXT(    return zPred>0 && nearest>knobs.z/zPred*1.03;
}
)TXT",
     R"TXT(// Exact terrain coverage only. The patch transform is in DirectX view
// space (+Z forward); the pass's rays use the runtime's -Z convention.
bool terrainPixel(float2 p, float3 d, out float2 pp, out float zp) {
    pp=0; zp=0;
    if ((uint(probe.w+0.5)&8u)==0u) return false;
    int2 q=region.xy+int2(p);
    uint index=TI.Load(int3(q,0));
    if (knobs.y==0 || index==0 || index>512 || uiCovered(q)) return false;
    float zraw=TZ.Load(int3(q,0)), scene=zSceneAt(q);
    if (zraw<=knobs.x || abs(scene-zraw)>abs(zraw)*0.00001) return false;
    TerrainRecord rec=TR[index-1];
    if (rec.t.w!=1) return false;
    float z=knobs.z/(zraw-knobs.x);
    float4 here=float4(d.xy*z,z,1);
    float3 before=float3(dot(rec.r[0],here),dot(rec.r[1],here),dot(rec.r[2],here));
    if (before.z<=0 || !all(isfinite(before))) return false;
    zp=before.z;
    pp.x=(before.x/zp-tanPrev.x)/(tanPrev.y-tanPrev.x)*size.x-0.5;
    pp.y=(tanPrev.w-before.y/zp)/(tanPrev.w-tanPrev.z)*size.y-0.5;
    return true;
}
)TXT",
     false},

    // (4) The call site in fetchHistoryT (the main entry's history fetch).
    {"fetchHistoryT call site", Where::Before,
     R"TXT(    float2 holoP; float holoZ;
    if (allowWorld && holoPixel(p,jit.xy,holoP,holoZ)) {
)TXT",
     R"TXT(    float2 terrainP; float terrainZ;
    if (allowWorld && terrainPixel(p,d,terrainP,terrainZ)) {
        pp=terrainP; zPred=terrainZ; world=1;
        if (any(pp<0) || any(pp>float2(size)-1)) return false;
    }
)TXT",
     false},

    // (5) The call site in mv (decisionPath 7).
    {"mv call site (decisionPath 7)", Where::Before,
     R"TXT(            float2 holoP; float holoZ;
            if(holoPixel(p,0,holoP,holoZ)) {
)TXT",
     R"TXT(            float2 terrainP; float terrainZ;
            if (terrainPixel(p,d,terrainP,terrainZ)) {
                pp=terrainP; motion=pp-p; zPred=terrainZ;
                decisionPath=7u;
            }
)TXT",
     false},

    // (6, 7) Comment text the retirement also reworded; byte identity with the
    // old file needs them, the compiler does not.
    {"comment: probe.w bit 8 description", Where::Replace,
     "4 adaptive UI, 8 (retired 2026-10-01: terrain), 16 holo",
     "4 adaptive UI, 8 terrain, 16 holo",
     true},
    {"comment: backgroundHistoryHidden", Where::Replace,
     R"TXT(    // 3x3-dilated depth; the terrain path (retired 2026-10-01) had an
    // exact depth that met the dilated footprint at every terrain edge
    // instead, 5.3% of the terrain's pixels a frame walking with the
    // jitter -- the shimmer of the eye run of 2026-09-17 11:48
    // (docs/terrain-history-shimmer-2026-09-17.md).
)TXT",
     R"TXT(    // 3x3-dilated depth; the terrain path's exact depth met the dilated
    // footprint at every terrain edge instead, 5.3% of the terrain's
    // pixels a frame walking with the jitter -- the shimmer of the eye
    // run of 2026-09-17 11:48 (docs/terrain-history-shimmer-2026-09-17.md).
)TXT",
     true},
};

std::string toLf(std::string s) {
    s.erase(std::remove(s.begin(), s.end(), '\r'), s.end());
    return s;
}

size_t countOf(const std::string& hay, const std::string& needle) {
    size_t n = 0;
    for (size_t p = hay.find(needle); p != std::string::npos; p = hay.find(needle, p + 1)) ++n;
    return n;
}

struct Rebuilt {
    std::string text;
    int applied = 0;    // edits made
    int skipped = 0;    // cosmetic edits whose anchor is gone (default run only)
    int cosmetic = 0;   // cosmetic edits made
};

// Rebuild the OLD text from the NEW one. Returns false and a clear message
// when an anchor is missing or ambiguous.
bool rebuildReference(const std::string& newText, bool requireCosmetic, Rebuilt& out, std::string& why) {
    out = Rebuilt{};
    out.text = newText;
    for (const Edit& e : kEdits) {
        const std::string anchor = toLf(e.anchor), text = toLf(e.text);
        const size_t inNew = countOf(newText, anchor);
        if (inNew == 0 && e.cosmetic && !requireCosmetic) { ++out.skipped; continue; }
        if (inNew != 1) {
            why = std::string("anchor of '") + e.name + "' found " + std::to_string(inNew) +
                  " times in " + kShaderPath + " (expected exactly 1); the shader moved since the retirement, "
                  "so this rig has done its job and can be deleted";
            return false;
        }
        const size_t at = out.text.find(anchor);
        if (at == std::string::npos || countOf(out.text, anchor) != 1) {
            why = std::string("anchor of '") + e.name + "' is no longer unique after an earlier fragment went in";
            return false;
        }
        switch (e.where) {
        case Where::Replace: out.text.replace(at, anchor.size(), text); break;
        case Where::After: out.text.insert(at + anchor.size(), text); break;
        case Where::Before: out.text.insert(at, text); break;
        }
        ++out.applied;
        if (e.cosmetic) ++out.cosmetic;
    }
    return true;
}

// One token of the camera term, flipped (the sign of a translation), in each
// entry and on each of its two paths: the mutations the A/B must catch.
// `needsWorld`: the line runs only on the world path (tvCam.w != 0).
struct Mutation {
    const char* name;
    const char* entry;
    const char* anchor;
    const char* mutated;
    bool needsWorld;
};
const Mutation kMutations[] = {
    {"mv head path: tvUsed sign flipped", "mv", "dp = dp * z + tvUsed.xyz;", "dp = dp * z - tvUsed.xyz;", false},
    {"mv world path: tvCam sign flipped", "mv", "\n                    dp = dp * z + tvCam.xyz;",
     "\n                    dp = dp * z - tvCam.xyz;", true},
    {"main head path: fetchHistoryT tv sign flipped", "main", "dp = dp * z + tv;", "dp = dp * z - tv;", false},
    {"main world path: fetchHistoryT tvCam sign flipped", "main", "\n                dp = dp * z + tvCam.xyz;",
     "\n                dp = dp * z - tvCam.xyz;", true},
};
constexpr int kMutationCount = 4;

bool mutate(const std::string& newText, const Mutation& m, std::string& out, std::string& why) {
    const size_t n = countOf(newText, m.anchor);
    if (n != 1) {
        why = std::string("mutation '") + m.name + "': anchor found " + std::to_string(n) + " times (expected 1)";
        return false;
    }
    out = newText;
    out.replace(out.find(m.anchor), std::strlen(m.anchor), m.mutated);
    return true;
}

// ---------------------------------------------------------------------------
// 2. Compiling, concurrently
// ---------------------------------------------------------------------------

struct Compiled {
    ComPtr<ID3DBlob> code;
    std::string log;
    HRESULT hr = E_FAIL;
    double seconds = 0.0;
};

// The production call: tools/temporal_shader_build compiles cs_5_0 with flags
// 0 and 0, the production source names, and the macro
// EDVR_TEMPORAL_DIAGNOSTICS set to 1 (diagnostic) or 0 (fast).
void compileOne(const std::string* hlsl, const char* sourceName, const char* entry, bool diagnostics, Compiled* out) {
    const auto t0 = Clock::now();
    const D3D_SHADER_MACRO macros[] = {{"EDVR_TEMPORAL_DIAGNOSTICS", diagnostics ? "1" : "0"}, {nullptr, nullptr}};
    ComPtr<ID3DBlob> errors;
    out->hr = D3DCompile(hlsl->data(), hlsl->size(), sourceName, macros, nullptr, entry, "cs_5_0", 0, 0,
                         out->code.ReleaseAndGetAddressOf(), errors.GetAddressOf());
    if (errors && errors->GetBufferSize())
        out->log.assign(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize());
    out->seconds = secondsSince(t0);
}

struct Layout {
    UINT size = 0;
    std::map<std::string, std::pair<UINT, UINT>> var;   // name -> offset, size
    bool operator==(const Layout& o) const { return size == o.size && var == o.var; }
};

bool reflectLayout(ID3DBlob* code, Layout& out, std::string& why) {
    ComPtr<ID3D11ShaderReflection> r;
    if (FAILED(D3DReflect(code->GetBufferPointer(), code->GetBufferSize(), __uuidof(ID3D11ShaderReflection),
                          reinterpret_cast<void**>(r.GetAddressOf())))) {
        why = "D3DReflect failed";
        return false;
    }
    ID3D11ShaderReflectionConstantBuffer* cb = r->GetConstantBufferByName("P");
    D3D11_SHADER_BUFFER_DESC bd{};
    if (!cb || FAILED(cb->GetDesc(&bd))) { why = "cbuffer P not found by reflection"; return false; }
    out = Layout{};
    out.size = bd.Size;
    for (UINT i = 0; i < bd.Variables; ++i) {
        ID3D11ShaderReflectionVariable* v = cb->GetVariableByIndex(i);
        D3D11_SHADER_VARIABLE_DESC vd{};
        if (!v || FAILED(v->GetDesc(&vd))) { why = "cbuffer P variable unreadable"; return false; }
        out.var[vd.Name] = {vd.StartOffset, vd.Size};
    }
    return true;
}

// The shader-resource bindings a shader declares in t[lo]..t[hi], by slot.
std::vector<std::string> srvsInRange(ID3DBlob* code, UINT lo, UINT hi) {
    std::vector<std::pair<UINT, std::string>> found;
    ComPtr<ID3D11ShaderReflection> r;
    if (FAILED(D3DReflect(code->GetBufferPointer(), code->GetBufferSize(), __uuidof(ID3D11ShaderReflection),
                          reinterpret_cast<void**>(r.GetAddressOf()))))
        return {};
    D3D11_SHADER_DESC sd{};
    if (FAILED(r->GetDesc(&sd))) return {};
    for (UINT i = 0; i < sd.BoundResources; ++i) {
        D3D11_SHADER_INPUT_BIND_DESC bd{};
        if (FAILED(r->GetResourceBindingDesc(i, &bd))) continue;
        const bool srv = bd.Type == D3D_SIT_TEXTURE || bd.Type == D3D_SIT_STRUCTURED ||
                         bd.Type == D3D_SIT_BYTEADDRESS || bd.Type == D3D_SIT_TBUFFER;
        if (srv && bd.BindPoint >= lo && bd.BindPoint <= hi)
            found.emplace_back(bd.BindPoint, std::string(bd.Name) + "(t" + std::to_string(bd.BindPoint) + ")");
    }
    std::sort(found.begin(), found.end());
    std::vector<std::string> names;
    for (const auto& f : found) names.push_back(f.second);
    return names;
}

std::string joined(const std::vector<std::string>& v) {
    std::string s;
    for (const auto& x : v) s += (s.empty() ? "" : " ") + x;
    return s.empty() ? "nothing" : s;
}

// ---------------------------------------------------------------------------
// 3. The scene: a deterministic LCG tile with terrain-like pixels
// ---------------------------------------------------------------------------

constexpr int kTexW = 64, kTexH = 48;   // the game's texture
constexpr int kW = 37, kH = 29;         // the region (render) size; not a multiple of 8
constexpr int kTerrainRecords = 16;
constexpr UINT kStatsN = 64;
constexpr float kDepthB = 0.025f;       // knobs.z: written depth = knobs.x + knobs.z / metres
constexpr float kSplitMetres = 60.0f;   // split.x: nearer is the head's path, farther the camera's
constexpr float kTanNow[4] = {-1.00f, 1.02f, -0.98f, 1.01f};    // l r t b; not equal to the previous frame's
constexpr float kTanPrev[4] = {-1.01f, 1.00f, -1.00f, 0.99f};

struct Lcg {
    uint32_t s;
    uint32_t next() { s = s * 1664525u + 1013904223u; return s; }
    float unit() { return static_cast<float>(next() >> 8) * (1.0f / 16777216.0f); }
};

struct Rot { float r[3][4]; };   // three rows [R | t]

Rot makeRot(double yaw, double pitch, double roll, const double t[3]) {
    const double cy = std::cos(yaw), sy = std::sin(yaw), cp = std::cos(pitch), sp = std::sin(pitch),
                 cr = std::cos(roll), sr = std::sin(roll);
    const double ry[3][3] = {{cy, 0, sy}, {0, 1, 0}, {-sy, 0, cy}};
    const double rx[3][3] = {{1, 0, 0}, {0, cp, -sp}, {0, sp, cp}};
    const double rz[3][3] = {{cr, -sr, 0}, {sr, cr, 0}, {0, 0, 1}};
    double tmp[3][3], m[3][3];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) tmp[i][j] = rz[i][0] * rx[0][j] + rz[i][1] * rx[1][j] + rz[i][2] * rx[2][j];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) m[i][j] = tmp[i][0] * ry[0][j] + tmp[i][1] * ry[1][j] + tmp[i][2] * ry[2][j];
    Rot out{};
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) out.r[i][j] = static_cast<float>(m[i][j]);
        out.r[i][3] = static_cast<float>(t[i]);
    }
    return out;
}

struct TerrainRec {   // struct TerrainRecord { uint4 key[12]; float4 q; float4 t; float4 r[3]; }
    uint32_t key[48];
    float q[4];
    float t[4];
    float r[3][4];
};
static_assert(sizeof(TerrainRec) == 272, "TerrainRecord stride");

struct Scene {
    const char* name = "";
    uint32_t seed = 0;
    int regX = 0, regY = 0; // this eye's region inside the texture: never the origin
    bool ui = false;        // UM, UP and ZP bound, coverage on, mover mask on, DLSS depth history valid
    float depthA = 0.0f;    // knobs.x
    std::vector<float> s, z, tz;        // texture-sized (s is RGBA)
    std::vector<uint32_t> ti;           // texture-sized
    std::vector<float> h, zp, up;       // region-sized (h and up are RGBA)
    std::vector<uint8_t> um;            // region-sized
    std::vector<TerrainRec> tr;
    ComPtr<ID3D11ShaderResourceView> srv[12];   // slots t0..t11

    size_t texIndex(int px, int py) const { return static_cast<size_t>(py + regY) * kTexW + static_cast<size_t>(px + regX); }
};

float depthRaw(const Scene& sc, float metres) {
    return metres > 0.0f ? sc.depthA + kDepthB / metres : 0.0f;
}

Scene makeScene(const char* name, uint32_t seed, int regX, int regY, bool ui, float depthA) {
    Scene sc;
    sc.name = name;
    sc.seed = seed;
    sc.regX = regX;
    sc.regY = regY;
    sc.ui = ui;
    sc.depthA = depthA;
    Lcg g{seed};
    const size_t texN = static_cast<size_t>(kTexW) * kTexH, regN = static_cast<size_t>(kW) * kH;
    const int cellsX = (kTexW + 3) / 4, cellsY = (kTexH + 3) / 4;
    std::vector<uint8_t> terrainCell(static_cast<size_t>(cellsX) * cellsY);
    for (auto& c : terrainCell) c = g.unit() < 0.62f ? 1 : 0;   // ~60% of 4x4 patches are terrain

    sc.s.assign(texN * 4, 0.0f);
    sc.z.assign(texN, 0.0f);
    sc.tz.assign(texN, 0.0f);
    sc.ti.assign(texN, 0u);
    for (int y = 0; y < kTexH; ++y) {
        for (int x = 0; x < kTexW; ++x) {
            const size_t i = static_cast<size_t>(y) * kTexW + x;
            const int rx = std::min(std::max(x - regX, 0), kW - 1);
            const int ry = std::min(std::max(y - regY, 0), kH - 1);
            // Depth in metres: sky above, a terrain slope receding upward, a
            // strip straddling split.x, the cockpit's dash below, and noise.
            float metres;
            if (ry < 5) {
                metres = g.unit() < 0.10f ? 2500.0f + 1000.0f * g.unit() : 0.0f;
            } else if (ry < 22) {
                metres = 150.0f + 38.0f * static_cast<float>(ry - 5) + 20.0f * g.unit();
                if (rx >= 16 && rx <= 20) metres = 40.0f + 10.0f * static_cast<float>(rx - 16) + 3.0f * g.unit();
            } else {
                metres = 1.0f + 0.55f * static_cast<float>(ry - 22) + 0.3f * g.unit();
            }
            const float u = g.unit();
            if (u < 0.05f) metres = 0.7f * std::exp(g.unit() * std::log(4000.0f / 0.7f));
            else if (u < 0.07f) metres = 0.0f;
            float raw = depthRaw(sc, metres);
            if (depthA > 0.0f && u >= 0.07f && u < 0.09f) raw = sc.depthA * 0.5f;   // behind the far plane: reads as sky
            sc.z[i] = raw;

            // The frame's colour: smooth plus noise, and a few bright pixels
            // (luma over 0.6 feeds the 16/17 counters).
            const float base = 0.25f + 0.30f * std::sin(0.37f * static_cast<float>(x) + 0.011f * static_cast<float>(seed & 255u)) *
                                           std::cos(0.29f * static_cast<float>(y));
            float rgb[3] = {base + 0.20f * (g.unit() - 0.5f), 0.8f * base + 0.20f * (g.unit() - 0.5f),
                            0.6f * base + 0.20f * (g.unit() - 0.5f)};
            if (g.unit() < 0.04f) for (float& c : rgb) c = 0.75f + 0.25f * g.unit();
            for (int k = 0; k < 3; ++k) sc.s[i * 4 + static_cast<size_t>(k)] = std::min(std::max(rgb[k], 0.0f), 1.0f);
            sc.s[i * 4 + 3] = 1.0f;

            // The terrain's own index and depth: ~60% of pixels, in 4x4
            // patches, depth equal to the scene's for most and deliberately
            // off or empty for the rest (the old gate rejected those).
            if (terrainCell[static_cast<size_t>(y / 4) * cellsX + x / 4]) {
                sc.ti[i] = 1u + static_cast<uint32_t>((y / 4) * cellsX + x / 4) % kTerrainRecords;
                const float v = g.unit();
                sc.tz[i] = v < 0.88f ? raw : (v < 0.94f ? raw * 1.01f : 0.0f);
            }
        }
    }

    // Region-sized inputs.
    sc.h.assign(regN * 4, 0.0f);
    sc.zp.assign(regN, 0.0f);
    sc.up.assign(regN * 4, 0.0f);
    sc.um.assign(regN, 0);
    for (int y = 0; y < kH; ++y) {
        for (int x = 0; x < kW; ++x) {
            const size_t i = static_cast<size_t>(y) * kW + x;
            const float base = 0.25f + 0.30f * std::sin(0.37f * static_cast<float>(x + 1) + 0.011f * static_cast<float>(seed & 255u)) *
                                           std::cos(0.29f * static_cast<float>(y));
            sc.h[i * 4 + 0] = std::min(std::max(base + 0.30f * (g.unit() - 0.5f), 0.0f), 1.0f);
            sc.h[i * 4 + 1] = std::min(std::max(0.8f * base + 0.30f * (g.unit() - 0.5f), 0.0f), 1.0f);
            sc.h[i * 4 + 2] = std::min(std::max(0.6f * base + 0.30f * (g.unit() - 0.5f), 0.0f), 1.0f);
            sc.h[i * 4 + 3] = 1.0f;
            // Last frame's depth: this frame's, a pixel over, a percent off,
            // and a few pixels that were sky or something else entirely.
            const int tx = std::min(x + regX + 1, kTexW - 1), ty = y + regY;
            float zv = sc.z[static_cast<size_t>(ty) * kTexW + static_cast<size_t>(tx)] * (1.0f + 0.01f * (g.unit() - 0.5f));
            const float w = g.unit();
            if (w < 0.05f) zv = 0.0f; else if (w < 0.10f) zv *= 2.5f;
            sc.zp[i] = zv;
        }
    }
    // The interface's coverage mask and last frame's UI colour, in 3x3 cells.
    for (int cy = 0; cy < (kH + 2) / 3; ++cy) {
        for (int cx = 0; cx < (kW + 2) / 3; ++cx) {
            const float u = g.unit();
            uint8_t byte = 0;
            const uint32_t strength = 1u + (g.next() >> 24) % 63u;
            if (u >= 0.80f && u < 0.86f) byte = static_cast<uint8_t>((strength << 2) | 1u);
            else if (u >= 0.86f && u < 0.92f) byte = static_cast<uint8_t>((strength << 2) | 2u);
            else if (u >= 0.92f) byte = static_cast<uint8_t>((strength << 2) | 3u);
            const float c0 = g.unit(), c1 = g.unit(), c2 = g.unit();
            const bool oldUi = g.unit() < 0.3f;
            for (int dy = 0; dy < 3; ++dy)
                for (int dx = 0; dx < 3; ++dx) {
                    const int x = cx * 3 + dx, y = cy * 3 + dy;
                    if (x >= kW || y >= kH) continue;
                    const size_t i = static_cast<size_t>(y) * kW + x;
                    sc.um[i] = byte;
                    if (oldUi) { sc.up[i * 4 + 0] = c0; sc.up[i * 4 + 1] = c1; sc.up[i * 4 + 2] = c2; sc.up[i * 4 + 3] = 1.0f; }
                }
        }
    }

    // The terrain records: a small rigid transform each, a few pixels of
    // motion against the camera term, distinct per index.
    sc.tr.resize(kTerrainRecords);
    for (int k = 0; k < kTerrainRecords; ++k) {
        const double t[3] = {0.8 + 0.3 * k, -0.5, 1.5 + 0.2 * k};
        const Rot rot = makeRot(0.05 + 0.01 * k, -0.02 + 0.004 * k, 0.01 * (k % 3), t);
        TerrainRec& rec = sc.tr[static_cast<size_t>(k)];
        std::memset(&rec, 0, sizeof(rec));
        for (int j = 0; j < 48; ++j) rec.key[j] = 0xC0DE0000u + static_cast<uint32_t>(k * 64 + j);
        rec.q[3] = 1.0f;
        rec.t[3] = 1.0f;   // the old gate demanded t.w == 1
        std::memcpy(rec.r, rot.r, sizeof(rec.r));
    }
    return sc;
}

// ---------------------------------------------------------------------------
// 4. The device, the outputs, one dispatch, one comparison
// ---------------------------------------------------------------------------

constexpr int kStatsSlot = 2;
const char* const kSlotName[8] = {"O (u0)", "N (u1)", "Stats (u2)", "MV (u3)", "ZC (u4)", "MK (u5)", "UN (u6)", "ML (u7)"};
const DXGI_FORMAT kSlotFormat[8] = {DXGI_FORMAT_R32G32B32A32_FLOAT, DXGI_FORMAT_R32G32B32A32_FLOAT, DXGI_FORMAT_UNKNOWN,
                                    DXGI_FORMAT_R32G32_FLOAT,       DXGI_FORMAT_R32_FLOAT,          DXGI_FORMAT_R32_FLOAT,
                                    DXGI_FORMAT_R32G32B32A32_FLOAT, DXGI_FORMAT_R32G32_FLOAT};
const UINT kSlotBpp[8] = {16, 16, 4, 8, 4, 4, 16, 8};   // bytes per pixel (per element for Stats)
constexpr int kSlotN = 1, kSlotMV = 3;

using Outputs = std::array<std::vector<uint8_t>, 8>;

struct Rig {
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<ID3D11Buffer> cb;
    ComPtr<ID3D11SamplerState> smp;
    ComPtr<ID3D11Resource> res[8], stage[8];
    ComPtr<ID3D11UnorderedAccessView> uav[8];
    std::vector<uint8_t> fill[8];   // what every output holds before each dispatch
};

ComPtr<ID3D11ShaderResourceView> makeTexSrv(Rig& R, DXGI_FORMAT fmt, int w, int h, UINT bpp, const void* data, const char* what) {
    D3D11_TEXTURE2D_DESC td{};
    td.Width = static_cast<UINT>(w);
    td.Height = static_cast<UINT>(h);
    td.MipLevels = td.ArraySize = td.SampleDesc.Count = 1;
    td.Format = fmt;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA init{data, static_cast<UINT>(w) * bpp, 0};
    ComPtr<ID3D11Texture2D> tex;
    hr(R.dev->CreateTexture2D(&td, &init, &tex), what);
    ComPtr<ID3D11ShaderResourceView> srv;
    hr(R.dev->CreateShaderResourceView(tex.Get(), nullptr, &srv), what);
    return srv;
}

void uploadScene(Rig& R, Scene& sc) {
    sc.srv[0] = makeTexSrv(R, DXGI_FORMAT_R32G32B32A32_FLOAT, kTexW, kTexH, 16, sc.s.data(), "S texture");
    sc.srv[1] = makeTexSrv(R, DXGI_FORMAT_R32G32B32A32_FLOAT, kW, kH, 16, sc.h.data(), "H texture");
    sc.srv[2] = makeTexSrv(R, DXGI_FORMAT_R32_FLOAT, kTexW, kTexH, 4, sc.z.data(), "Z texture");
    if (sc.ui) {
        sc.srv[3] = makeTexSrv(R, DXGI_FORMAT_R32_FLOAT, kW, kH, 4, sc.zp.data(), "ZP texture");
        sc.srv[4] = makeTexSrv(R, DXGI_FORMAT_R8_UNORM, kW, kH, 1, sc.um.data(), "UM texture");
        sc.srv[8] = makeTexSrv(R, DXGI_FORMAT_R32G32B32A32_FLOAT, kW, kH, 16, sc.up.data(), "UP texture");
    }
    sc.srv[9] = makeTexSrv(R, DXGI_FORMAT_R32_UINT, kTexW, kTexH, 4, sc.ti.data(), "TI texture");
    sc.srv[10] = makeTexSrv(R, DXGI_FORMAT_R32_FLOAT, kTexW, kTexH, 4, sc.tz.data(), "TZ texture");
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = static_cast<UINT>(sc.tr.size() * sizeof(TerrainRec));
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    bd.StructureByteStride = sizeof(TerrainRec);
    D3D11_SUBRESOURCE_DATA init{sc.tr.data(), 0, 0};
    ComPtr<ID3D11Buffer> buf;
    hr(R.dev->CreateBuffer(&bd, &init, &buf), "TR buffer");
    hr(R.dev->CreateShaderResourceView(buf.Get(), nullptr, &sc.srv[11]), "TR SRV");
}

void bindScene(Rig& R, const Scene& sc) {
    ID3D11ShaderResourceView* raw[12] = {};
    for (int i = 0; i < 12; ++i) raw[i] = sc.srv[i].Get();
    R.ctx->CSSetShaderResources(0, 12, raw);
}

void initRig(Rig& R, UINT cbSize) {
    D3D_FEATURE_LEVEL level{};
    hr(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &R.dev, &level, &R.ctx),
       "D3D11CreateDevice(WARP)");
    require(level >= D3D_FEATURE_LEVEL_11_0, "WARP reports feature level 11_0 or better");

    D3D11_BUFFER_DESC cbd{};
    cbd.ByteWidth = (cbSize + 15u) & ~15u;
    cbd.Usage = D3D11_USAGE_DEFAULT;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    hr(R.dev->CreateBuffer(&cbd, nullptr, &R.cb), "P constant buffer");

    D3D11_SAMPLER_DESC sd{};   // the shader's L: bilinear, clamp
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    hr(R.dev->CreateSamplerState(&sd, &R.smp), "sampler");

    for (int s = 0; s < 8; ++s) {
        if (s == kStatsSlot) {
            D3D11_BUFFER_DESC bd{};
            bd.ByteWidth = kStatsN * 4;
            bd.Usage = D3D11_USAGE_DEFAULT;
            bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
            bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
            bd.StructureByteStride = 4;
            ComPtr<ID3D11Buffer> b;
            hr(R.dev->CreateBuffer(&bd, nullptr, &b), "Stats buffer");
            hr(R.dev->CreateUnorderedAccessView(b.Get(), nullptr, &R.uav[s]), "Stats UAV");
            R.res[s] = b;
            D3D11_BUFFER_DESC sb = bd;
            sb.Usage = D3D11_USAGE_STAGING;
            sb.BindFlags = 0;
            sb.MiscFlags = 0;
            sb.StructureByteStride = 0;
            sb.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            ComPtr<ID3D11Buffer> st;
            hr(R.dev->CreateBuffer(&sb, nullptr, &st), "Stats staging");
            R.stage[s] = st;
            R.fill[s].assign(kStatsN * 4, 0);   // Stats start at zero before every dispatch
        } else {
            D3D11_TEXTURE2D_DESC td{};
            td.Width = kW;
            td.Height = kH;
            td.MipLevels = td.ArraySize = td.SampleDesc.Count = 1;
            td.Format = kSlotFormat[s];
            td.Usage = D3D11_USAGE_DEFAULT;
            td.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
            ComPtr<ID3D11Texture2D> t;
            hr(R.dev->CreateTexture2D(&td, nullptr, &t), "output texture");
            hr(R.dev->CreateUnorderedAccessView(t.Get(), nullptr, &R.uav[s]), "output UAV");
            R.res[s] = t;
            D3D11_TEXTURE2D_DESC sd2 = td;
            sd2.Usage = D3D11_USAGE_STAGING;
            sd2.BindFlags = 0;
            sd2.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            ComPtr<ID3D11Texture2D> st;
            hr(R.dev->CreateTexture2D(&sd2, nullptr, &st), "output staging");
            R.stage[s] = st;
            // A finite sentinel: a pixel the shader never wrote stays recognisable.
            const float sentinel = -123.456f;
            R.fill[s].resize(static_cast<size_t>(kW) * kH * kSlotBpp[s]);
            for (size_t o = 0; o < R.fill[s].size(); o += 4) std::memcpy(&R.fill[s][o], &sentinel, 4);
        }
    }
    R.ctx->CSSetConstantBuffers(0, 1, R.cb.GetAddressOf());
    R.ctx->CSSetSamplers(0, 1, R.smp.GetAddressOf());
    ID3D11UnorderedAccessView* uavs[8];
    for (int s = 0; s < 8; ++s) uavs[s] = R.uav[s].Get();
    R.ctx->CSSetUnorderedAccessViews(0, 8, uavs, nullptr);
}

Outputs run(Rig& R, ID3D11ComputeShader* cs, const std::vector<char>& params) {
    R.ctx->UpdateSubresource(R.cb.Get(), 0, nullptr, params.data(), 0, 0);
    for (int s = 0; s < 8; ++s) {
        const UINT pitch = s == kStatsSlot ? 0u : static_cast<UINT>(kW) * kSlotBpp[s];
        R.ctx->UpdateSubresource(R.res[s].Get(), 0, nullptr, R.fill[s].data(), pitch, 0);
    }
    R.ctx->CSSetShader(cs, nullptr, 0);
    R.ctx->Dispatch(static_cast<UINT>((kW + 7) / 8), static_cast<UINT>((kH + 7) / 8), 1);
    for (int s = 0; s < 8; ++s) R.ctx->CopyResource(R.stage[s].Get(), R.res[s].Get());
    Outputs out;
    for (int s = 0; s < 8; ++s) {
        D3D11_MAPPED_SUBRESOURCE m{};
        hr(R.ctx->Map(R.stage[s].Get(), 0, D3D11_MAP_READ, 0, &m), "map output");
        if (s == kStatsSlot) {
            out[static_cast<size_t>(s)].assign(static_cast<const uint8_t*>(m.pData), static_cast<const uint8_t*>(m.pData) + kStatsN * 4);
        } else {
            const size_t row = static_cast<size_t>(kW) * kSlotBpp[s];
            out[static_cast<size_t>(s)].resize(row * kH);
            for (int y = 0; y < kH; ++y)
                std::memcpy(out[static_cast<size_t>(s)].data() + row * static_cast<size_t>(y),
                            static_cast<const uint8_t*>(m.pData) + static_cast<size_t>(m.RowPitch) * static_cast<size_t>(y), row);
        }
        R.ctx->Unmap(R.stage[s].Get(), 0);
    }
    return out;
}

struct Diff {
    bool differs = false;
    int slot = -1;
    size_t byte = 0;   // the first differing byte
};

// The first difference, looking at the per-pixel outputs before Stats (a
// pixel is more telling than a counter).
Diff firstDiff(const Outputs& a, const Outputs& b) {
    static const int order[8] = {3, 1, 0, 4, 5, 6, 7, 2};
    Diff d;
    for (const int s : order) {
        const auto& x = a[static_cast<size_t>(s)];
        const auto& y = b[static_cast<size_t>(s)];
        if (x.size() != y.size()) { d.differs = true; d.slot = s; return d; }
        for (size_t i = 0; i < x.size(); ++i)
            if (x[i] != y[i]) { d.differs = true; d.slot = s; d.byte = i; return d; }
    }
    return d;
}

std::string pixelText(const std::vector<uint8_t>& v, int slot, size_t byte) {
    const size_t bpp = kSlotBpp[slot];
    const size_t first = byte / bpp * bpp;
    std::string s = "(";
    for (size_t k = 0; k < bpp / 4; ++k) {
        float f;
        uint32_t u;
        std::memcpy(&f, &v[first + k * 4], 4);
        std::memcpy(&u, &v[first + k * 4], 4);
        char b[64];
        std::snprintf(b, sizeof(b), "%s%.9g [0x%08X]", k ? ", " : "", static_cast<double>(f), u);
        s += b;
    }
    return s + ")";
}

std::string describe(const Outputs& a, const Outputs& b, const Diff& d) {
    char head[160];
    const auto& x = a[static_cast<size_t>(d.slot)];
    const auto& y = b[static_cast<size_t>(d.slot)];
    if (d.slot == kStatsSlot) {
        uint32_t u, v;
        std::memcpy(&u, &x[d.byte / 4 * 4], 4);
        std::memcpy(&v, &y[d.byte / 4 * 4], 4);
        std::snprintf(head, sizeof(head), "%s element %zu: %u versus %u", kSlotName[d.slot], d.byte / 4, u, v);
        return head;
    }
    const size_t px = d.byte / kSlotBpp[d.slot];
    std::snprintf(head, sizeof(head), "%s pixel (%zu, %zu): ", kSlotName[d.slot], px % kW, px / kW);
    return std::string(head) + pixelText(x, d.slot, d.byte) + " versus " + pixelText(y, d.slot, d.byte);
}

// One check: every output of the two runs is byte-identical, else the first
// differing pixel and texture.
void expectSame(const Outputs& a, const Outputs& b, const std::string& what) {
    ++g_checks;
    const Diff d = firstDiff(a, b);
    if (d.differs) die(what + "; first difference: " + describe(a, b, d));
}

// Which pixels of a per-pixel output differ (byte for byte) between two runs.
std::vector<uint8_t> diffPixels(const Outputs& a, const Outputs& b, int slot) {
    std::vector<uint8_t> out(static_cast<size_t>(kW) * kH, 0);
    const size_t bpp = kSlotBpp[slot];
    for (size_t i = 0; i < out.size(); ++i)
        if (std::memcmp(&a[static_cast<size_t>(slot)][i * bpp], &b[static_cast<size_t>(slot)][i * bpp], bpp) != 0) out[i] = 1;
    return out;
}

uint32_t statAt(const Outputs& o, int index) {
    uint32_t v;
    std::memcpy(&v, &o[kStatsSlot][static_cast<size_t>(index) * 4], 4);
    return v;
}

size_t sentinelPixels(const Rig& R, const Outputs& o, int slot) {
    size_t n = 0;
    const size_t bpp = kSlotBpp[slot];
    for (size_t i = 0; i < static_cast<size_t>(kW) * kH; ++i)
        if (std::memcmp(&o[static_cast<size_t>(slot)][i * bpp], &R.fill[slot][i * bpp], bpp) == 0) ++n;
    return n;
}

// ---------------------------------------------------------------------------
// 5. The parameters, written through cbuffer P's reflected layout
// ---------------------------------------------------------------------------

struct Cfg {
    int knobsY;    // knobs.y: 1 = depth bound
    int tvCamW;    // tvCam.w: 1 = the world (camera) path is on
    int probeW;    // probe.w bits (never 8 in the A/B)
    int splitY;    // split.y: the debug view
};

std::string cfgText(const Cfg& c) {
    char b[96];
    std::snprintf(b, sizeof(b), "knobs.y=%d tvCam.w=%d probe.w=%d split.y=%d", c.knobsY, c.tvCamW, c.probeW, c.splitY);
    return b;
}

std::vector<char> buildParams(const Layout& L, const Scene& sc, const Cfg& c) {
    std::vector<char> cb(L.size, 0);
    auto put = [&](const char* name, const void* v, UINT bytes) {
        const auto it = L.var.find(name);
        require(it != L.var.end(), std::string("cbuffer P has no variable '") + name + "'");
        require(bytes <= it->second.second && it->second.first + bytes <= cb.size(),
                std::string("cbuffer P variable '") + name + "' is smaller than the value written");
        std::memcpy(cb.data() + it->second.first, v, bytes);
    };
    auto f4 = [&](const char* name, float a, float b, float cc, float d) { const float v[4] = {a, b, cc, d}; put(name, v, 16); };
    auto i4 = [&](const char* name, int a, int b, int cc, int d) { const int v[4] = {a, b, cc, d}; put(name, v, 16); };
    auto i2 = [&](const char* name, int a, int b) { const int v[2] = {a, b}; put(name, v, 8); };
    auto i1 = [&](const char* name, int a) { put(name, &a, 4); };
    auto f1 = [&](const char* name, float a) { put(name, &a, 4); };
    auto rows = [&](const char* n0, const char* n1, const char* n2, const Rot& r) {
        f4(n0, r.r[0][0], r.r[0][1], r.r[0][2], 0.0f);
        f4(n1, r.r[1][0], r.r[1][1], r.r[1][2], 0.0f);
        f4(n2, r.r[2][0], r.r[2][1], r.r[2][2], 0.0f);
    };
    const double none[3] = {0, 0, 0};

    i4("region", sc.regX, sc.regY, sc.regX + kW, sc.regY + kH);
    i2("size", kW, kH);
    i2("texSize", kTexW, kTexH);
    f4("tanNow", kTanNow[0], kTanNow[1], kTanNow[2], kTanNow[3]);
    f4("tanPrev", kTanPrev[0], kTanPrev[1], kTanPrev[2], kTanPrev[3]);
    f4("jit", 0.31f, -0.18f, 1.0f, 0.75f);         // sub-pixel jitter, filter on, a sharp bicubic
    rows("dR0", "dR1", "dR2", makeRot(0.012, -0.007, 0.004, none));   // the head's delta: not the identity
    rows("c0R0", "c0R1", "c0R2", makeRot(0.010, -0.006, 0.002, none));
    rows("c1R0", "c1R1", "c1R2", makeRot(0.014, -0.008, 0.005, none));
    rows("c2R0", "c2R1", "c2R2", makeRot(0.031, 0.011, -0.003, none)); // the camera's delta (the world path's rows)
    rows("c3R0", "c3R1", "c3R2", makeRot(0.012, -0.007, 0.004, none));
    f1("blend", 0.9f);
    f1("gamma", 1.25f);
    i1("haveHistory", 1);
    i1("candMask", 15);                              // the registration instrument's four candidates
    f4("knobs", sc.depthA, static_cast<float>(c.knobsY), kDepthB, 1000.0f);
    f4("tvUsed", 0.012f, -0.007f, 0.020f, 1.0f);     // non-trivial head translation; w = 1: main uses depth
    f4("tvCand", -0.030f, 0.002f, 0.010f, 0.0f);
    f4("tvCam", 0.35f, -0.12f, 0.90f, static_cast<float>(c.tvCamW));
    f4("split", kSplitMetres, static_cast<float>(c.splitY), 0.0f, 0.0f);
    f4("fovea0", 0, 0, 0, 0);                        // fovea off, skip empty
    f4("fovea1", 0, 0, 0, 0);
    f4("movers", sc.ui ? 1.0f : 0.0f, 0.05f, 0.8f, 1.0f);   // w = 1: main writes ZC
    f4("probe", 1.0f, 1.0f, sc.ui ? 1.0f : 0.0f, static_cast<float>(c.probeW));
    f4("holoJitter", sc.ui ? 0.15f : 0.0f, sc.ui ? -0.10f : 0.0f, sc.ui ? 1.0f : 0.0f, sc.ui ? 1.0f : 0.0f);
    f4("skip", 0, 0, 0, 0);
    f4("lead", 0, 0, 0, 0);
    return cb;
}

std::vector<Cfg> makeConfigs(const Scene& sc, bool isMain) {
    std::vector<int> bits = {0, 1, 2, 128};     // bit 8 is clear in every one
    if (sc.ui) { bits.push_back(4 | 2); bits.push_back(4 | 2 | 1); bits.push_back(4 | 128); }
    const std::vector<int> views = isMain ? std::vector<int>{1, 2, 3, 6} : std::vector<int>{1, 3, 6};
    std::vector<Cfg> v;
    for (int ky : {1, 0})
        for (int tw : {1, 0}) {
            for (int pw : bits) v.push_back({ky, tw, pw, 0});
            for (int sy : views) v.push_back({ky, tw, 0, sy});
        }
    return v;
}

// The old gate, on the CPU: terrainPixel() with bit 8 set admits a pixel when
// the depth is bound, TI is in 1..512, the interface does not cover it, TZ is
// a real depth and equals the scene's to a part in 1e5, and the record is
// valid. (The behind-the-eye test cannot fail for these records.)
bool oldGateAdmits(const Scene& sc, int px, int py, int knobsY) {
    const size_t q = sc.texIndex(px, py);
    const uint32_t index = sc.ti[q];
    if (knobsY == 0 || index == 0 || index > 512) return false;
    if (sc.ui && (sc.um[static_cast<size_t>(py) * kW + static_cast<size_t>(px)] & 1u) != 0) return false;   // uiCovered
    const float zraw = sc.tz[q], scene = sc.z[q];
    if (zraw <= sc.depthA || std::fabs(scene - zraw) > std::fabs(zraw) * 0.00001f) return false;
    return sc.tr[index - 1].t[3] == 1.0f;
}

// What terrainPixel() computes for an admitted pixel, in double: the motion
// (previous - current, render pixels) the mv entry writes.
void oldTerrainMotion(const Scene& sc, int px, int py, double& mx, double& my) {
    const size_t q = sc.texIndex(px, py);
    const TerrainRec& rec = sc.tr[sc.ti[q] - 1];
    const double tn[4] = {kTanNow[0], kTanNow[1], kTanNow[2], kTanNow[3]};
    const double tp[4] = {kTanPrev[0], kTanPrev[1], kTanPrev[2], kTanPrev[3]};
    const double dx = tn[0] + (px + 0.5) / kW * (tn[1] - tn[0]);
    const double dy = tn[3] - (py + 0.5) / kH * (tn[3] - tn[2]);
    const double z = static_cast<double>(kDepthB) / (static_cast<double>(sc.tz[q]) - static_cast<double>(sc.depthA));
    const double here[4] = {dx * z, dy * z, z, 1.0};
    double before[3];
    for (int i = 0; i < 3; ++i) {
        before[i] = 0;
        for (int j = 0; j < 4; ++j) before[i] += static_cast<double>(rec.r[i][j]) * here[j];
    }
    const double ppx = (before[0] / before[2] - tp[0]) / (tp[1] - tp[0]) * kW - 0.5;
    const double ppy = (tp[3] - before[1] / before[2]) / (tp[3] - tp[2]) * kH - 0.5;
    mx = ppx - px;
    my = ppy - py;
}

// ---------------------------------------------------------------------------
// 6. The A/B matrix, the control and the mutation
// ---------------------------------------------------------------------------

struct Shaders {
    ComPtr<ID3D11ComputeShader> oldCs, newCs;
};

// Old reference (bit 8 clear) against new, over every configuration of a scene.
void abMatrix(Rig& R, const char* label, Scene& sc, bool isMain, bool diagnostics, const Shaders& sh, const Layout& L) {
    bindScene(R, sc);
    const std::vector<Cfg> cfgs = makeConfigs(sc, isMain);
    const auto t0 = Clock::now();
    int moverSeen = 0;
    std::string facts;   // what the first configuration's counters say the shader did
    for (const Cfg& c : cfgs) {
        require((c.probeW & 8) == 0, "the A/B runs with probe.w bit 8 clear");
        const std::vector<char> params = buildParams(L, sc, c);
        const Outputs a = run(R, sh.newCs.Get(), params);
        const Outputs a2 = run(R, sh.newCs.Get(), params);
        const Outputs o = run(R, sh.oldCs.Get(), params);
        const std::string where = std::string(label) + " scene " + sc.name + " [" + cfgText(c) + "]";
        // The harness must see the shader work: every pixel of the entry's own
        // image output was written.
        const int imageSlot = isMain ? kSlotN : kSlotMV;
        check(sentinelPixels(R, a, imageSlot) == 0, where + ": the new shader left pixels of " + kSlotName[imageSlot] + " unwritten");
        // WARP must agree with itself before an A/B means anything.
        expectSame(a, a2, where + ": WARP is not deterministic, one shader and one input gave two results");
        // The claim.
        expectSame(o, a, where + ": OLD (bit 8 clear) and NEW differ");
        if (diagnostics) {
            // The camera/world term ran exactly where the configuration asks for it.
            const bool world = c.knobsY == 1 && c.tvCamW == 1;
            check((statAt(a, 15) > 0) == world, where + ": the world path's pixel count (Stats[15] = " + std::to_string(statAt(a, 15)) +
                                                     ") does not match what the configuration asks for");
            if (sc.ui && statAt(a, 28) > 0) ++moverSeen;
            if (facts.empty()) {   // the first configuration: knobs.y 1, tvCam.w 1, probe.w 0
                char b[256];
                const unsigned probes = statAt(a, 20) + statAt(a, 23) + statAt(a, 32);
                if (isMain)
                    std::snprintf(b, sizeof(b), "       first configuration: %u of %d pixels on the world path, history refused on %u, clipped on %u, "
                                                "%u bright, %u registration probes",
                                  statAt(a, 15), kW * kH, statAt(a, 0), statAt(a, 1), statAt(a, 16), probes);
                else
                    std::snprintf(b, sizeof(b), "       first configuration: %u of %d pixels on the world path, mover mask on %u, %u bright, "
                                                "%u registration probes",
                                  statAt(a, 15), kW * kH, statAt(a, 28), statAt(a, 16), probes);
                facts = b;
            }
        }
    }
    if (diagnostics && sc.ui)
        check(moverSeen > 0, std::string(label) + " scene " + sc.name + ": the mover mask never fired (Stats[28] stayed 0)");
    std::printf("  %-10s scene %-2s %3zu configs x (NEW, NEW again, OLD) x 8 outputs: OLD == NEW bit for bit  [%.1f s]\n",
                label, sc.name, cfgs.size(), secondsSince(t0));
    if (!facts.empty()) std::printf("%s\n", facts.c_str());
    std::fflush(stdout);
}

// The old shader with bit 8 set: the control that the harness can see terrain.
void terrainControl(Rig& R, Scene& sc, bool isMain, const Shaders& sh, const Layout& L) {
    bindScene(R, sc);
    const char* entry = isMain ? "main" : "mv";
    size_t movedMin = SIZE_MAX, movedMax = 0, admittedMin = SIZE_MAX, admittedMax = 0, quiet = 0, motionChecked = 0;
    double worst = 0.0;
    for (int tw : {1, 0}) {
        for (int base : {0, 1}) {
            const Cfg off{1, tw, base, 0}, on{1, tw, base | 8, 0};
            const std::vector<char> pOff = buildParams(L, sc, off), pOn = buildParams(L, sc, on);
            const Outputs a = run(R, sh.oldCs.Get(), pOff);
            const Outputs b = run(R, sh.oldCs.Get(), pOn);
            const std::string where = std::string("control ") + entry + " scene " + sc.name + " tvCam.w=" + std::to_string(tw) +
                                      " probe.w " + std::to_string(base) + " -> " + std::to_string(base | 8);
            // Per-pixel outputs that differ between bit 8 clear and set.
            std::vector<uint8_t> anyDiff(static_cast<size_t>(kW) * kH, 0);
            for (int s = 0; s < 8; ++s) {
                if (s == kStatsSlot) continue;
                const std::vector<uint8_t> dp = diffPixels(a, b, s);
                for (size_t i = 0; i < dp.size(); ++i) anyDiff[i] |= dp[i];
            }
            size_t moved = 0, admitted = 0, quietHere = 0;
            std::string stray;
            for (int y = 0; y < kH; ++y)
                for (int x = 0; x < kW; ++x) {
                    const size_t i = static_cast<size_t>(y) * kW + static_cast<size_t>(x);
                    const bool admits = oldGateAdmits(sc, x, y, 1);
                    const bool hasTerrain = sc.ti[sc.texIndex(x, y)] != 0;
                    if (admits) ++admitted;
                    if (anyDiff[i]) {
                        ++moved;
                        if (stray.empty() && !(hasTerrain && admits))
                            stray = "pixel (" + std::to_string(x) + ", " + std::to_string(y) + ") changed with TI " +
                                    (hasTerrain ? "!= 0 but the old gate does not admit it" : "== 0");
                    } else if (!hasTerrain) {
                        ++quietHere;
                    }
                }
            // Only terrain pixels the old gate admits changed; every TI == 0
            // pixel is byte-identical in all seven per-pixel outputs.
            check(stray.empty(), where + ": " + stray);
            check(moved > 0, where + ": bit 8 set changed nothing, so this harness cannot see the terrain path");
            // Every admitted pixel moved, where nothing can hide the change
            // (the mv entry's hidden-history sentinel, in the scene that has
            // DLSS history, can mask an admitted pixel's vector; main can
            // land both histories off the image).
            if (!isMain && !sc.ui)
                check(moved == admitted, where + ": " + std::to_string(admitted) + " pixels are admitted but " + std::to_string(moved) + " changed");
            if (!isMain) {
                // And they moved to where the old function says: the CPU model of terrainPixel().
                const float hiddenSentinel = static_cast<float>(kW) * 2.0f;   // size*2: NVIDIA's no-history vector
                size_t modelled = 0;
                double worstHere = 0.0;
                for (int y = 0; y < kH; ++y)
                    for (int x = 0; x < kW; ++x) {
                        if (!oldGateAdmits(sc, x, y, 1)) continue;
                        float mv[2];
                        std::memcpy(mv, &b[kSlotMV][(static_cast<size_t>(y) * kW + static_cast<size_t>(x)) * 8], 8);
                        if (mv[0] == hiddenSentinel) continue;
                        double ex, ey;
                        oldTerrainMotion(sc, x, y, ex, ey);
                        worstHere = std::max(worstHere, std::max(std::fabs(static_cast<double>(mv[0]) - ex), std::fabs(static_cast<double>(mv[1]) - ey)));
                        ++modelled;
                    }
                check(modelled > 0 && worstHere < 2e-3, where + ": the terrain vectors differ from the CPU model of terrainPixel by " +
                                                              std::to_string(worstHere) + " px over " + std::to_string(modelled) + " pixels");
                motionChecked += modelled;
                worst = std::max(worst, worstHere);
            }
            // The new shader ignores the retired bit: bit 8 set equals bit 8 clear.
            const Outputs n0 = run(R, sh.newCs.Get(), pOff);
            const Outputs n1 = run(R, sh.newCs.Get(), pOn);
            expectSame(n0, n1, where + ": the NEW shader reacts to probe.w bit 8");
            movedMin = std::min(movedMin, moved);
            movedMax = std::max(movedMax, moved);
            admittedMin = std::min(admittedMin, admitted);
            admittedMax = std::max(admittedMax, admitted);
            quiet += quietHere;
        }
    }
    std::printf("  control %-4s scene %-2s old shader, probe.w 0/1 -> 8/9, tvCam.w 1/0: %zu..%zu pixels differ per run (%zu..%zu admitted by the old gate), "
                "none with TI == 0 (%zu such pixel-runs byte-identical); NEW ignores the bit",
                entry, sc.name, movedMin, movedMax, admittedMin, admittedMax, quiet);
    if (!isMain) std::printf("; %zu terrain vectors match the CPU model (worst %.2g px)", motionChecked, worst);
    std::printf("\n");
    std::fflush(stdout);
}

// A broken camera term must not pass: the mutated new text against the
// reference, over every configuration of every scene.
void mutationControl(Rig& R, std::vector<Scene>& scenes, const Mutation& m, bool isMain, const Shaders& sh, const Layout& L) {
    int runs = 0, caught = 0, idle = 0, idleCaught = 0;
    std::string first;
    for (Scene& sc : scenes) {
        bindScene(R, sc);
        for (const Cfg& c : makeConfigs(sc, isMain)) {
            const std::vector<char> params = buildParams(L, sc, c);
            const Outputs o = run(R, sh.oldCs.Get(), params);
            const Outputs x = run(R, sh.newCs.Get(), params);
            const Diff d = firstDiff(o, x);
            // The mutated line runs only with depth bound (and, for the world
            // path's, with the world path on).
            const bool lineRuns = c.knobsY == 1 && (!m.needsWorld || c.tvCamW == 1);
            if (lineRuns) { ++runs; if (d.differs) ++caught; }
            else { ++idle; if (d.differs) ++idleCaught; }
            if (d.differs && first.empty()) first = std::string("scene ") + sc.name + " [" + cfgText(c) + "] " + describe(o, x, d);
        }
    }
    check(runs > 0 && caught == runs, std::string("mutation '") + m.name + "' slipped through the A/B in " + std::to_string(runs - caught) +
                                          " of " + std::to_string(runs) + " configurations where the mutated line runs");
    check(idleCaught == 0, std::string("mutation '") + m.name + "' showed in " + std::to_string(idleCaught) +
                               " configurations where the mutated line never runs");
    std::printf("  mutation %-50s caught in %d of %d configurations where the line runs; the other %d stay identical\n"
                "      first: %s\n",
                m.name, caught, runs, idle, first.c_str());
    std::fflush(stdout);
}

}  // namespace

int main(int argc, char** argv) {
    bool dryRun = false;
    const char* verifyOld = nullptr;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--self-test") {
        } else if (a == "--dry-run") {
            dryRun = true;
        } else if (a == "--verify-old" && i + 1 < argc) {
            verifyOld = argv[++i];
        } else {
            std::fprintf(stderr, "usage: terrain_retired_test [--self-test | --dry-run | --verify-old <old temporal_shader_source.h>]\n"
                                 "run from the repository root\n");
            return 2;
        }
    }
    const auto t0 = Clock::now();

    std::string source, newText, why;
    if (!readFileLf(kShaderPath, source)) die(std::string(kShaderPath) + " is not readable (run from the repository root)");
    if (!extractHlsl(source, newText, why)) die(std::string(kShaderPath) + ": " + why);

    if (verifyOld) {
        std::string oldSource, oldText;
        if (!readFileLf(verifyOld, oldSource)) die(std::string(verifyOld) + " is not readable");
        if (!extractHlsl(oldSource, oldText, why)) die(std::string(verifyOld) + ": " + why);
        Rebuilt rb;
        if (!rebuildReference(newText, true, rb, why)) die(why);
        const bool same = rb.text == oldText;
        std::printf("verify-old: OLD  %s\n            %zu chars, FNV-1a-64 %016llx\n", verifyOld, oldText.size(),
                    static_cast<unsigned long long>(fnv1a64(oldText)));
        std::printf("verify-old: NEW  %s rebuilt with %d fragments (%d comment-only)\n            %zu chars, FNV-1a-64 %016llx\n",
                    kShaderPath, rb.applied, rb.cosmetic, rb.text.size(), static_cast<unsigned long long>(fnv1a64(rb.text)));
        if (!same) {
            size_t at = 0;
            while (at < rb.text.size() && at < oldText.size() && rb.text[at] == oldText[at]) ++at;
            const auto excerpt = [&](const std::string& t) {
                std::string e = t.substr(at > 40 ? at - 40 : 0, 120);
                std::replace(e.begin(), e.end(), '\n', '|');
                return e;
            };
            std::fprintf(stderr, "FAIL: the rebuilt reference differs from the old file at offset %zu\n  rebuilt: %s\n  old:     %s\n", at,
                         excerpt(rb.text).c_str(), excerpt(oldText).c_str());
            return 1;
        }
        std::printf("verify-old: the reference rebuilt from the current text is byte-identical to the old file\n");
        return 0;
    }

    Rebuilt ref;
    if (!rebuildReference(newText, false, ref, why)) die(why);
    std::puts(dryRun ? "Terrain retirement A/B (text only)" : "Terrain retirement A/B (WARP)");
    std::printf("  text: NEW %s, %zu chars; reference rebuilt from %d fragments (%d comment-only, %d skipped), %zu chars\n", kShaderPath,
                newText.size(), ref.applied, ref.cosmetic, ref.skipped, ref.text.size());
    check(ref.text.find("terrainPixel") != std::string::npos && newText.find("terrainPixel") == std::string::npos,
          "the reference carries terrainPixel and the new text does not");
    if (dryRun) {
        std::printf("Terrain retirement A/B: %u checks passed (--dry-run: text only)\n", g_checks);
        return 0;
    }

    // The mutated texts.
    std::string mutatedText[kMutationCount];
    for (int m = 0; m < kMutationCount; ++m)
        if (!mutate(newText, kMutations[m], mutatedText[m], why)) die(why);

    // Every compile at once: the main entry alone is ~20 s in fxc.
    struct Job { const char* label; const std::string* text; const char* sourceName; const char* entry; bool diag; Compiled res; };
    enum { kOldMvD, kNewMvD, kOldMainD, kNewMainD, kOldMvF, kNewMvF, kOldMainF, kNewMainF, kMutBase, kJobs = kMutBase + kMutationCount };
    std::vector<Job> jobs(kJobs);
    jobs[kOldMvD] = {"OLD mv (diagnostics)", &ref.text, "temporal_mv_cs", "mv", true, {}};
    jobs[kNewMvD] = {"NEW mv (diagnostics)", &newText, "temporal_mv_cs", "mv", true, {}};
    jobs[kOldMainD] = {"OLD main (diagnostics)", &ref.text, "temporal_aa_cs", "main", true, {}};
    jobs[kNewMainD] = {"NEW main (diagnostics)", &newText, "temporal_aa_cs", "main", true, {}};
    jobs[kOldMvF] = {"OLD mv (fast)", &ref.text, "temporal_mv_fast_cs", "mv", false, {}};
    jobs[kNewMvF] = {"NEW mv (fast)", &newText, "temporal_mv_fast_cs", "mv", false, {}};
    jobs[kOldMainF] = {"OLD main (fast)", &ref.text, "temporal_aa_fast_cs", "main", false, {}};
    jobs[kNewMainF] = {"NEW main (fast)", &newText, "temporal_aa_fast_cs", "main", false, {}};
    for (int m = 0; m < kMutationCount; ++m)
        jobs[static_cast<size_t>(kMutBase + m)] = {kMutations[m].name, &mutatedText[m],
                                                   std::strcmp(kMutations[m].entry, "mv") == 0 ? "temporal_mv_cs" : "temporal_aa_cs",
                                                   kMutations[m].entry, true, {}};
    {
        std::vector<std::thread> pool;
        for (Job& j : jobs) pool.emplace_back(compileOne, j.text, j.sourceName, j.entry, j.diag, &j.res);
        for (auto& t : pool) t.join();
    }
    double slowest = 0.0;
    const char* slowestName = "";
    for (const Job& j : jobs) {
        if (FAILED(j.res.hr) || !j.res.code) die(std::string(j.label) + " did not compile (0x" + hexOf(j.res.hr) + "):\n" + j.res.log);
        if (j.res.seconds > slowest) { slowest = j.res.seconds; slowestName = j.label; }
    }
    ++g_checks;
    std::printf("  compile: %d shaders (cs_5_0, flags 0, production macros), one thread each, in %.1f s (slowest: %s, %.1f s)\n", kJobs,
                secondsSince(t0), slowestName, slowest);
    std::printf("           seconds OLD/NEW: mv %.1f/%.1f, main %.1f/%.1f (diagnostics); mv %.1f/%.1f, main %.1f/%.1f (fast); mutated mv %.1f %.1f, main %.1f %.1f\n",
                jobs[kOldMvD].res.seconds, jobs[kNewMvD].res.seconds, jobs[kOldMainD].res.seconds, jobs[kNewMainD].res.seconds,
                jobs[kOldMvF].res.seconds, jobs[kNewMvF].res.seconds, jobs[kOldMainF].res.seconds, jobs[kNewMainF].res.seconds,
                jobs[kMutBase].res.seconds, jobs[kMutBase + 1].res.seconds, jobs[kMutBase + 2].res.seconds, jobs[kMutBase + 3].res.seconds);
    std::printf("           bytecode bytes OLD -> NEW: mv %zu -> %zu, main %zu -> %zu (diagnostics); mv %zu -> %zu, main %zu -> %zu (fast)\n",
                jobs[kOldMvD].res.code->GetBufferSize(), jobs[kNewMvD].res.code->GetBufferSize(),
                jobs[kOldMainD].res.code->GetBufferSize(), jobs[kNewMainD].res.code->GetBufferSize(),
                jobs[kOldMvF].res.code->GetBufferSize(), jobs[kNewMvF].res.code->GetBufferSize(),
                jobs[kOldMainF].res.code->GetBufferSize(), jobs[kNewMainF].res.code->GetBufferSize());
    std::fflush(stdout);

    // Reflection: the reference binds the terrain resources and the new text
    // binds nothing in t9..t11; cbuffer P is the same in both.
    std::vector<Layout> layout(kJobs);
    for (size_t j = 0; j < jobs.size(); ++j)
        if (!reflectLayout(jobs[j].res.code.Get(), layout[j], why)) die(std::string(jobs[j].label) + ": " + why);
    const std::pair<int, int> pairs[] = {{kOldMvD, kNewMvD}, {kOldMainD, kNewMainD}, {kOldMvF, kNewMvF}, {kOldMainF, kNewMainF}};
    for (const std::pair<int, int>& pair : pairs) {
        const Job& o = jobs[static_cast<size_t>(pair.first)];
        const Job& n = jobs[static_cast<size_t>(pair.second)];
        const auto oldBind = srvsInRange(o.res.code.Get(), 9, 11);
        const auto newBind = srvsInRange(n.res.code.Get(), 9, 11);
        check(oldBind.size() == 3, std::string(o.label) + " should declare TI, TZ and TR at t9..t11, found " + joined(oldBind));
        check(newBind.empty(), std::string(n.label) + " should declare nothing at t9..t11, found " + joined(newBind));
        check(layout[static_cast<size_t>(pair.first)] == layout[static_cast<size_t>(pair.second)],
              std::string("cbuffer P differs between ") + o.label + " and " + n.label);
    }
    std::printf("  reflect: the reference binds %s at t9..t11, NEW binds nothing there (all four variants); cbuffer P identical (%zu variables, %u bytes)\n",
                joined(srvsInRange(jobs[kOldMvD].res.code.Get(), 9, 11)).c_str(), layout[kOldMvD].var.size(), layout[kOldMvD].size);

    Rig R;
    initRig(R, layout[kNewMainD].size);
    auto make = [&](int job) {
        ComPtr<ID3D11ComputeShader> cs;
        hr(R.dev->CreateComputeShader(jobs[static_cast<size_t>(job)].res.code->GetBufferPointer(),
                                      jobs[static_cast<size_t>(job)].res.code->GetBufferSize(), nullptr, &cs),
           jobs[static_cast<size_t>(job)].label);
        return cs;
    };
    const Shaders mvD{make(kOldMvD), make(kNewMvD)}, mainD{make(kOldMainD), make(kNewMainD)};
    const Shaders mvF{make(kOldMvF), make(kNewMvF)}, mainF{make(kOldMainF), make(kNewMainF)};
    Shaders mutated[kMutationCount];
    for (int m = 0; m < kMutationCount; ++m) {
        const bool isMain = std::strcmp(kMutations[m].entry, "main") == 0;
        mutated[m] = Shaders{isMain ? mainD.oldCs : mvD.oldCs, make(kMutBase + m)};   // against the same reference shader
    }

    std::vector<Scene> scenes;
    scenes.push_back(makeScene("A", 0x1234u, 3, 5, false, 0.0f));
    scenes.push_back(makeScene("B", 0xBEEFu, 3, 5, true, 2.0e-5f));
    scenes.push_back(makeScene("C", 0x5EEDu, 11, 9, false, 1.0e-5f));
    for (Scene& sc : scenes) {
        uploadScene(R, sc);
        size_t terrain = 0, gate = 0;
        for (int y = 0; y < kH; ++y)
            for (int x = 0; x < kW; ++x) {
                if (sc.ti[sc.texIndex(x, y)] != 0) ++terrain;
                if (oldGateAdmits(sc, x, y, 1)) ++gate;
            }
        check(terrain * 10 > static_cast<size_t>(kW) * kH * 4 && gate * 10 > static_cast<size_t>(kW) * kH * 2,
              std::string("scene ") + sc.name + " has too few terrain-like pixels");
        std::printf("  scene %s: seed 0x%X, %dx%d region at (%d, %d) in a %dx%d texture, knobs.x %g; TI != 0 on %zu of %zu pixels, "
                    "the old gate admits %zu%s\n",
                    sc.name, sc.seed, kW, kH, sc.regX, sc.regY, kTexW, kTexH, static_cast<double>(sc.depthA), terrain,
                    static_cast<size_t>(kW) * kH, gate, sc.ui ? "; UM, UP, ZP bound, mover mask on, DLSS depth history valid" : "");
    }
    std::fflush(stdout);

    // A/B: the claim.
    for (Scene& sc : scenes) abMatrix(R, "mv", sc, false, true, mvD, layout[kNewMvD]);
    for (Scene& sc : scenes) abMatrix(R, "main", sc, true, true, mainD, layout[kNewMainD]);
    for (Scene& sc : scenes) abMatrix(R, "mv fast", sc, false, false, mvF, layout[kNewMvF]);
    for (Scene& sc : scenes) abMatrix(R, "main fast", sc, true, false, mainF, layout[kNewMainF]);

    // Control: the harness can see the terrain path when the bit is set.
    for (Scene& sc : scenes) terrainControl(R, sc, false, mvD, layout[kNewMvD]);
    for (Scene& sc : scenes) terrainControl(R, sc, true, mainD, layout[kNewMainD]);

    // Control: a broken camera term does not pass.
    for (int m = 0; m < kMutationCount; ++m) {
        const bool isMain = std::strcmp(kMutations[m].entry, "main") == 0;
        mutationControl(R, scenes, kMutations[m], isMain, mutated[m], layout[isMain ? kNewMainD : kNewMvD]);
    }

    std::printf("Terrain retirement A/B: %u checks passed (%.1f s)\n", g_checks, secondsSince(t0));
    return 0;
}
