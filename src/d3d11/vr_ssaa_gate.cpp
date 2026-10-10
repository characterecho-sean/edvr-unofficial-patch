// The VR Supersampling gate, step 1 (vr_ssaa_gate.h says what and why). Log only.
#include "vr_ssaa_gate.h"

#include "device_hook.h"     // deviceHookFxcfgMultipliers: the newest .fxcfg's two floats (device_hook.cpp's eliteHmdMultiplier)
#include "ui_panel_scale.h"  // uiPanelScaleFactor, uiPanelScaleChosenSupersampling: lock-free
#include "ui_sizing_math.h"  // kUiPanelStamp, kUiPanelImageSize: build 332841
#include "ui_surfaces.h"     // uiSurfacesHmdQuality: lock-free
#include "vr_ssaa_hold.h"    // the hold's report, mode re-read and loader object (the hold changes values; this file only logs)
#include "vr_display_observer.h"  // the game window's frame check (read-only)

#include "../common/elite_graphics_folder.h"  // the Options\Graphics folder under %LOCALAPPDATA%
#include "../common/log.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace edvr {
namespace {

constexpr uint32_t kSetterLinesFirst = 64;  // every setter call logs while the count is at or under this
constexpr uint32_t kSetterLinesMax = 512;   // the hard cap on setter lines, change-only past the first 64

std::atomic<bool> g_on{false};              // build 332841 checked; every entry point below is a no-op until then
std::atomic<bool> g_started{false};         // vrSsaaGateStartup has run
std::atomic<uint32_t> g_presents{0};        // the game's Presents entered: the frame number the other lines carry
std::atomic<bool> g_presentLogged{false};
std::atomic<uint32_t> g_setterCalls{0};     // setter calls seen (the after side of each)
std::atomic<uint32_t> g_setterLines{0};     // setter lines written
std::atomic<uint32_t> g_beforeBits{0};      // float bits of the before value of the current setter call
std::atomic<uint32_t> g_lastPassed{0}, g_lastBefore{0}, g_lastAfter{0};  // float bits of the last setter line
std::atomic<bool> g_getterSeen{false};

uint32_t bitsOf(float v) {
    uint32_t b = 0;
    std::memcpy(&b, &v, sizeof(b));
    return b;
}

float floatOf(uint32_t b) {
    float v = 0.0f;
    std::memcpy(&v, &b, sizeof(v));
    return v;
}

// Seconds on the performance counter (boot-relative): the lines' clock, comparable across the lines of one run.
double nowSeconds() {
    LARGE_INTEGER freq{}, now{};
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    return freq.QuadPart ? static_cast<double>(now.QuadPart) / static_cast<double>(freq.QuadPart) : 0.0;
}

// The game's own PE header: build 332841's stamp and image size, the same gate ui_panel_scale.cpp applies. The guard
// holds no objects.
bool gameBuildChecked() {
    const uint8_t* base = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
    if (!base) return false;
    __try {
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
        return nt->FileHeader.TimeDateStamp == kUiPanelStamp && nt->OptionalHeader.SizeOfImage == kUiPanelImageSize;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// A small text file, read whole. False when missing or over 1 MB.
bool readSmallFile(const std::wstring& path, std::string* out) {
    out->clear();
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    const DWORD size = GetFileSize(f, nullptr);
    bool ok = size != INVALID_FILE_SIZE && size <= (1u << 20);
    if (ok) {
        std::string buf(size, '\0');
        DWORD got = 0;
        ok = size == 0 || ReadFile(f, &buf[0], size, &got, nullptr) != 0;
        if (ok) {
            buf.resize(got);
            *out = buf;
        }
    }
    CloseHandle(f);
    return ok;
}

// The text between <StereoscopicMode> and the next '<', trimmed; "absent" when the tag is not there.
std::string stereoscopicModeText(const std::string& xml) {
    static const char kTag[] = "<StereoscopicMode>";
    const size_t at = xml.find(kTag);
    if (at == std::string::npos) return "absent";
    const size_t from = at + std::strlen(kTag);
    const size_t end = xml.find('<', from);
    if (end == std::string::npos) return "unterminated";
    size_t a = from, b = end;
    while (a < b && (xml[a] == ' ' || xml[a] == '\t' || xml[a] == '\r' || xml[a] == '\n')) ++a;
    while (b > a && (xml[b - 1] == ' ' || xml[b - 1] == '\t' || xml[b - 1] == '\r' || xml[b - 1] == '\n')) --b;
    return a == b ? std::string("empty") : xml.substr(a, b - a);
}

std::string narrow(const std::wstring& w) {
    if (w.empty()) return std::string();
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), &out[0], n, nullptr, nullptr);
    return out;
}

}  // namespace

void vrSsaaGateStartup() {
    bool expected = false;
    if (!g_started.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) return;
    if (!gameBuildChecked()) {
        Log::get().note("vr ssaa gate: game build not checked; instruments off");
        vrSsaaHoldReport();
        return;
    }
    vrSsaaHoldReport();
    g_on.store(true, std::memory_order_release);

    // Settings.xml names the 3D mode (the live value is not located in this step; see the line below).
    wchar_t appdata[MAX_PATH] = {};
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", appdata, MAX_PATH);
    const std::wstring folder = (n == 0 || n >= MAX_PATH) ? std::wstring() : eliteGraphicsFolderUnder(appdata);
    const std::wstring settingsPath = folder.empty() ? std::wstring() : folder + L"\\Settings.xml";
    std::string xml;
    const bool haveXml = !settingsPath.empty() && readSmallFile(settingsPath, &xml);
    const std::string mode = haveXml ? stereoscopicModeText(xml) : std::string("absent");

    // The newest .fxcfg's two floats, through the one reader the device hook already uses (0 when absent).
    float mult = 0.0f, ssaa = 0.0f;
    char fxcfg[80] = {};
    deviceHookFxcfgMultipliers(&mult, &ssaa, fxcfg, sizeof(fxcfg));
    char multText[24], ssaaText[24];
    if (mult > 0.0f) std::snprintf(multText, sizeof(multText), "%.4f", static_cast<double>(mult));
    else std::snprintf(multText, sizeof(multText), "absent");
    if (ssaa > 0.0f) std::snprintf(ssaaText, sizeof(ssaaText), "%.4f", static_cast<double>(ssaa));
    else std::snprintf(ssaaText, sizeof(ssaaText), "absent");

    Log::get().note("vr ssaa gate: Settings.xml StereoscopicMode=%s (file %s%s); fxcfg %s SSAAMultiplier=%s "
                    "HMDRenderTargetMultiplier=%s; read at qpc=%.3f s",
                    mode.c_str(), narrow(settingsPath).c_str(), haveXml ? "" : ", not readable",
                    fxcfg[0] ? fxcfg : "none", ssaaText, multText, nowSeconds());
    Log::get().note("vr ssaa gate: live 3D mode not located; using the startup file value");
}

uint32_t vrSsaaGateFrame() { return g_presents.load(std::memory_order_relaxed); }

void vrSsaaGateNotePresent() {
    vrSsaaHoldFrameBoundary();  // the Supersampling hold's mode re-read (a small read, only when a setter call asked for one)
    vrDisplayObserveFrame();    // the game window's style and client size, one line when they change (read-only)
    const uint32_t frame = g_presents.fetch_add(1, std::memory_order_relaxed) + 1;
    if (!g_on.load(std::memory_order_acquire)) return;
    bool expected = false;
    if (!g_presentLogged.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) return;
    Log::get().note("vr ssaa gate: first Present at qpc=%.3f s (frame %u); setter calls so far %u, getter read %s",
                    nowSeconds(), frame, g_setterCalls.load(std::memory_order_relaxed),
                    g_getterSeen.load(std::memory_order_relaxed) ? "seen" : "not yet");
}

void vrSsaaGateNoteSetterBefore(float before) {
    if (!g_on.load(std::memory_order_acquire)) return;
    g_beforeBits.store(bitsOf(before), std::memory_order_relaxed);
}

void vrSsaaGateNoteSetterAfter(uintptr_t ctx, float passed, float after, float lo, float hi) {
    if (!g_on.load(std::memory_order_acquire)) return;
    const uint32_t seq = g_setterCalls.fetch_add(1, std::memory_order_relaxed) + 1;
    const uint32_t kPassed = bitsOf(passed), kBefore = g_beforeBits.load(std::memory_order_relaxed), kAfter = bitsOf(after);
    const bool changed = kPassed != g_lastPassed.load(std::memory_order_relaxed) ||
                         kBefore != g_lastBefore.load(std::memory_order_relaxed) ||
                         kAfter != g_lastAfter.load(std::memory_order_relaxed);
    const uint32_t lines = g_setterLines.load(std::memory_order_relaxed);
    if (!(seq <= kSetterLinesFirst || (changed && lines < kSetterLinesMax))) return;
    g_setterLines.store(lines + 1, std::memory_order_relaxed);
    g_lastPassed.store(kPassed, std::memory_order_relaxed);
    g_lastBefore.store(kBefore, std::memory_order_relaxed);
    g_lastAfter.store(kAfter, std::memory_order_relaxed);
    const char* source = "none";
    const double chosen = uiPanelScaleChosenSupersampling(&source);
    Log::get().note("vr ssaa gate: SS setter call %u%s: passed %.4f; ctx 0x%llX (loader object 0x%llX) ctx+0x3564 before "
                    "%.4f after %.4f (range %.2f to %.2f); HMD Quality %.3f; panel factor %.4f; chosen Supersampling %.4f "
                    "from %s; frame %u; qpc=%.3f s; sizing record not read by EDVR",
                    seq, seq == 1 ? " (first)" : "", static_cast<double>(passed),
                    static_cast<unsigned long long>(ctx), vrSsaaHoldLoaderObject(),
                    static_cast<double>(floatOf(kBefore)), static_cast<double>(after), static_cast<double>(lo),
                    static_cast<double>(hi), static_cast<double>(uiSurfacesHmdQuality()),
                    uiPanelScaleFactor(), chosen, source, g_presents.load(std::memory_order_relaxed),
                    nowSeconds());
}

bool vrSsaaGateGetterPending() {
    return g_on.load(std::memory_order_acquire) && !g_getterSeen.load(std::memory_order_relaxed);
}

void vrSsaaGateNoteGetter(float value) {
    if (!g_on.load(std::memory_order_acquire)) return;
    if (g_getterSeen.exchange(true, std::memory_order_acq_rel)) return;
    Log::get().note("vr ssaa gate: first SS getter read at qpc=%.3f s (frame %u): the game returns %.4f",
                    nowSeconds(), g_presents.load(std::memory_order_relaxed), static_cast<double>(value));
}

}  // namespace edvr
