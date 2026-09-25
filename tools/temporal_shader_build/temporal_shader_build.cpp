// Compile fixed temporal shaders during the build, never in the game.
#include <windows.h>
#include <d3dcompiler.h>
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

struct Options { bool selfTest = false, dry = false; fs::path output; };
static bool parse(int argc, const wchar_t* const* argv, Options& o) {
    o = {};
    bool outputSeen = false;
    for (int i = 1; i < argc; ++i) {
        if (!wcscmp(argv[i], L"--self-test") && !o.selfTest) o.selfTest = true;
        else if (!wcscmp(argv[i], L"--dry-run") && !o.dry) o.dry = true;
        else if (!wcscmp(argv[i], L"--output") && !outputSeen && i + 1 < argc) {
            const wchar_t* value = argv[++i];
            if (!*value || !wcsncmp(value, L"--", 2)) return false;
            o.output = value;
            outputSeen = true;
        } else return false;
    }
    return o.selfTest ? !outputSeen && !o.dry : outputSeen;
}

struct Variant {
    const char* symbol;
    const char* sourceName;
    const char* entry;
    const D3D_SHADER_MACRO* macros;
    std::vector<unsigned char> bytes;
    bool flat = false;
};

static bool compile(CompileFn fn, const char* source, Variant& v, bool quiet = false) {
    v.bytes.clear();
    ComPtr<ID3DBlob> code, errors;
    const ULONGLONG start = GetTickCount64();
    const HRESULT hr = fn(source, std::strlen(source), v.sourceName, v.macros,
                          nullptr, v.entry, "cs_5_0", 0, 0, &code, &errors);
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
static const char kKeyTag[] = "edvr-temporal-shader-key/3";
static const char kProfile[] = "cs_5_0";
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
    key.add(kProfile);
    key.add(source);
    for (const auto& v : variants) {
        key.add(v.symbol);
        key.add(v.sourceName);
        key.add(v.entry);
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

static std::string render(const std::vector<Variant>& variants, const std::string& key, const std::string& core) {
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
    // Adjacent raw literals of at most 4000 characters (MSVC caps one literal).
    const std::string close = std::string(")") + kCoreDelimiter + "\"";
    if (core.find(close) != std::string::npos) throw std::runtime_error("the core text contains its own delimiter");
    out += "inline constexpr char kEngineMotionCoreHlsl[] =\n";
    if (core.empty()) out += "\"\"\n";
    for (size_t at = 0; at < core.size(); at += 4000)
        out += std::string("R\"") + kCoreDelimiter + "(" + core.substr(at, 4000) + close + "\n";
    out += ";\n";
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

static int generate(const Options& o) {
    // The actual command path short-circuits before compiler or file access.
    if (o.dry) {
        std::puts("dry-run: compile mv and main (fast/diagnostic) plus capture-only mv trace; embed bytecode and the "
                  "engine-motion core text; no files written");
        return 0;
    }
    static const D3D_SHADER_MACRO fast[] = {{"EDVR_TEMPORAL_DIAGNOSTICS", "0"}, {nullptr, nullptr}};
    static const D3D_SHADER_MACRO diagnostic[] = {{"EDVR_TEMPORAL_DIAGNOSTICS", "1"}, {nullptr, nullptr}};
    static const D3D_SHADER_MACRO trace[] = {{"EDVR_TEMPORAL_DIAGNOSTICS", "1"}, {"EDVR_TEMPORAL_TRACE", "1"}, {nullptr, nullptr}};
    std::vector<Variant> variants = {
        {"kTemporalMvFastBytecode", "temporal_mv_fast_cs", "mv", fast, {}},
        {"kTemporalMvBytecode", "temporal_mv_cs", "mv", diagnostic, {}},
        {"kTemporalMvTraceBytecode", "temporal_mv_trace_cs", "mv", trace, {}},
        {"kTemporalAaBytecode", "temporal_aa_cs", "main", diagnostic, {}},
        {"kTemporalAaFastBytecode", "temporal_aa_fast_cs", "main", fast, {}},
        {"kFlatMonoPrepBytecode", "flat_mono_prep_cs", "prep", nullptr, {}, true},
        {"kFlatMonoTaaBytecode", "flat_mono_taa_cs", "taa", nullptr, {}, true},
        {"kFlatMonoFinishBytecode", "flat_mono_finish_cs", "finish", nullptr, {}, true},
        {"kFlatMonoSpatialBytecode", "flat_mono_spatial_cs", "spatial", nullptr, {}, true}
    };
    const std::string core = extractCore(edvr::kTemporalCsHlsl);   // throws on a broken core before any work
    const std::string flat = core + edvr::kFlatMonoShaderSource;
    const std::string allSources = std::string(edvr::kTemporalCsHlsl) + flat;
    const std::string key = sourceKey(allSources.c_str(), variants, compilerPath());
    if (outputCurrent(o.output, key)) {
        std::printf("temporal shaders: unchanged (key %s), reusing %ls\n", key.c_str(), o.output.c_str());
        return 0;
    }
    Compiler compiler;
    for (auto& v : variants) if (!compile(compiler.fn, v.flat ? flat.c_str() : edvr::kTemporalCsHlsl, v)) return 4;
    if (!writeAtomic(o.output, render(variants, key, core))) {
        std::fprintf(stderr, "cannot atomically write generated shader header\n");
        return 5;
    }
    return 0;
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
    for (const char* entry : {"prep", "taa", "finish", "spatial"}) {
        Variant mono{"kFlatSelfTest", "flat_mono_self_test", entry, nullptr, {}, true};
        check(compile(compiler.fn, flat.c_str(), mono), "production flat mono shader compilation");
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
    } catch (...) {
        DeleteFileW(target.c_str());
        RemoveDirectoryW(parent.c_str());
        throw;
    }
    check(DeleteFileW(target.c_str()) && RemoveDirectoryW(parent.c_str()), "self-test cleanup");
    std::puts("PASS: temporal shader compiler, CLI, byte round-trip, engine-motion core text, atomic output, reuse key "
              "and dry-run invariants");
}

int wmain(int argc, wchar_t** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    setvbuf(stdout, nullptr, _IONBF, 0);
    try {
        Options o;
        if (!parse(argc, argv, o)) {
            std::fprintf(stderr, "usage: --output path [--dry-run] | --self-test\n");
            return 2;
        }
        if (o.selfTest) { selfTest(); return 0; }
        return generate(o);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "temporal shader build: %s\n", e.what());
        return 10;
    }
}
