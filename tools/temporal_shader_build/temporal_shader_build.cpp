// Compile fixed temporal shaders during the build, never in the game -- and, with --stereo-output, the native runtime's four stereo-renderer shaders
// (src/openxr/stereo_shader_source.h) into a header of their own, so the runtime DLL carries no compiler.
#include <windows.h>
#include <d3dcompiler.h>
#include <d3d11shader.h>
#include <wrl/client.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#include "../../src/d3d11/temporal_shader_source.h"
#include "../../src/d3d11/flat_mono_shader_source.h"
#include "../../src/d3d11/engine_velocity_primary_copy_shader.h"
#include "../../src/d3d11/fixed_shader_source.h"
#include "../../src/d3d11/ui_resolve.h"
#include "../../src/d3d11/night_vision_shader.h"
#include "../../src/d3d11/stellar_coverage.h"
#include "../../src/openxr/stereo_shader_source.h"

namespace fs = std::filesystem;
using Microsoft::WRL::ComPtr;
using CompileFn = decltype(&D3DCompile);

static fs::path compilerPath() {
    wchar_t dir[MAX_PATH]{};
    const UINT n = GetSystemDirectoryW(dir, MAX_PATH);
    if (!n || n >= MAX_PATH) throw std::runtime_error("system directory unavailable");
    return fs::path(dir) / L"d3dcompiler_47.dll";
}

struct Compiler {
    HMODULE module = nullptr;
    CompileFn fn = nullptr;
    Compiler() {
        const fs::path path = compilerPath();
        module = LoadLibraryW(path.c_str());
        if (module) fn = reinterpret_cast<CompileFn>(GetProcAddress(module, "D3DCompile"));
        if (!fn) {
            if (module) FreeLibrary(module);
            throw std::runtime_error("system d3dcompiler_47.dll/D3DCompile unavailable");
        }
    }
    Compiler(const Compiler&) = delete;
    Compiler& operator=(const Compiler&) = delete;
    ~Compiler() { FreeLibrary(module); }
};

// --stereo-output names the second header (the runtime's stereo shaders); it is generated beside --output, never alone.
struct Options { bool selfTest = false, dry = false; fs::path output; fs::path stereoOutput; };
static bool parse(int argc, const wchar_t* const* argv, Options& o) {
    o = {};
    bool outputSeen = false, stereoSeen = false;
    for (int i = 1; i < argc; ++i) {
        if (!wcscmp(argv[i], L"--self-test") && !o.selfTest) o.selfTest = true;
        else if (!wcscmp(argv[i], L"--dry-run") && !o.dry) o.dry = true;
        else if (!wcscmp(argv[i], L"--output") && !outputSeen && i + 1 < argc) {
            const wchar_t* value = argv[++i];
            if (!*value || !wcsncmp(value, L"--", 2)) return false;
            o.output = value;
            outputSeen = true;
        } else if (!wcscmp(argv[i], L"--stereo-output") && !stereoSeen && i + 1 < argc) {
            const wchar_t* value = argv[++i];
            if (!*value || !wcsncmp(value, L"--", 2)) return false;
            o.stereoOutput = value;
            stereoSeen = true;
        } else return false;
    }
    if (stereoSeen && !outputSeen) return false;
    return o.selfTest ? !outputSeen && !o.dry && !stereoSeen : outputSeen;
}

struct Variant {
    const char* symbol;
    const char* sourceName;
    const char* entry;
    const D3D_SHADER_MACRO* macros;
    std::vector<unsigned char> bytes;
    bool flat = false;
    const char* alternate = nullptr;
    const char* profile = "cs_5_0";
    UINT flags1 = 0, flags2 = 0;
};

struct LegacyContract {
    const char* sourceName;
    const char* entry;
    const char* profile;
    const D3D_SHADER_MACRO* macros;
    uint64_t sourceHash;
};
#include "fixed_core_shader_variants.h"
#include "fixed_extra_shader_variants.h"

static bool compile(CompileFn fn, const char* source, Variant& v, bool quiet = false) {
    v.bytes.clear();
    ComPtr<ID3DBlob> code, errors;
    const ULONGLONG start = GetTickCount64();
    const HRESULT hr = fn(source, std::strlen(source), v.sourceName, v.macros,
                          nullptr, v.entry, v.profile, v.flags1, v.flags2, &code, &errors);
    if (errors && !quiet) {
        const int size = static_cast<int>(std::min<SIZE_T>(errors->GetBufferSize(), 4096));
        std::fprintf(stderr, "%s: %.*s\n", v.sourceName, size,
                     static_cast<const char*>(errors->GetBufferPointer()));
    }
    if (FAILED(hr) || !code || !code->GetBufferPointer() || !code->GetBufferSize()) {
        if (!quiet) std::fprintf(stderr, "%s: compile failed (0x%08X)\n", v.sourceName, unsigned(hr));
        return false;
    }
    const auto* begin = static_cast<const unsigned char*>(code->GetBufferPointer());
    v.bytes.assign(begin, begin + code->GetBufferSize());
    if (!quiet) std::printf("%s: %zu bytes, compiled in %llu ms\n", v.sourceName,
                            v.bytes.size(), GetTickCount64() - start);
    return true;
}

// The AA variant alone takes ~19 s in fxc (its optimizer reports that it did
// not converge), paid on every build for a shader that changes rarely. So the
// header carries a key over everything that decides its bytes -- the HLSL, the
// variant table, the profile and flags, and the system compiler DLL itself --
// and an output whose key still matches is reused instead of recompiled. Any
// source edit changes the key, so stale bytecode cannot survive one, and
// --clean removes the header outright. Bump the tag when render() changes.
// (/2, 2026-09-23: the header also carries kEngineMotionCoreHlsl.)
static const char kKeyTag[] = "edvr-temporal-shader-key/4";
static const char kKeyPrefix[] = "// key: ";

struct Fnv1a64 {
    unsigned long long hash = 14695981039346656037ull;
    void add(const void* data, size_t size) {
        const auto* bytes = static_cast<const unsigned char*>(data);
        for (size_t i = 0; i < size; ++i) { hash ^= bytes[i]; hash *= 1099511628211ull; }
    }
    void add(const char* text) {
        add(text ? text : "", text ? std::strlen(text) : 0);
        add("", 1);  // a terminator, so adjacent strings cannot re-split the same way
    }
};

static std::string sourceKey(const char* source, const std::vector<Variant>& variants,
                             const fs::path& compiler) {
    Fnv1a64 key;
    key.add(kKeyTag);
    key.add(source);
    for (const auto& v : variants) {
        key.add(v.symbol);
        key.add(v.sourceName);
        key.add(v.entry);
        key.add(v.profile);
        key.add(&v.flags1, sizeof(v.flags1));
        key.add(&v.flags2, sizeof(v.flags2));
        key.add(v.alternate);
        key.add(v.flat ? "flat" : "stereo");
        for (const D3D_SHADER_MACRO* m = v.macros; m && m->Name; ++m) {
            key.add(m->Name);
            key.add(m->Definition);
        }
        key.add("");  // closes this variant's macro list
    }
    std::ifstream dll(compiler, std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(dll)), {});
    if (bytes.empty()) throw std::runtime_error("cannot read the shader compiler DLL for the source key");
    key.add(bytes.data(), bytes.size());
    char hex[17];
    std::snprintf(hex, sizeof hex, "%016llx", key.hash);
    return hex;
}

// True only when the output exists and its second line carries exactly this key.
static bool outputCurrent(const fs::path& output, const std::string& key) {
    std::ifstream existing(output, std::ios::binary);
    std::string first, second;
    if (!std::getline(existing, first) || !std::getline(existing, second)) return false;
    return second == kKeyPrefix + key;
}

// The engine-motion core (temporal_shader_source.h, between the two
// ENGINE_MOTION_CORE markers inside the ENGINE_MOTION_HLSL block): the pure
// arithmetic -- no resource -- that the on-foot screen shader is compiled with
// in front of it (screen_motion.cpp). One of each marker, in order, and no
// register binding inside, or the build fails here rather than the screen
// shader at runtime.
static const char kCoreBegin[] = "// ENGINE_MOTION_CORE_BEGIN\n";
static const char kCoreEnd[] = "// ENGINE_MOTION_CORE_END";
static const char kCoreDelimiter[] = "EDVRCORE";
static std::string extractCore(const std::string& source) {
    const size_t b = source.find(kCoreBegin), e = source.find(kCoreEnd);
    if (b == std::string::npos || e == std::string::npos || e < b ||
        source.find(kCoreBegin, b + 1) != std::string::npos || source.find(kCoreEnd, e + 1) != std::string::npos)
        throw std::runtime_error("the ENGINE_MOTION_CORE markers must appear exactly once each, in order");
    const size_t from = b + std::strlen(kCoreBegin);
    std::string core = source.substr(from, e - from);
    if (core.find("register(") != std::string::npos)
        throw std::runtime_error("the ENGINE_MOTION_CORE block must declare no resource (it is compiled into two shaders)");
    return core;
}

// withCore = false is the stereo header's form: the bytecode arrays alone, no engine-motion core text (the runtime has no use for it).
static std::string render(const std::vector<Variant>& variants, const std::string& key, const std::string& core, bool withCore = true) {
    std::string out = "// Generated by tools/temporal_shader_build; do not edit.\n";
    out += kKeyPrefix + key + "\n#pragma once\nnamespace edvr {\n";
    for (const auto& v : variants) {
        if (v.bytes.empty()) throw std::runtime_error("cannot emit an empty shader");
        out += "inline constexpr unsigned char " + std::string(v.symbol) + "[] = {\n";
        for (size_t i = 0; i < v.bytes.size(); ++i) {
            if (i % 16 == 0) out += "  ";
            out += std::to_string(v.bytes[i]);
            out += (i + 1 == v.bytes.size()) ? "\n" : (i % 16 == 15 ? ",\n" : ", ");
        }
        out += "};\n";
    }
    if (withCore) {
        // Adjacent raw literals of at most 4000 characters (MSVC caps one literal).
        const std::string close = std::string(")") + kCoreDelimiter + "\"";
        if (core.find(close) != std::string::npos) throw std::runtime_error("the core text contains its own delimiter");
        out += "inline constexpr char kEngineMotionCoreHlsl[] =\n";
        if (core.empty()) out += "\"\"\n";
        for (size_t at = 0; at < core.size(); at += 4000)
            out += std::string("R\"") + kCoreDelimiter + "(" + core.substr(at, 4000) + close + "\n";
        out += ";\n";
    }
    return out + "}  // namespace edvr\n";
}

// The core text back out of a rendered header (the self-test's round trip).
static std::string coreFromHeader(const std::string& header) {
    const std::string open = std::string("R\"") + kCoreDelimiter + "(", close = std::string(")") + kCoreDelimiter + "\"";
    std::string core;
    for (size_t at = header.find("kEngineMotionCoreHlsl"); at != std::string::npos;) {
        const size_t b = header.find(open, at);
        if (b == std::string::npos) break;
        const size_t e = header.find(close, b + open.size());
        if (e == std::string::npos) break;
        core += header.substr(b + open.size(), e - (b + open.size()));
        at = e + close.size();
    }
    return core;
}

// A unique, exclusively created sibling prevents clobbering another build's
// temporary output. Only this function's own temporary file is ever removed.
static bool writeAtomic(const fs::path& target, const std::string& text) {
    fs::path temp;
    HANDLE file = INVALID_HANDLE_VALUE;
    for (unsigned attempt = 0; attempt < 16; ++attempt) {
        temp = target;
        temp += L".tmp." + std::to_wstring(GetCurrentProcessId()) + L"." +
                std::to_wstring(GetTickCount64()) + L"." + std::to_wstring(attempt);
        file = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) break;
        if (GetLastError() != ERROR_FILE_EXISTS) return false;
    }
    if (file == INVALID_HANDLE_VALUE) return false;
    bool ok = true;
    size_t offset = 0;
    while (offset < text.size()) {
        const DWORD chunk = static_cast<DWORD>(std::min<size_t>(text.size() - offset, 1024 * 1024));
        DWORD written = 0;
        if (!WriteFile(file, text.data() + offset, chunk, &written, nullptr) || !written) {
            ok = false;
            break;
        }
        offset += written;
    }
    if (ok) ok = FlushFileBuffers(file) != 0;
    const bool closed = CloseHandle(file) != 0;
    if (ok && closed) ok = MoveFileExW(temp.c_str(), target.c_str(),
                                      MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
    else ok = false;
    if (!ok) DeleteFileW(temp.c_str());
    return ok;
}

static std::vector<Variant> initialFixedVariants() {
    static const D3D_SHADER_MACRO night00[] = {{"EDVR_NIGHT_REALISTIC", "0"}, {"EDVR_NIGHT_PULSE_STABLE", "0"}, {nullptr, nullptr}};
    static const D3D_SHADER_MACRO night10[] = {{"EDVR_NIGHT_REALISTIC", "1"}, {"EDVR_NIGHT_PULSE_STABLE", "0"}, {nullptr, nullptr}};
    static const D3D_SHADER_MACRO night01[] = {{"EDVR_NIGHT_REALISTIC", "0"}, {"EDVR_NIGHT_PULSE_STABLE", "1"}, {nullptr, nullptr}};
    static const D3D_SHADER_MACRO night11[] = {{"EDVR_NIGHT_REALISTIC", "1"}, {"EDVR_NIGHT_PULSE_STABLE", "1"}, {nullptr, nullptr}};
    return {
        {"kUiResolveBytecode", "UI resolve", "main", nullptr, {}, false, edvr::kUiResolve},
        {"kUiContentBytecode", "UI source edits", "main", nullptr, {}, false, edvr::kUiContentCs},
        {"kHoloMotionBytecode", "holo motion", "main", nullptr, {}, false, edvr::kHoloMotionBuild},
        {"kNightVisionStockBytecode", "night_vision", "main", night00, {}, false, edvr::kNightVisionPs, "ps_5_0"},
        {"kNightVisionRealisticBytecode", "night_vision", "main", night10, {}, false, edvr::kNightVisionPs, "ps_5_0"},
        {"kNightVisionPulseBytecode", "night_vision", "main", night01, {}, false, edvr::kNightVisionPs, "ps_5_0"},
        {"kNightVisionRealisticPulseBytecode", "night_vision", "main", night11, {}, false, edvr::kNightVisionPs, "ps_5_0"}
    };
}

static std::vector<Variant> fixedVariants(const std::string& core) {
    auto variants = initialFixedVariants();
    const auto coreFixed = coreVariants(core);
    variants.insert(variants.end(), coreFixed.begin(), coreFixed.end());
    const auto extra = extraVariants();
    variants.insert(variants.end(), extra.begin(), extra.end());
    return variants;
}

static std::vector<LegacyContract> initialLegacyContracts() {
    static const D3D_SHADER_MACRO night00[] = {{"EDVR_NIGHT_REALISTIC","0"},{"EDVR_NIGHT_PULSE_STABLE","0"},{nullptr,nullptr}};
    static const D3D_SHADER_MACRO night10[] = {{"EDVR_NIGHT_REALISTIC","1"},{"EDVR_NIGHT_PULSE_STABLE","0"},{nullptr,nullptr}};
    static const D3D_SHADER_MACRO night01[] = {{"EDVR_NIGHT_REALISTIC","0"},{"EDVR_NIGHT_PULSE_STABLE","1"},{nullptr,nullptr}};
    static const D3D_SHADER_MACRO night11[] = {{"EDVR_NIGHT_REALISTIC","1"},{"EDVR_NIGHT_PULSE_STABLE","1"},{nullptr,nullptr}};
    return {
        {"UI resolve","main","cs_5_0",nullptr,0xC2BDB42B78A27FB5ull},
        {"UI source edits","main","cs_5_0",nullptr,0xF9D4871EB8DD272Cull},
        {"holo motion","main","cs_5_0",nullptr,0x4304863E8780F02Eull},
        {"night_vision","main","ps_5_0",night00,0xA810FC6C0DE1C1B7ull},
        {"night_vision","main","ps_5_0",night10,0xA810FC6C0DE1C1B7ull},
        {"night_vision","main","ps_5_0",night01,0xA810FC6C0DE1C1B7ull},
        {"night_vision","main","ps_5_0",night11,0xA810FC6C0DE1C1B7ull},
    };
}

static bool sameMacros(const D3D_SHADER_MACRO* a, const D3D_SHADER_MACRO* b) {
    if (!a || !b) return a == b;
    for (size_t i=0; i<32; ++i) {
        if (!a[i].Name || !b[i].Name) return a[i].Name == b[i].Name;
        if (!a[i].Definition || !b[i].Definition || std::strcmp(a[i].Name,b[i].Name) || std::strcmp(a[i].Definition,b[i].Definition)) return false;
    }
    return false;
}

// The native runtime's stereo renderer: a blit and a skybox, each a vertex and a pixel shader from one HLSL text (src/openxr/stereo_shader_source.h). The
// parameters are the runtime's former D3DCompile calls, field for field: the source names, the entry points "vs" and "ps", the profiles, no macros, flags1 =
// D3DCOMPILE_ENABLE_STRICTNESS, flags2 = 0. The self-test holds them to independently written copies of those calls and the text to the hash of the original.
static std::vector<Variant> stereoVariants() {
    return {
        {"kStereoBlitVsBytecode", "EDVR captured blit", "vs", nullptr, {}, false, edvr::openxr::kStereoBlitHlsl, "vs_5_0", D3DCOMPILE_ENABLE_STRICTNESS, 0},
        {"kStereoBlitPsBytecode", "EDVR captured blit", "ps", nullptr, {}, false, edvr::openxr::kStereoBlitHlsl, "ps_5_0", D3DCOMPILE_ENABLE_STRICTNESS, 0},
        {"kStereoSkyboxVsBytecode", "EDVR skybox", "vs", nullptr, {}, false, edvr::openxr::kStereoSkyboxHlsl, "vs_5_0", D3DCOMPILE_ENABLE_STRICTNESS, 0},
        {"kStereoSkyboxPsBytecode", "EDVR skybox", "ps", nullptr, {}, false, edvr::openxr::kStereoSkyboxHlsl, "ps_5_0", D3DCOMPILE_ENABLE_STRICTNESS, 0},
    };
}

// The stereo header: its own key over the text, the variant table (names, entries, profiles, flags) and the compiler DLL, reused when current.
static int generateStereo(const Options& o) {
    auto variants = stereoVariants();
    const std::string sources = std::string(edvr::openxr::kStereoBlitHlsl) + edvr::openxr::kStereoSkyboxHlsl;
    const std::string key = sourceKey(sources.c_str(), variants, compilerPath());
    if (outputCurrent(o.stereoOutput, key)) {
        std::printf("stereo shaders: unchanged (key %s), reusing %ls\n", key.c_str(), o.stereoOutput.c_str());
        return 0;
    }
    Compiler compiler;
    for (auto& v : variants) if (!compile(compiler.fn, v.alternate, v)) return 4;
    if (!writeAtomic(o.stereoOutput, render(variants, key, std::string(), false))) {
        std::fprintf(stderr, "cannot atomically write generated stereo shader header\n");
        return 5;
    }
    return 0;
}

static int generateTemporal(const Options& o) {
    static const D3D_SHADER_MACRO fast[] = {{"EDVR_TEMPORAL_DIAGNOSTICS", "0"}, {nullptr, nullptr}};
    static const D3D_SHADER_MACRO diagnostic[] = {{"EDVR_TEMPORAL_DIAGNOSTICS", "1"}, {nullptr, nullptr}};
    static const D3D_SHADER_MACRO trace[] = {{"EDVR_TEMPORAL_DIAGNOSTICS", "1"}, {"EDVR_TEMPORAL_TRACE", "1"}, {nullptr, nullptr}};
    std::vector<Variant> variants = {
        {"kEnginePrimaryCopyScatterBytecode", "engine_primary_copy_scatter_cs", "main", nullptr, {}, false, edvr::kEnginePrimaryCopyScatterCsHlsl},
        {"kTemporalMvFastBytecode", "temporal_mv_fast_cs", "mv", fast, {}},
        {"kTemporalMvBytecode", "temporal_mv_cs", "mv", diagnostic, {}},
        {"kTemporalMvTraceBytecode", "temporal_mv_trace_cs", "mv", trace, {}},
        {"kTemporalAaBytecode", "temporal_aa_cs", "main", diagnostic, {}},
        {"kTemporalAaFastBytecode", "temporal_aa_fast_cs", "main", fast, {}},
        {"kFlatMonoPrepBytecode", "flat_mono_prep_cs", "prep", nullptr, {}, true},
        {"kFlatMonoTaaBytecode", "flat_mono_taa_cs", "taa", nullptr, {}, true},
        {"kFlatMonoFinishBytecode", "flat_mono_finish_cs", "finish", nullptr, {}, true},
        {"kFlatMonoSpatialBytecode", "flat_mono_spatial_cs", "spatial", nullptr, {}, true},
        // The VR world route's refusal census (design doc section 82, stage 2 experiment build): one counting pass over the class
        // texture the prep writes on a frame that samples. Made on first use, so a profile that never asks never creates it.
        {"kFlatMonoCensusBytecode", "flat_mono_census_cs", "census", nullptr, {}, true},
        // The HDR route's pixel-shader half (section 81): the result goes back into the game's HDR target, a render
        // target, so the finish and the spatial recovery are draws: one triangle vertex shader and two pixel shaders.
        {"kFlatMonoHdrVsBytecode", "flat_mono_hdr_vs", "hdrVs", nullptr, {}, true, nullptr, "vs_5_0"},
        {"kFlatMonoFinishHdrBytecode", "flat_mono_finish_hdr_ps", "finishHdr", nullptr, {}, true, nullptr, "ps_5_0"},
        {"kFlatMonoSpatialHdrBytecode", "flat_mono_spatial_hdr_ps", "spatialHdr", nullptr, {}, true, nullptr, "ps_5_0"}
    };
    const std::string core = extractCore(edvr::kTemporalCsHlsl); // validate before fixed source assembly
    const auto fixed = fixedVariants(core);
    variants.insert(variants.end(), fixed.begin(), fixed.end());
    const std::string flat = core + edvr::kFlatMonoShaderSource;
    const std::string allSources = std::string(edvr::kTemporalCsHlsl) + flat + edvr::kEnginePrimaryCopyScatterCsHlsl;
    const std::string key = sourceKey(allSources.c_str(), variants, compilerPath());
    if (outputCurrent(o.output, key)) {
        std::printf("temporal shaders: unchanged (key %s), reusing %ls\n", key.c_str(), o.output.c_str());
        return 0;
    }
    Compiler compiler;
    for (auto& v : variants) if (!compile(compiler.fn, v.alternate ? v.alternate : (v.flat ? flat.c_str() : edvr::kTemporalCsHlsl), v)) return 4;
    if (!writeAtomic(o.output, render(variants, key, core))) {
        std::fprintf(stderr, "cannot atomically write generated shader header\n");
        return 5;
    }
    return 0;
}

static int generate(const Options& o) {
    // The actual command path short-circuits before compiler or file access.
    if (o.dry) {
        std::puts("dry-run: compile mv and main (fast/diagnostic) plus capture-only mv trace; embed bytecode and the "
                  "fixed UI/hologram compute and night-vision pixel variants plus engine-motion core text; with --stereo-output, the native "
                  "runtime's four stereo-renderer shaders into their own header; no files written");
        return 0;
    }
    const int temporal = generateTemporal(o);
    if (temporal != 0 || o.stereoOutput.empty()) return temporal;
    return generateStereo(o);
}

static void check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

static void selfTest() {
    const auto cli = [](std::initializer_list<const wchar_t*> args) {
        Options o;
        return parse(static_cast<int>(args.size()), args.begin(), o);
    };
    check(cli({L"tool", L"--self-test"}), "self-test CLI");
    check(cli({L"tool", L"--output", L"shader.h", L"--dry-run"}), "dry-run CLI");
    check(!cli({L"tool"}) && !cli({L"tool", L"--unknown"}), "missing/unknown CLI");
    check(!cli({L"tool", L"--output"}) && !cli({L"tool", L"--output", L""}) &&
          !cli({L"tool", L"--output", L"--dry-run"}), "missing output argument");
    check(!cli({L"tool", L"--output", L"a", L"--output", L"b"}) &&
          !cli({L"tool", L"--self-test", L"--self-test"}) &&
          !cli({L"tool", L"--output", L"a", L"--dry-run", L"--dry-run"}), "duplicate CLI");
    check(!cli({L"tool", L"--self-test", L"--output", L"a"}) &&
          !cli({L"tool", L"--self-test", L"--dry-run"}), "conflicting CLI");
    check(cli({L"tool", L"--output", L"a", L"--stereo-output", L"b"}) &&
          cli({L"tool", L"--stereo-output", L"b", L"--output", L"a", L"--dry-run"}), "stereo output CLI");
    check(!cli({L"tool", L"--stereo-output", L"b"}) && !cli({L"tool", L"--output", L"a", L"--stereo-output"}) &&
          !cli({L"tool", L"--output", L"a", L"--stereo-output", L""}) &&
          !cli({L"tool", L"--output", L"a", L"--stereo-output", L"--dry-run"}) &&
          !cli({L"tool", L"--output", L"a", L"--stereo-output", L"b", L"--stereo-output", L"c"}) &&
          !cli({L"tool", L"--self-test", L"--stereo-output", L"b"}),
          "the stereo output needs --output, a value, once, and never --self-test");

    Compiler compiler;
    Variant v{"kSelfTest", "self.hlsl", "main", nullptr, {}};
    const char* good = "RWStructuredBuffer<float> x:register(u0); [numthreads(1,1,1)] void main(){x[0]=1;}";
    check(compile(compiler.fn, good, v, true), "typed valid HLSL compilation");
    check(v.bytes.size() > 4 && !std::memcmp(v.bytes.data(), "DXBC", 4), "DXBC compiler output");
    check(!compile(compiler.fn, "invalid HLSL", v, true) && v.bytes.empty(), "failed compile clears old bytes");

    // The reuse key: stable for the same inputs, different for any input that
    // decides the bytes. Reads the compiler DLL, never loads it.
    const fs::path dll = compilerPath();
    static const D3D_SHADER_MACRO one[] = {{"X", "1"}, {nullptr, nullptr}};
    const Variant plain{"kA", "a.hlsl", "main", nullptr, {}};
    const Variant defined{"kA", "a.hlsl", "main", one, {}};
    const Variant other{"kA", "a.hlsl", "mv", nullptr, {}};
    const std::string key = sourceKey(good, {plain}, dll);
    check(key.size() == 16 && key == sourceKey(good, {plain}, dll), "source key is stable");
    check(key != sourceKey("void main(){}", {plain}, dll), "source key follows the HLSL");
    check(key != sourceKey(good, {defined}, dll), "source key follows the macros");
    check(key != sourceKey(good, {other}, dll), "source key follows the entry point");
    check(key != sourceKey(good, {plain, plain}, dll), "source key follows the variant list");
    Variant changed = plain;
    changed.profile = "ps_5_0";
    check(key != sourceKey(good, {changed}, dll), "source key follows the shader stage/profile");
    changed = plain; changed.flags1 = D3DCOMPILE_ENABLE_STRICTNESS;
    check(key != sourceKey(good, {changed}, dll), "source key follows compilation flags1");
    changed = plain; changed.flags2 = 1;
    check(key != sourceKey(good, {changed}, dll), "source key follows compilation flags2");
    changed = plain; changed.alternate = "alternate HLSL";
    check(key != sourceKey(good, {changed}, dll), "source key follows each alternate source");
    static const D3D_SHADER_MACRO two[] = {{"X", "2"}, {nullptr, nullptr}};
    changed = defined; changed.macros = two;
    check(sourceKey(good, {defined}, dll) != sourceKey(good, {changed}, dll), "source key follows macro values");

    // Independent copies of the former runtime call metadata. A wrong table
    // profile, source name, macro permutation or flags changes the actual DXBC.
    auto fixed = initialFixedVariants();
    check(fixed.size() == 7, "three fixed compute shaders and every night pixel permutation");
    const char* legacySources[] = {edvr::kUiResolve, edvr::kUiContentCs, edvr::kHoloMotionBuild};
    const char* legacyNames[] = {"UI resolve", "UI source edits", "holo motion"};
    for (size_t i = 0; i < fixed.size(); ++i) {
        auto& shader = fixed[i];
        check(compile(compiler.fn, shader.alternate, shader, true), "fixed variant compiles at build time");
        const unsigned nightMode = i < 3 ? 0 : static_cast<unsigned>(i - 3);
        const D3D_SHADER_MACRO legacyNight[] = {
            {"EDVR_NIGHT_REALISTIC", (nightMode & 1) ? "1" : "0"},
            {"EDVR_NIGHT_PULSE_STABLE", (nightMode & 2) ? "1" : "0"}, {nullptr, nullptr}};
        const char* source = i < 3 ? legacySources[i] : edvr::kNightVisionPs;
        check(!std::strcmp(shader.sourceName, i < 3 ? legacyNames[i] : "night_vision") &&
            !std::strcmp(shader.entry, "main") &&
            !std::strcmp(shader.profile, i < 3 ? "cs_5_0" : "ps_5_0") &&
            shader.flags1 == 0 && shader.flags2 == 0,
            "fixed runtime source name, entry, profile and zero flags are preserved");
        if (i < 3) check(shader.macros == nullptr, "fixed compute has the original empty macro list");
        else check(shader.macros && shader.macros[0].Name && shader.macros[1].Name &&
            !std::strcmp(shader.macros[0].Name, legacyNight[0].Name) &&
            !std::strcmp(shader.macros[0].Definition, legacyNight[0].Definition) &&
            !std::strcmp(shader.macros[1].Name, legacyNight[1].Name) &&
            !std::strcmp(shader.macros[1].Definition, legacyNight[1].Definition) &&
            shader.macros[2].Name == nullptr, "night macro values and order match the runtime permutation");
        ComPtr<ID3DBlob> code, errors;
        const HRESULT hr = compiler.fn(source, std::strlen(source), i < 3 ? legacyNames[i] : "night_vision",
            i < 3 ? nullptr : legacyNight, nullptr, "main", i < 3 ? "cs_5_0" : "ps_5_0", 0, 0, &code, &errors);
        check(SUCCEEDED(hr) && code && code->GetBufferSize() == shader.bytes.size() &&
            !std::memcmp(code->GetBufferPointer(), shader.bytes.data(), shader.bytes.size()),
            "fixed bytecode is byte-exact with its former flags-zero runtime compilation");
    }


    // Independently frozen former-call metadata and fully assembled source
    // fingerprints: extraction changes must fail before a headset flight.
    // The independent legacy contracts also cover diagnostic-only shaders.
    auto coreLegacy = initialLegacyContracts();
    const auto originalCore = coreLegacyContracts(), originalExtra = extraLegacyContracts();
    coreLegacy.insert(coreLegacy.end(), originalCore.begin(), originalCore.end());
    coreLegacy.insert(coreLegacy.end(), originalExtra.begin(), originalExtra.end());
    auto coreFixed = fixedVariants(extractCore(edvr::kTemporalCsHlsl));
    check(originalCore.size() == 30 && originalExtra.size() == 18 && coreFixed.size() == 55 && coreLegacy.size() == coreFixed.size(), "all fixed shader contracts including diagnostic variants are registered");
    for(size_t i=0;i<coreFixed.size();++i)for(size_t j=0;j<i;++j)
        check(std::strcmp(coreFixed[i].symbol,coreFixed[j].symbol)!=0,"generated shader symbols do not collide");
    using ReflectFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, REFIID, void**);
    const auto reflect = reinterpret_cast<ReflectFn>(GetProcAddress(compiler.module, "D3DReflect"));
    check(reflect != nullptr, "system compiler reflection available");
    for (size_t i = 0; i < coreFixed.size() && i < coreLegacy.size(); ++i) {
        auto& shader = coreFixed[i]; const auto& legacy = coreLegacy[i];
        Fnv1a64 sourceFingerprint; sourceFingerprint.add(shader.alternate, std::strlen(shader.alternate));
        check(legacy.sourceHash && sourceFingerprint.hash == legacy.sourceHash, "fixed HLSL remains exact to the original assembled source");
        check(!std::strcmp(shader.sourceName, legacy.sourceName) && !std::strcmp(shader.entry, legacy.entry) &&
            !std::strcmp(shader.profile, legacy.profile) && sameMacros(shader.macros,legacy.macros) && shader.flags1 == 0 && shader.flags2 == 0,
            "fixed original source-name, entry, stage and flags are preserved");
        check(compile(compiler.fn, shader.alternate, shader, true), "fixed shader compiles");
        ComPtr<ID3DBlob> code, errors;
        const HRESULT made = compiler.fn(shader.alternate, std::strlen(shader.alternate), legacy.sourceName,
            legacy.macros, nullptr, legacy.entry, legacy.profile, 0, 0, &code, &errors);
        check(SUCCEEDED(made) && code && code->GetBufferSize() == shader.bytes.size() &&
            !std::memcmp(code->GetBufferPointer(), shader.bytes.data(), shader.bytes.size()),
            "fixed bytecode is byte-exact with original runtime compilation");
        if (reflect && !shader.bytes.empty()) {
            ComPtr<ID3D11ShaderReflection> reflection;
            const HRESULT reflected = reflect(shader.bytes.data(), shader.bytes.size(), __uuidof(ID3D11ShaderReflection), reinterpret_cast<void**>(reflection.GetAddressOf()));
            D3D11_SHADER_DESC desc{};
            const unsigned expected = legacy.profile[0] == 'v' ? D3D11_SHVER_VERTEX_SHADER : legacy.profile[0] == 'p' ? D3D11_SHVER_PIXEL_SHADER : D3D11_SHVER_COMPUTE_SHADER;
            check(SUCCEEDED(reflected) && reflection && SUCCEEDED(reflection->GetDesc(&desc)) &&
                D3D11_SHVER_GET_TYPE(desc.Version) == expected && D3D11_SHVER_GET_MAJOR(desc.Version) == 5,
                "generated payload reflects its original shader stage and SM5 contract");
        }
    }

    // The native runtime's stereo shaders (src/openxr/stereo_shader_source.h): four variants from two texts, held to the four D3DCompile calls they replace. The
    // contracts below are written independently of stereoVariants(): the source names, entries, profiles and flag word of src/openxr/d3d11_stereo.cpp at
    // origin/main 069ebee4, and the FNV-1a64 hash of each text as it stood there (the raw string between R"( and )"). A changed text, name, entry, profile or flag word
    // fails here, in the build, not in a headset session. The bytes are compared with a compile made here, at test time, with the old call's arguments.
    {
        struct StereoContract { const char* symbol; const char* sourceName; const char* entry; const char* profile; UINT flags1; unsigned long long textHash; bool blit; };
        static const StereoContract contracts[] = {
            {"kStereoBlitVsBytecode", "EDVR captured blit", "vs", "vs_5_0", D3DCOMPILE_ENABLE_STRICTNESS, 0x8F805D7103F7BA83ull, true},
            {"kStereoBlitPsBytecode", "EDVR captured blit", "ps", "ps_5_0", D3DCOMPILE_ENABLE_STRICTNESS, 0x8F805D7103F7BA83ull, true},
            {"kStereoSkyboxVsBytecode", "EDVR skybox", "vs", "vs_5_0", D3DCOMPILE_ENABLE_STRICTNESS, 0x1F8D73118F42ED8Bull, false},
            {"kStereoSkyboxPsBytecode", "EDVR skybox", "ps", "ps_5_0", D3DCOMPILE_ENABLE_STRICTNESS, 0x1F8D73118F42ED8Bull, false},
        };
        auto stereo = stereoVariants();
        check(stereo.size() == 4 && sizeof(contracts) / sizeof(contracts[0]) == 4, "the four stereo shaders are registered");
        for (size_t i = 0; i < stereo.size(); ++i) {
            auto& shader = stereo[i]; const auto& legacy = contracts[i];
            for (size_t j = 0; j < i; ++j) check(std::strcmp(stereo[j].symbol, shader.symbol) != 0, "stereo symbols do not collide");
            const char* text = legacy.blit ? edvr::openxr::kStereoBlitHlsl : edvr::openxr::kStereoSkyboxHlsl;
            Fnv1a64 fingerprint; fingerprint.add(text, std::strlen(text));
            check(fingerprint.hash == legacy.textHash, "stereo HLSL remains exact to the text the runtime compiled");
            check(shader.alternate && !std::strcmp(shader.alternate, text), "each stereo variant compiles the text its contract names");
            check(!std::strcmp(shader.symbol, legacy.symbol) && !std::strcmp(shader.sourceName, legacy.sourceName) && !std::strcmp(shader.entry, legacy.entry) &&
                  !std::strcmp(shader.profile, legacy.profile) && shader.macros == nullptr && shader.flags1 == legacy.flags1 && shader.flags2 == 0,
                  "stereo symbol, source name, entry, profile, no macros and the strictness flag are the runtime's former call's");
            check(compile(compiler.fn, shader.alternate, shader, true), "stereo shader compiles");
            check(shader.bytes.size() > 4 && !std::memcmp(shader.bytes.data(), "DXBC", 4), "stereo output is DXBC");
            ComPtr<ID3DBlob> code, errors;
            const HRESULT made = compiler.fn(text, std::strlen(text), legacy.sourceName, nullptr, nullptr, legacy.entry, legacy.profile, legacy.flags1, 0, &code, &errors);
            check(SUCCEEDED(made) && code && code->GetBufferSize() == shader.bytes.size() &&
                  !std::memcmp(code->GetBufferPointer(), shader.bytes.data(), shader.bytes.size()),
                  "stereo bytecode is byte-exact with the runtime's former compilation");
            ComPtr<ID3D11ShaderReflection> reflection;
            D3D11_SHADER_DESC desc{};
            const unsigned stage = legacy.profile[0] == 'v' ? D3D11_SHVER_VERTEX_SHADER : D3D11_SHVER_PIXEL_SHADER;
            check(SUCCEEDED(reflect(shader.bytes.data(), shader.bytes.size(), __uuidof(ID3D11ShaderReflection), reinterpret_cast<void**>(reflection.GetAddressOf()))) &&
                  reflection && SUCCEEDED(reflection->GetDesc(&desc)) && D3D11_SHVER_GET_MAJOR(desc.Version) == 5 &&
                  D3D11_SHVER_GET_TYPE(desc.Version) == stage,
                  "stereo payload reflects its stage and SM5");
        }
        // The text really is the old call's: a one-byte edit of either changes the hash (the control for the hashes above), and the two texts differ.
        Fnv1a64 edited; std::string tweaked = edvr::openxr::kStereoBlitHlsl; tweaked[tweaked.size() / 2] ^= 1; edited.add(tweaked.data(), tweaked.size());
        check(edited.hash != contracts[0].textHash && contracts[0].textHash != contracts[2].textHash, "a one-byte change to the text changes its hash");
        // The stereo header: no engine-motion core, the key line, the four arrays by name, every byte round-tripped.
        const std::string skey = sourceKey((std::string(edvr::openxr::kStereoBlitHlsl) + edvr::openxr::kStereoSkyboxHlsl).c_str(), stereo, dll);
        const std::string sheader = render(stereo, skey, std::string(), false);
        check(sheader.find("kEngineMotionCoreHlsl") == std::string::npos && sheader.find(std::string(kKeyPrefix) + skey + "\n") != std::string::npos &&
              render(stereo, skey, std::string(), true).find("kEngineMotionCoreHlsl") != std::string::npos,
              "the stereo header carries its key and no engine-motion core text");
        size_t at = 0;
        for (const auto& shader : stereo) {
            const std::string head = "inline constexpr unsigned char " + std::string(shader.symbol) + "[] = {\n";
            at = sheader.find(head, at);
            check(at != std::string::npos, "the stereo header names each array, in order");
            const size_t from = at + head.size(), to = sheader.find("};", from);
            check(to != std::string::npos, "stereo array delimiters");
            std::string text = sheader.substr(from, to - from);
            std::replace(text.begin(), text.end(), ',', ' ');
            std::istringstream in(text);
            for (auto expected : shader.bytes) {
                unsigned actual = 999;
                check(bool(in >> actual) && actual == expected, "all stereo bytes round-trip");
            }
            unsigned more = 0;
            check(!(in >> more) && in.eof(), "no extra stereo bytes");
            at = to;
        }
        // The key follows what decides the bytes: the text, and each variant's flags and profile.
        auto flagChanged = stereo; flagChanged[0].flags1 = 0;
        auto profileChanged = stereo; profileChanged[1].profile = "ps_4_0";
        auto nameChanged = stereo; nameChanged[2].sourceName = "EDVR skybox 2";
        const std::string textKey = (std::string(edvr::openxr::kStereoBlitHlsl) + edvr::openxr::kStereoSkyboxHlsl);
        check(skey != sourceKey(textKey.c_str(), flagChanged, dll) && skey != sourceKey(textKey.c_str(), profileChanged, dll) &&
              skey != sourceKey(textKey.c_str(), nameChanged, dll) && skey != sourceKey((textKey + " ").c_str(), stereo, dll),
              "the stereo key follows the flag word, the profile, the source name and the text");
    }

    // The engine-motion core: exactly one pair of markers in order, no
    // resource inside, and the text round-trips through the header's raw
    // literals -- including one long enough to be split into several.
    const auto throwsOn = [](const std::string& s) {
        try { extractCore(s); } catch (const std::runtime_error&) { return true; }
        return false;
    };
    check(extractCore("x\n// ENGINE_MOTION_CORE_BEGIN\nA\nB\n// ENGINE_MOTION_CORE_END\ny") == "A\nB\n",
          "core text between its markers");
    check(throwsOn("A\n// ENGINE_MOTION_CORE_END") && throwsOn("// ENGINE_MOTION_CORE_BEGIN\nA") &&
          throwsOn("// ENGINE_MOTION_CORE_END\n// ENGINE_MOTION_CORE_BEGIN\n") &&
          throwsOn("// ENGINE_MOTION_CORE_BEGIN\n// ENGINE_MOTION_CORE_BEGIN\nA// ENGINE_MOTION_CORE_END") &&
          throwsOn("// ENGINE_MOTION_CORE_BEGIN\nTexture2D t : register(t0);\n// ENGINE_MOTION_CORE_END"),
          "a missing, reordered or doubled marker, or a resource inside, fails the build");
    const std::string production = extractCore(edvr::kTemporalCsHlsl);
    const std::string flat = production + edvr::kFlatMonoShaderSource;
    static const char* flatEntries[] = {"prep", "taa", "finish", "spatial", "census"};
    for (const char* entry : flatEntries) {
        Variant mono{"kFlatSelfTest", "flat_mono_self_test", entry, nullptr, {}, true};
        check(compile(compiler.fn, flat.c_str(), mono), "production flat mono shader compilation");
    }
    // The HDR route's three graphics entry points, each under its own profile.
    static const struct { const char* entry; const char* profile; } flatHdrEntries[] = {
        {"hdrVs", "vs_5_0"}, {"finishHdr", "ps_5_0"}, {"spatialHdr", "ps_5_0"}};
    for (const auto& e : flatHdrEntries) {
        Variant mono{"kFlatSelfTest", "flat_mono_self_test_hdr", e.entry, nullptr, {}, true, nullptr, e.profile};
        check(compile(compiler.fn, flat.c_str(), mono), "production flat HDR route shader compilation");
    }
    check(production.find("bool engineReprojectRows(") != std::string::npos &&
          production.find("uint engineRecordKind(") != std::string::npos, "the production core carries the shared arithmetic");
    std::string longCore;
    for (unsigned i = 0; i < 1500; ++i) longCore += "float f" + std::to_string(i) + ";\n";
    check(longCore.size() > 8000 && coreFromHeader(render({}, key, longCore)) == longCore, "a long core round-trips in pieces");

    for (unsigned i = 0; i < 256; ++i) v.bytes.push_back(static_cast<unsigned char>(i));
    v.bytes.push_back(0);  // also cover a partial final output row
    const std::string header = render({v}, key, production);
    check(coreFromHeader(header) == production, "the production core round-trips through the header");
    check(header.find(std::string(kKeyPrefix) + key + "\n") != std::string::npos, "generated header carries the key");
    const size_t begin = header.find("[] = {\n"), end = header.find("};", begin);
    check(begin != std::string::npos && end != std::string::npos, "generated array delimiters");
    std::string body = header.substr(begin + 7, end - (begin + 7));
    std::replace(body.begin(), body.end(), ',', ' ');
    std::istringstream input(body);
    for (auto expected : v.bytes) {
        unsigned actual = 999;
        check(bool(input >> actual) && actual == expected, "all generated bytes round-trip");
    }
    unsigned extra = 0;
    check(!(input >> extra) && input.eof(), "no extra generated bytes");

    // Self-test owns one uniquely created temporary directory. Dry-run must
    // preserve its existing output and must not create a missing parent.
    const fs::path parent = fs::temp_directory_path() /
        (L"edvr-shader-selftest-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    check(CreateDirectoryW(parent.c_str(), nullptr) != 0, "create self-test directory");
    const fs::path target = parent / L"test.h";
    const fs::path stereoTarget = parent / L"stereo.h";
    try {
        check(!outputCurrent(target, key), "a missing output is not reused");
        check(writeAtomic(target, "sentinel"), "write first output");
        check(!outputCurrent(target, key), "an output without a key line is not reused");
        check(generate({false, true, target}) == 0, "existing-output dry-run");
        std::ifstream sentinel(target, std::ios::binary);
        const std::string saved((std::istreambuf_iterator<char>(sentinel)), {});
        check(saved == "sentinel", "dry-run preserves existing output");
        sentinel.close();
        check(generate({false, true, parent / L"missing" / L"test.h"}) == 0 &&
              !fs::exists(parent / L"missing"), "dry-run creates no parent");
        check(std::distance(fs::directory_iterator(parent), fs::directory_iterator()) == 1,
              "dry-run creates no temporary files");
        check(writeAtomic(target, header), "atomic replacement");
        std::ifstream generated(target, std::ios::binary);
        const std::string disk((std::istreambuf_iterator<char>(generated)), {});
        check(disk == header, "emitted file matches byte-for-byte");
        generated.close();
        check(outputCurrent(target, key), "an output with the matching key is reused");
        check(!outputCurrent(target, "0000000000000000"), "an output with another key is not reused");
        check(!writeAtomic(parent / L"missing" / L"bad.h", "bad"), "missing parent fails cleanly");
        check(std::distance(fs::directory_iterator(parent), fs::directory_iterator()) == 1,
              "atomic writer leaves no temporary files");
        // The stereo header, beside the temporal one and only on request: a dry run writes none (not even next to an existing output), a real run writes it
        // atomically with its key, a second run reuses it untouched, and a stale key is rebuilt. (The temporal half is not run here: its AA variant alone is 19 s of fxc.)
        const Options dryStereo{false, true, target, stereoTarget};
        check(generate(dryStereo) == 0 && !fs::exists(stereoTarget), "a dry run with --stereo-output writes no stereo header");
        check(std::distance(fs::directory_iterator(parent), fs::directory_iterator()) == 1, "...and no temporary file");
        const Options stereoOnly{false, false, target, stereoTarget};
        check(generateStereo(stereoOnly) == 0 && fs::exists(stereoTarget), "the stereo header is generated");
        const auto stereoNow = stereoVariants();
        const std::string stereoText = std::string(edvr::openxr::kStereoBlitHlsl) + edvr::openxr::kStereoSkyboxHlsl;
        const std::string stereoKey = sourceKey(stereoText.c_str(), stereoNow, dll);
        check(outputCurrent(stereoTarget, stereoKey), "the generated stereo header carries the current key");
        std::ifstream first(stereoTarget, std::ios::binary);
        const std::string firstText((std::istreambuf_iterator<char>(first)), {});
        first.close();
        const auto firstWrite = fs::last_write_time(stereoTarget);
        check(generateStereo(stereoOnly) == 0 && fs::last_write_time(stereoTarget) == firstWrite, "a current stereo header is reused, not rewritten");
        check(writeAtomic(stereoTarget, "// stale\n// key: 0000000000000000\n") && generateStereo(stereoOnly) == 0, "a stale stereo header is regenerated");
        std::ifstream again(stereoTarget, std::ios::binary);
        const std::string againText((std::istreambuf_iterator<char>(again)), {});
        again.close();
        check(againText == firstText && std::distance(fs::directory_iterator(parent), fs::directory_iterator()) == 2,
              "...to exactly the text it was, with no temporary file left");
        check(DeleteFileW(stereoTarget.c_str()) != 0, "stereo header cleanup");
    } catch (...) {
        DeleteFileW(stereoTarget.c_str());
        DeleteFileW(target.c_str());
        RemoveDirectoryW(parent.c_str());
        throw;
    }
    check(DeleteFileW(target.c_str()) && RemoveDirectoryW(parent.c_str()), "self-test cleanup");
    std::puts("PASS: 58 fixed-shader original-source hashes, byte parity and SM5 reflection; 4 stereo shaders held to the runtime's former compile calls (text hash, parameters, bytes); "
              "temporal shader compiler, CLI, byte round-trip, engine-motion core text, atomic output, reuse key and dry-run invariants");
}

int wmain(int argc, wchar_t** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    setvbuf(stdout, nullptr, _IONBF, 0);
    try {
        Options o;
        if (!parse(argc, argv, o)) {
            std::fprintf(stderr, "usage: --output path [--stereo-output path] [--dry-run] | --self-test\n");
            return 2;
        }
        if (o.selfTest) { selfTest(); return 0; }
        return generate(o);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "temporal shader build: %s\n", e.what());
        return 10;
    }
}
