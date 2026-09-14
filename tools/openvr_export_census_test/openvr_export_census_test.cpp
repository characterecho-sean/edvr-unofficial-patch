// Exercise the built proxy, with fresh DLL/config/log state in every child.
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <map>
#include <thread>
#include "fixture.h"
#include "../../src/openvr/compat/openvr_v0_9_20.h"
namespace fs = std::filesystem;
using Init = uint32_t (__cdecl*)(vr::EVRInitError*, vr::EVRApplicationType);
using Shutdown = void (__cdecl*)();
using Valid = bool (__cdecl*)(const char*);
using Token = uint32_t (__cdecl*)();
using Generic = void* (__cdecl*)(const char*, vr::EVRInitError*);
static void require(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }
static fs::path exePath() {
    wchar_t path[MAX_PATH]{};
    const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    require(n && n < MAX_PATH, "executable path"); return path;
}
static int child(const std::string& mode) {
    HMODULE proxy = LoadLibraryW((exePath().parent_path() / "openvr_api.dll").c_str());
    require(proxy != nullptr, "load built proxy");
    const auto init = reinterpret_cast<Init>(GetProcAddress(proxy, "VR_InitInternal"));
    const auto shutdown = reinterpret_cast<Shutdown>(GetProcAddress(proxy, "VR_ShutdownInternal"));
    const auto valid = reinterpret_cast<Valid>(GetProcAddress(proxy, "VR_IsInterfaceVersionValid"));
    const auto token = reinterpret_cast<Token>(GetProcAddress(proxy, "VR_GetInitToken"));
    const auto generic = reinterpret_cast<Generic>(GetProcAddress(proxy, "VR_GetGenericInterface"));
    require(init && shutdown && valid && token && generic, "five historical exports");
    vr::EVRInitError error = vr::VRInitError_Unknown;
    const auto first = init(&error, vr::VRApplication_Scene); // Must load real runtime lazily.
    const auto statsFn = reinterpret_cast<const ExportFixtureStats* (__cdecl*)()>(
        GetProcAddress(GetModuleHandleW(L"openvr_api_orig.dll"), "edvrExportFixtureStats"));
    require(statsFn != nullptr, "fake runtime actually loaded");
    const auto* stats = statsFn();
    if (mode == "missing") {
        require(first == 0 && error == vr::VRInitError_Unknown, "missing init leaves error untouched");
        require(!valid("EDVRTest_001") && token() == 0, "missing validity/token preserve zero stubs");
        require(!generic("EDVRTest_001", &error) && static_cast<int>(error) == 1,
                "missing Generic preserves legacy error write");
        shutdown(); return 0;
    }
    require(first == 41 && error == vr::VRInitError_None, "init return and error");
    require(stats->lastApplication == vr::VRApplication_Scene && stats->calls[0] == 1, "init forwarded once");
    if (mode == "reentry") require(stats->reentryCalls == 1 && stats->reentryResult == 0,
                                    "DllMain reentry reached safe zero stub");
    uint32_t expected[5] = {1,0,0,0,0};
    require(valid("EDVRTest_001") && !valid("IVROverlay_011"), "validity forwarding"); expected[3] += 2;
    require(!valid(reinterpret_cast<const char*>(1)), "unreadable diagnostic input is guarded"); ++expected[3];
    void* iface = generic("EDVRTest_001", &error); ++expected[2];
    require(iface && error == vr::VRInitError_None, "Generic exact non-null result");
    require(generic("EDVRTest_001", nullptr) == iface, "Generic repeated identity/null error"); ++expected[2];
    require(!generic("missing", &error) && error == vr::VRInitError_Init_InterfaceNotFound,
            "Generic failure forwarding"); ++expected[2];
    require(!generic("IVROverlay_011", &error) && error == vr::VRInitError_Init_InterfaceNotFound,
            "suppression still shields runtime"); // No call to the real getter.
    require(token() == first, "token matches init"); ++expected[4];
    shutdown(); ++expected[1];
    require(init(nullptr, vr::VRApplication_Scene) == first + 1, "re-init/null error unchanged"); ++expected[0];
    require(stats->nullErrors == 1, "null error was not replaced by local storage");
    require(init(&error, vr::VRApplication_Background) == 0 && error == vr::VRInitError_Unknown,
            "failed init forwarded unchanged"); ++expected[0];
    require(init(reinterpret_cast<vr::EVRInitError*>(1), vr::VRApplication_Scene) == first + 2,
            "unreadable diagnostic output is guarded"); ++expected[0];
    // Validity budget saturation is concurrent; the other four are sequential.
    std::thread workers[8];
    for (auto& w : workers) w = std::thread([&] { for(unsigned i=0;i<20;++i) valid("EDVRTest_001"); });
    for (auto& w : workers) w.join(); expected[3] += 160;
    for (unsigned i=0;i<100;++i) {
        require(init(&error, vr::VRApplication_Scene) != 0 && error == vr::VRInitError_None, "saturated init"); ++expected[0];
        require(generic("EDVRTest_001", &error) == iface, "saturated Generic identity"); ++expected[2];
        require(token() != 0, "saturated token"); ++expected[4];
        shutdown(); ++expected[1];
    }
    for (unsigned i=0;i<5;++i) require(stats->calls[i] == expected[i], "each runtime export called exactly once per request");
    // Keep the proxy loaded; normal process detach flushes the log without
    // inventing unsupported runtime unload/reload ownership semantics.
    return 0;
}
static std::string readLog(const fs::path& stage) {
    std::string result; unsigned count = 0;
    for (const auto& f : fs::directory_iterator(stage / "edvr_logs")) {
        if (f.path().filename().string().rfind("edvr_vr_",0) != 0 || f.path().extension() != ".log") continue;
        std::ifstream in(f.path(), std::ios::binary); require(in.good(), "read child log");
        result.assign(std::istreambuf_iterator<char>(in), {}); ++count;
    }
    require(count == 1, "one fresh VR log"); return result;
}
static void verifyLog(const std::string& log, const std::string& mode) {
    std::map<unsigned long long, std::string> begins, ends;
    std::map<std::string, unsigned> counts;
    std::istringstream lines(log); std::string line;
    while (std::getline(lines,line)) {
        const auto at = line.find("VR export census: record="); if (at == std::string::npos) continue;
        unsigned long long id = 0; char phase[16]{}, name[64]{}, caller[32]{};
        require(sscanf_s(line.c_str()+at, "VR export census: record=%llu phase=%15s name=%63s caller=%31s",
            &id,phase,unsigned(sizeof(phase)),name,unsigned(sizeof(name)),caller,unsigned(sizeof(caller))) == 4,
            "parse census identity");
        require(id != 0 && std::strcmp(caller,"game-exe") == 0, "caller attribution; no loader-lock reentry log");
        if (std::strcmp(phase,"begin") == 0) { require(begins.emplace(id,name).second, "unique begin"); ++counts[name]; }
        else { require(std::strcmp(phase,"end") == 0 && ends.emplace(id,name).second, "unique end"); }
    }
    if (mode == "disabled") { require(begins.empty() && ends.empty(), "disabled means no export census"); return; }
    require(begins == ends && counts.size() == 5, "all five exports enabled with exactly paired records");
    for (const auto& c : counts) require(c.second == (mode == "missing" ? 1u : 64u), "bounded per-export budget");
    require(log.find("error_present=1 error_readable=1") != std::string::npos, "output error recorded");
    if (mode != "missing") {
        require(log.find("interface_readable=0") != std::string::npos, "invalid input flagged");
        require(log.find("error_present=1 error_readable=0") != std::string::npos, "invalid output flagged");
        require(log.find("error_present=0 error_readable=0") != std::string::npos, "null output flagged");
        require(log.find("suppressed=1") != std::string::npos, "suppression result recorded");
    }
}
static void runCase(const fs::path& build, const std::string& mode) {
    const auto stage = build / ("export-census-" + mode + "-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()));
    require(fs::create_directory(stage), "fresh fixture directory");
    fs::copy_file(exePath(), stage / "child.exe"); fs::copy_file(build / "openvr_api.dll", stage / "openvr_api.dll");
    const char* fixture = mode == "missing" ? "export_census_missing.dll" :
        mode == "reentry" ? "export_census_reentry.dll" : "export_census_fakevr.dll";
    fs::copy_file(build / fixture, stage / "openvr_api_orig.dll");
    std::ofstream ini(stage / "edvr.ini",std::ios::binary);
    ini << "[advanced]\nopenvr_census = " << (mode == "disabled" ? "off" : "on")
        << "\nsuppress_interfaces = IVROverlay\n[fix]\nvr_handover = stock\n";
    ini.close(); require(ini.good(), "fixture config");
    std::wstring command = L"\"" + (stage / "child.exe").wstring() + L"\" --child "; command.append(mode.begin(),mode.end());
    STARTUPINFOW si{}; si.cb = sizeof(si); PROCESS_INFORMATION pi{};
    require(CreateProcessW(nullptr,command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,stage.c_str(),&si,&pi) != FALSE,"spawn fixture child");
    const DWORD waited = WaitForSingleObject(pi.hProcess,20000);
    if (waited != WAIT_OBJECT_0) { TerminateProcess(pi.hProcess,124); WaitForSingleObject(pi.hProcess,5000); }
    DWORD code=1; GetExitCodeProcess(pi.hProcess,&code); CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    std::printf("export census %s: child=%lu, logs %ls\n",mode.c_str(),code,stage.c_str());
    require(waited == WAIT_OBJECT_0 && code == 0,"child completed successfully within 20 seconds");
    verifyLog(readLog(stage),mode);
}
int main(int argc,char** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    try {
        if (argc == 3 && std::strcmp(argv[1],"--child") == 0) return child(argv[2]);
        require((argc == 3 || argc == 4) && std::strcmp(argv[1],"--self-test") == 0,"usage: --self-test <build-dir> [--dry-run]");
        const fs::path build = fs::absolute(argv[2]);
        for (const char* f : {"openvr_api.dll","export_census_fakevr.dll","export_census_missing.dll","export_census_reentry.dll"})
            require(fs::is_regular_file(build / f),"required built proxy/fixture exists");
        if(argc == 4) { require(std::strcmp(argv[3],"--dry-run") == 0,"unknown option");
            std::puts("Would stage four isolated children and verify lifecycle forwarding/logs; no files written."); return 0; }
        for (const char* mode : {"enabled","disabled","missing","reentry"}) runCase(build,mode);
        std::puts("PASS: actual proxy preserves export ABI/results, suppression, null outputs, missing stubs and loader reentry; census is optional, bounded and paired.");
        return 0;
    } catch(const std::exception& e) { std::fprintf(stderr,"FAIL export census: %s\n",e.what()); return 1; }
}
