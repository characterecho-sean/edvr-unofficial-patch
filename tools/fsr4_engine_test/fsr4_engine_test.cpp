// Rig for the FSR 4 engine (src\d3d11\fsr4_engine.cpp), modelled on tools\xess_engine_test. Three modes:
//
//   --dry-run                 prints the plan; no device, no file.
//   --self-test               GPU-free. The pure decisions of fsr4_version.h (which version AMD's runtime may run as FSR 4,
//                             the refusal text, the typed texture formats, the creation and dispatch flags, the camera
//                             planes, the frame time, the failure latch), then the engine's entries with no GPU: a slot out
//                             of range and a null device are refused without a crash, and a D3D11 device that supports
//                             nothing fails the probe once and is remembered. In a build without AMD's SDK it checks the
//                             other half: fsr4BuiltIn is false and every entry refuses by name.
//   --gpu-test-if-available   a hardware D3D11 device. fsr4Available true runs small LDR and HDR evaluations (128x128 to
//                             192x192), checks the output is not all zero, and checks the release and re-create counts. A
//                             clean refusal with a reason is a PASS and prints the reason: CI has no Radeon RX 7000 or 9000,
//                             where AMD's runtime offers only FSR 3.1.5 and fsr4 refuses by design (decision D7). Absent
//                             hardware skips. --gpu-test is the strict form: the device must exist and fsr4 must run.
//
// AMD's loader and upscaler DLLs are found beside this executable (the engine resolves them beside its own module), so the build
// stages them next to the rig as it stages libxess.dll for the XeSS rig; without them the GPU test is a refusal that names the
// missing file. The rig needs no AMD header: it sees only fsr4_engine.h and fsr4_version.h.
#include <windows.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <cfloat>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include "../../src/common/config.h"
#include "../../src/common/log.h"
#include "../../src/d3d11/fsr4_engine.h"
#include "../../src/d3d11/fsr4_version.h"
#include "../../src/d3d11/hdr_backend_flags.h"
#include "../xess_engine_test/unsupported_device.h"   // a D3D11 device that supports nothing; nothing in it is XeSS-specific

using Microsoft::WRL::ComPtr;

namespace edvr {
Log& Log::get() { static auto* log = new Log; return *log; }
void Log::note(const char* format, ...) {
    va_list args;
    va_start(args, format);
    std::vfprintf(stderr, format, args);
    va_end(args);
    std::fputc('\n', stderr);
}
// The engine reads diag.fsr4_shared_locks once when the backend becomes available; the rig runs it at its default.
Config& Config::get() { static Config config; return config; }
std::string Config::getString(const char*, const char* def) const { return def; }
}  // namespace edvr

namespace {
bool check(bool okay, const char* stage) {
    if (!okay) std::fprintf(stderr, "fsr4_engine_test: FAILED %s\n", stage);
    return okay;
}

using Names = std::vector<std::string>;

// ---- fsr4_version.h ----

bool pickCase(const Names& names, int expectIndex, const char* expectVersion, const char* stage) {
    edvr::Fsr4Version picked;
    const int index = edvr::fsr4PickVersion(names, &picked);
    if (!check(index == expectIndex, stage)) {
        std::fprintf(stderr, "fsr4_engine_test:   picked index %d, expected %d\n", index, expectIndex);
        return false;
    }
    if (expectIndex < 0) return true;
    return check(edvr::fsr4FormatVersion(picked) == expectVersion, stage);
}

bool versionSelfTest() {
    edvr::Fsr4Version v;
    if (!check(edvr::fsr4ParseVersion("3.1.5", &v) && v.major == 3 && v.minor == 1 && v.patch == 5, "parse 3.1.5") ||
        !check(edvr::fsr4ParseVersion("4.1", &v) && v.major == 4 && v.minor == 1 && v.patch == 0, "a missing patch reads as 0") ||
        !check(edvr::fsr4ParseVersion("FSR 4.1.1", &v) && v.major == 4 && v.minor == 1 && v.patch == 1, "a leading prefix is accepted") ||
        !check(edvr::fsr4ParseVersion("FSR3.1.5", &v) && v.major == 3 && v.minor == 1 && v.patch == 5, "a prefix with a digit in it is accepted") ||
        !check(edvr::fsr4ParseVersion("AMD FSR v4.0.3 (ML)", &v) && v.major == 4 && v.minor == 0 && v.patch == 3, "prefix and suffix are accepted") ||
        !check(edvr::fsr4ParseVersion("04.01.01", &v) && v.major == 4 && v.minor == 1 && v.patch == 1, "leading zeros read as numbers"))
        return false;
    const char* const garbage[] = {"", "unknown", "4", "v4", "1..2", "x.y.z", "1234567.1.1", "4.", ".4"};
    for (const char* name : garbage)
        if (!check(!edvr::fsr4ParseVersion(name), "a garbage name is not a version")) return false;
    if (!check(!edvr::fsr4ParseVersion(nullptr), "a null name is not a version")) return false;
    v = {9, 9, 9};
    if (!check(!edvr::fsr4ParseVersion("junk", &v) && v.major == 9 && v.minor == 9 && v.patch == 9, "a failed parse leaves its output")) return false;

    // D7: only a 4.x version may run as FSR 4.
    if (!pickCase({"3.1.5"}, -1, "", "only 3.1.5 offered: no pick") ||
        !pickCase({"3.1.5", "4.1.1"}, 1, "4.1.1", "3.1.5 and 4.1.1 offered: 4.1.1") ||
        !pickCase({"4.0.3", "4.1.1"}, 1, "4.1.1", "4.0.3 and 4.1.1 offered: 4.1.1") ||
        !pickCase({"4.1.1", "4.0.3"}, 0, "4.1.1", "the order AMD gives does not matter") ||
        !pickCase({"FSR 4.1.1"}, 0, "4.1.1", "a prefixed name is picked") ||
        !pickCase({"2.3.0", "3.1.5"}, -1, "", "FSR 2 and 3 only: no pick") ||
        !pickCase({"AMD FSR v3.1.5", "unknown", "", "4", "1..2"}, -1, "", "garbage names are ignored and leave no pick") ||
        !pickCase({"unknown", "4.0.2"}, 1, "4.0.2", "garbage beside a 4.x does not hide it") ||
        !pickCase({"4.1.1", "4.1.1"}, 0, "4.1.1", "two equal versions: the first") ||
        !pickCase({"4.1.0", "4.1.1", "4.0.9"}, 1, "4.1.1", "the patch decides within a minor") ||
        !pickCase({"4.9.0", "4.10.0"}, 1, "4.10.0", "numbers compare as numbers, not as text") ||
        !pickCase({"10.0.0", "4.1.1"}, 0, "10.0.0", "a later major wins") ||
        !pickCase({}, -1, "", "nothing offered: no pick"))
        return false;
    if (!check(edvr::fsr4PickVersion({"3.1.5"}) < 0, "pick without an output pointer")) return false;

    if (!check(edvr::fsr4FormatVersionList({"3.1.5", "4.1.1"}) == "3.1.5, 4.1.1", "list formatting") ||
        !check(edvr::fsr4FormatVersionList({"3.1.5"}) == "3.1.5", "one-item list formatting") ||
        !check(edvr::fsr4FormatVersionList({"FSR 4.1.1", "4.0"}) == "4.1.1, 4.0.0", "a list shows versions as major.minor.patch") ||
        !check(edvr::fsr4FormatVersionList({"3.1.5", "junk", ""}) == "3.1.5, junk, ?", "a name that is not a version is shown as it came") ||
        !check(edvr::fsr4FormatVersionList({}).empty(), "an empty list formats to nothing"))
        return false;

    const std::string only315 =
        "AMD's runtime offers only FSR 3.1.5 for this GPU; FSR 4 needs a Radeon RX 7000 or 9000 series GPU "
        "(set temporal_aa = fsr for FSR 3.1)";
    if (!check(edvr::fsr4RefusalText({"3.1.5"}, false) == only315, "the D7 refusal text") ||
        !check(edvr::fsr4RefusalText({"3.1.5"}, true) ==
                   only315 + " -- under Proton, launch Elite with FSR4_UPGRADE=1 %command% on a Proton that carries AMD's FSR 4 driver file",
               "under Wine the refusal adds the Proton launch option") ||
        !check(edvr::fsr4RefusalText({"2.3.0", "3.1.5"}, false).find("offers only FSR 2.3.0, 3.1.5 for this GPU") != std::string::npos,
               "the refusal lists everything offered") ||
        !check(edvr::fsr4RefusalText({}, false).find("AMD's runtime offers no FSR version for this GPU;") == 0,
               "the refusal for an empty offer says nothing was offered"))
        return false;

    // The belt-and-braces test on the provider a made context reports.
    if (!check(edvr::fsr4ProviderIsFour("4.1.1", 5, 5), "a 4.x provider passes") ||
        !check(!edvr::fsr4ProviderIsFour("3.1.5", 5, 5), "a 3.x provider fails even when its id is the requested one") ||
        !check(edvr::fsr4ProviderIsFour("", 7, 7), "an unnamed provider with the requested id passes") ||
        !check(!edvr::fsr4ProviderIsFour("", 7, 8), "an unnamed provider with another id fails") ||
        !check(!edvr::fsr4ProviderIsFour("", 0, 0), "an unnamed provider with no requested id fails") ||
        !check(edvr::fsr4ProviderIsFour(nullptr, 7, 7), "a null name is judged by its id"))
        return false;
    return true;
}

// ---- the key and its latch ----

bool latchSelfTest() {
    edvr::Fsr4FailureLatch latch;
    edvr::Fsr4ContextKey original;
    original.inW = 640; original.inH = 360; original.outW = 1280; original.outH = 720;
    original.colourFormat = 28; original.depthFormat = 41; original.motionFormat = 34; original.outFormat = 28;
    if (!check(!latch.matches(original), "new key is not failed")) return false;
    latch.record(original, "ffxCreateContext returned 4");
    if (!check(latch.matches(original) && std::strcmp(latch.reason(), "ffxCreateContext returned 4") == 0,
               "failed key keeps its precise reason")) return false;
    latch.clearIfDifferent(original);
    if (!check(latch.matches(original), "same key stays failed without retry")) return false;
    edvr::Fsr4ContextKey changes[10] = {original, original, original, original, original,
                                        original, original, original, original, original};
    ++changes[0].inW; ++changes[1].inH; ++changes[2].outW; ++changes[3].outH;
    changes[4].infiniteDepth = true; changes[5].hdr = true;
    ++changes[6].colourFormat; ++changes[7].depthFormat; ++changes[8].motionFormat; ++changes[9].outFormat;
    for (const auto& changed : changes) {
        latch.record(original, "unavailable");
        latch.clearIfDifferent(changed);
        if (!check(!latch.matches(original) && !latch.matches(changed),
                   "a size, depth, HDR or format change clears the failed key")) return false;
    }
    latch.record(original, "shared texture failed");
    latch.clear();
    if (!check(!latch.matches(original), "a mode reset clears the failed key")) return false;
    return check(original == original && !(original == changes[5]), "key equality");
}

// ---- formats ----

bool formatSelfTest() {
    using edvr::Fsr4Role;
    using edvr::fsr4TypedFormat;
    if (!check(fsr4TypedFormat(Fsr4Role::Colour, DXGI_FORMAT_R8G8B8A8_TYPELESS) == DXGI_FORMAT_R8G8B8A8_UNORM &&
               fsr4TypedFormat(Fsr4Role::Colour, DXGI_FORMAT_R8G8B8A8_UNORM) == DXGI_FORMAT_R8G8B8A8_UNORM &&
               fsr4TypedFormat(Fsr4Role::Colour, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) == DXGI_FORMAT_R8G8B8A8_UNORM &&
               fsr4TypedFormat(Fsr4Role::Colour, DXGI_FORMAT_R11G11B10_FLOAT) == DXGI_FORMAT_R11G11B10_FLOAT &&
               fsr4TypedFormat(Fsr4Role::Colour, DXGI_FORMAT_R16G16B16A16_TYPELESS) == DXGI_FORMAT_R16G16B16A16_FLOAT &&
               fsr4TypedFormat(Fsr4Role::Colour, DXGI_FORMAT_D32_FLOAT) == DXGI_FORMAT_UNKNOWN &&
               fsr4TypedFormat(Fsr4Role::Colour, DXGI_FORMAT_R16G16_FLOAT) == DXGI_FORMAT_UNKNOWN,
               "colour formats map to typed ones")) return false;
    if (!check(fsr4TypedFormat(Fsr4Role::Depth, DXGI_FORMAT_R32_FLOAT) == DXGI_FORMAT_R32_FLOAT &&
               fsr4TypedFormat(Fsr4Role::Depth, DXGI_FORMAT_R32_TYPELESS) == DXGI_FORMAT_R32_FLOAT &&
               fsr4TypedFormat(Fsr4Role::Depth, DXGI_FORMAT_D32_FLOAT) == DXGI_FORMAT_R32_FLOAT &&
               fsr4TypedFormat(Fsr4Role::Depth, DXGI_FORMAT_D24_UNORM_S8_UINT) == DXGI_FORMAT_UNKNOWN &&
               fsr4TypedFormat(Fsr4Role::Depth, DXGI_FORMAT_D32_FLOAT_S8X24_UINT) == DXGI_FORMAT_UNKNOWN &&
               fsr4TypedFormat(Fsr4Role::Depth, DXGI_FORMAT_R16_UNORM) == DXGI_FORMAT_UNKNOWN,
               "depth must be 32-bit float and maps typeless and D32 to R32_FLOAT")) return false;
    if (!check(fsr4TypedFormat(Fsr4Role::Motion, DXGI_FORMAT_R16G16_FLOAT) == DXGI_FORMAT_R16G16_FLOAT &&
               fsr4TypedFormat(Fsr4Role::Motion, DXGI_FORMAT_R16G16_TYPELESS) == DXGI_FORMAT_R16G16_FLOAT &&
               fsr4TypedFormat(Fsr4Role::Motion, DXGI_FORMAT_R32G32_FLOAT) == DXGI_FORMAT_R32G32_FLOAT &&
               fsr4TypedFormat(Fsr4Role::Motion, DXGI_FORMAT_R8G8_UNORM) == DXGI_FORMAT_UNKNOWN,
               "motion formats map to typed ones")) return false;
    return check(fsr4TypedFormat(Fsr4Role::Output, DXGI_FORMAT_R16G16B16A16_FLOAT) == DXGI_FORMAT_R16G16B16A16_FLOAT &&
                 fsr4TypedFormat(Fsr4Role::Output, DXGI_FORMAT_R8G8B8A8_TYPELESS) == DXGI_FORMAT_R8G8B8A8_UNORM &&
                 fsr4TypedFormat(Fsr4Role::Output, DXGI_FORMAT_R8G8B8A8_UNORM) == DXGI_FORMAT_R8G8B8A8_UNORM &&
                 fsr4TypedFormat(Fsr4Role::Output, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) == DXGI_FORMAT_UNKNOWN &&
                 fsr4TypedFormat(Fsr4Role::Output, DXGI_FORMAT_R11G11B10_FLOAT) == DXGI_FORMAT_UNKNOWN,
                 "output formats are the ones a typed UAV stores to");
}

// ---- flags ----

// AMD's real bit values (FidelityFX SDK v2.3.0 ffx_upscale.h), written out as numbers: the engine static_asserts the same
// constants against AMD's enum, so a drift in either side fails a build with the SDK, and this rig pins what the flags mean
// in a build without it.
bool flagsSelfTest() {
    constexpr uint32_t kHdr = 1u << 0, kInverted = 1u << 3, kInfinite = 1u << 4, kAuto = 1u << 5, kNonLinear = 1u << 8;
    if (!check(edvr::fsr4CreateFlags(false, false) == (kInverted | kAuto | kNonLinear),
               "LDR creation flags: reversed Z, automatic exposure and non-linear colour") ||
        !check(edvr::fsr4CreateFlags(true, false) == (kInverted | kInfinite | kAuto | kNonLinear), "LDR infinite depth adds the infinite bit") ||
        !check(edvr::fsr4CreateFlags(false, true) == (kHdr | kInverted | kAuto), "HDR creation flags: HDR, reversed Z, automatic exposure, no non-linear bit") ||
        !check(edvr::fsr4CreateFlags(true, true) == (kHdr | kInverted | kInfinite | kAuto), "HDR infinite depth"))
        return false;
    return check(edvr::fsr4DispatchFlags(false) == (1u << 1) && edvr::fsr4DispatchFlags(true) == 0u,
                 "dispatch declares LDR colour sRGB and leaves HDR alone");
}

// ---- camera and time ----

bool cameraSelfTest() {
    float n = 0.0f, f = 0.0f;
    const char* why = nullptr;
    if (!check(edvr::fsr4CameraPlanes(0.1f, 1000.0f, false, &n, &f, &why) && n == 1000.0f && f == 0.1f, "the larger plane is cameraNear") ||
        !check(edvr::fsr4CameraPlanes(1000.0f, 0.1f, false, &n, &f, &why) && n == 1000.0f && f == 0.1f, "the order given does not matter") ||
        !check(edvr::fsr4CameraPlanes(0.0f, 0.0f, false, &n, &f, &why) && n == edvr::kFsr4DefaultFarZ && f == edvr::kFsr4DefaultNearZ,
               "unknown planes take the documented defaults") ||
        !check(edvr::fsr4CameraPlanes(0.1f, 0.0f, false, &n, &f, &why) && n == edvr::kFsr4DefaultFarZ && f == edvr::kFsr4DefaultNearZ,
               "an unknown far plane takes both defaults, as route 1 does") ||
        !check(edvr::fsr4CameraPlanes(0.1f, 123.0f, true, &n, &f, &why) && n == FLT_MAX && f == 0.1f,
               "infinite depth: cameraNear FLT_MAX, cameraFar the near plane"))
        return false;
    why = nullptr;
    if (!check(!edvr::fsr4CameraPlanes(0.0f, 0.0f, true, &n, &f, &why) && why && std::strstr(why, "near plane"),
               "infinite depth without a near plane is refused with a reason")) return false;
    const float nan = std::numeric_limits<float>::quiet_NaN();
    if (!check(!edvr::fsr4CameraPlanes(nan, 0.0f, true, &n, &f, &why), "infinite depth with a NaN near plane is refused") ||
        !check(edvr::fsr4CameraPlanes(nan, nan, false, &n, &f, &why) && n == edvr::kFsr4DefaultFarZ, "NaN planes read as unknown"))
        return false;
    return check(edvr::fsr4FrameTimeMs(0.0f) == 11.1f && edvr::fsr4FrameTimeMs(-3.0f) == 11.1f && edvr::fsr4FrameTimeMs(nan) == 11.1f,
                 "an unknown frame time is a nominal 11.1 ms") &&
           check(edvr::fsr4FrameTimeMs(0.05f) == 0.1f && edvr::fsr4FrameTimeMs(500.0f) == 100.0f && edvr::fsr4FrameTimeMs(16.6f) == 16.6f,
                 "a real frame time keeps the 0.1 to 100 ms clamp");
}

// ---- the engine's entries, with no GPU ----

// Built without AMD's SDK every entry refuses, naming the macro, and nothing crashes.
bool notBuiltSelfTest() {
    const char* why = nullptr;
    if (!check(!edvr::fsr4BuiltIn(), "a build without the SDK reports it")) return false;
    if (!check(!edvr::fsr4Available(nullptr, &why) && why && std::strstr(why, "EDVR_HAVE_FSR4"), "fsr4Available refuses by name")) return false;
    why = nullptr;
    if (!check(!edvr::fsr4Evaluate(nullptr, 0, nullptr, nullptr, nullptr, nullptr, 1, 1, 1, 1, 0.0f, 0.0f, false, 0.0f, 0.0f, 0.0f,
                                   0.0f, &why) && why && std::strstr(why, "EDVR_HAVE_FSR4"), "fsr4Evaluate refuses by name")) return false;
    why = nullptr;
    if (!check(!edvr::fsr4Evaluate(nullptr, 3, nullptr, nullptr, nullptr, nullptr, 1, 1, 1, 1, 0.0f, 0.0f, false, 0.0f, 0.0f, 0.0f,
                                   0.0f, &why) && why && std::strstr(why, "EDVR_HAVE_FSR4"), "fsr4Evaluate refuses a bad slot by name too")) return false;
    uint32_t evaluations = 0, resets = 0;
    double avg = 0.0, max = 0.0;
    return check(!edvr::fsr4SlotTotals(0, &evaluations, &resets, &avg, &max) &&
                 edvr::fsr4TestContextCreations(0) == 0 && !edvr::fsr4TestContextLive(0),
                 "no totals, no contexts and no creations without the SDK");
}

// The slot check runs before any device use, so a null context proves the range without a GPU: slot 3 is refused by range, and
// slot 2 (the last of kUpscalerSlots = 3) passes it and stops at the context check.
bool slotRangeSelfTest() {
    const char* why = nullptr;
    const bool refusedFourth = !edvr::fsr4Evaluate(nullptr, 3, nullptr, nullptr, nullptr, nullptr, 1, 1, 1, 1, 0.0f, 0.0f, false,
                                                   0.0f, 0.0f, 0.0f, 0.0f, &why);
    if (!check(refusedFourth && why && std::strstr(why, "slot 3") != nullptr, "upscaler slot 3 is refused by range")) return false;
    why = nullptr;
    const bool acceptedLast = !edvr::fsr4Evaluate(nullptr, 2, nullptr, nullptr, nullptr, nullptr, 1, 1, 1, 1, 0.0f, 0.0f, false,
                                                  0.0f, 0.0f, 0.0f, 0.0f, &why);
    if (!check(acceptedLast && why && std::strstr(why, "invalid FSR 4") != nullptr, "upscaler slot 2 passes the range check")) return false;
    return check(!edvr::fsr4TestContextLive(edvr::kUpscalerSlots) && edvr::fsr4TestContextCreations(edvr::kUpscalerSlots) == 0,
                 "the test seams answer nothing for a slot out of range");
}

// A null device is refused with a reason and no crash, and the refusal is remembered rather than redone.
bool nullDeviceSelfTest() {
    edvr::fsr4Shutdown();
    const char* why = nullptr;
    if (!check(!edvr::fsr4Available(nullptr, &why) && why && why[0], "a null device is refused with a reason")) return false;
    const char* again = nullptr;
    if (!check(!edvr::fsr4Available(nullptr, &again) && again && std::strcmp(again, why) == 0, "the same refusal is said again")) return false;
    edvr::fsr4Shutdown();
    return true;
}

// A failed availability probe belongs to the device it ran on. The first probe step, the shared-fence interface, fails on
// UnsupportedD3D11Device, which counts the QueryInterface calls it receives. The same device offered again is a cached failure
// and asks nothing of it; a different device is probed afresh, with no fsr4Shutdown between. GPU-free.
bool failedProbeSelfTest() {
    edvr::fsr4Shutdown();
    UnsupportedD3D11Device a, b;
    const char* why = nullptr;
    if (!check(!edvr::fsr4Available(&a, &why) && why && std::strstr(why, "shared fences"),
               "a device without shared fences fails the first probe")) return false;
    const char* firstWhy = why;
    if (!check(a.queries == 1 && a.refs == 2, "the failed probe asked the device once and holds a reference to it")) return false;
    if (!check(!edvr::fsr4Available(&a, &why) && a.queries == 1 && why == firstWhy,
               "the same device offered again is a cached failure, not a second probe")) return false;
    if (!check(!edvr::fsr4Available(&b, &why) && b.queries == 1,
               "a different device is probed afresh after a failed probe")) return false;
    if (!check(a.refs == 1 && b.refs == 2, "the retry released the first failed device and holds the second")) return false;
    edvr::fsr4Shutdown();
    return check(b.refs == 1, "shutdown released the failed device");
}

bool entriesSelfTest() {
    edvr::fsr4ReleaseFeatures();   // safe when FSR 4 never started
    edvr::fsr4Shutdown();
    if (!check(std::strcmp(edvr::fsr4VersionLabel(), "fsr4") == 0, "the label is fsr4 before any context exists")) return false;
    if (!edvr::fsr4BuiltIn()) return notBuiltSelfTest();
    uint32_t evaluations = 0, resets = 0;
    double avg = 0.0, max = 0.0;
    return slotRangeSelfTest() && nullDeviceSelfTest() && failedProbeSelfTest() &&
           check(!edvr::fsr4SlotTotals(0, &evaluations, &resets, &avg, &max), "no totals before a slot has evaluated");
}

bool selfTest() {
    const bool good = versionSelfTest() && latchSelfTest() && formatSelfTest() && flagsSelfTest() && cameraSelfTest() &&
                      entriesSelfTest();
    edvr::fsr4Shutdown();
    if (good) std::fprintf(stderr, "fsr4_engine_test: self-test PASS (FSR 4 SDK %s in this build)\n",
                           edvr::fsr4BuiltIn() ? "compiled" : "not compiled");
    return good;
}

// ---- hardware ----

enum class Probe { Ready, Skip, Fail };

bool missingCapability(HRESULT hr) {
    return hr == DXGI_ERROR_UNSUPPORTED || hr == DXGI_ERROR_NOT_FOUND;
}

Probe hardwareAdapterPresent() {
    ComPtr<IDXGIFactory1> factory;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(factory.GetAddressOf()));
    if (FAILED(hr)) {
        std::fprintf(stderr, "fsr4_engine_test: FAILED DXGI factory 0x%08X\n", unsigned(hr));
        return Probe::Fail;
    }
    for (UINT index = 0;; ++index) {
        ComPtr<IDXGIAdapter1> adapter;
        hr = factory->EnumAdapters1(index, adapter.GetAddressOf());
        if (hr == DXGI_ERROR_NOT_FOUND) break;
        if (FAILED(hr)) {
            std::fprintf(stderr, "fsr4_engine_test: FAILED DXGI adapter enumeration 0x%08X\n", unsigned(hr));
            return Probe::Fail;
        }
        DXGI_ADAPTER_DESC1 desc{};
        hr = adapter->GetDesc1(&desc);
        if (FAILED(hr)) {
            std::fprintf(stderr, "fsr4_engine_test: FAILED DXGI adapter description 0x%08X\n", unsigned(hr));
            return Probe::Fail;
        }
        if (!(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) return Probe::Ready;
    }
    std::puts("fsr4_engine_test: SKIP no hardware DXGI adapter");
    return Probe::Skip;
}

ComPtr<ID3D11Texture2D> texture(ID3D11Device* device, UINT width, UINT height, DXGI_FORMAT format, UINT binds) {
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = binds;
    ComPtr<ID3D11Texture2D> result;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, result.GetAddressOf()))) return {};
    return result;
}

bool clear(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* target, const FLOAT color[4]) {
    ComPtr<ID3D11RenderTargetView> view;
    if (FAILED(device->CreateRenderTargetView(target, nullptr, view.GetAddressOf()))) return false;
    context->ClearRenderTargetView(view.Get(), color);
    return true;
}

// Clears a typeless 32-bit depth texture (bound as a depth-stencil) to a depth through a D32_FLOAT view.
bool clearDepth(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* target, float depth) {
    D3D11_DEPTH_STENCIL_VIEW_DESC desc{};
    desc.Format = DXGI_FORMAT_D32_FLOAT;
    desc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    ComPtr<ID3D11DepthStencilView> view;
    if (FAILED(device->CreateDepthStencilView(target, &desc, view.GetAddressOf()))) return false;
    context->ClearDepthStencilView(view.Get(), D3D11_CLEAR_DEPTH, depth, 0);
    return true;
}

// Whether any byte of the texture is nonzero (read back through a staging copy). The whole image, so an output FSR 4 wrote
// anywhere passes and an output nobody wrote does not.
bool anyNonZero(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* source) {
    D3D11_TEXTURE2D_DESC desc{};
    source->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = desc.MiscFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, staging.GetAddressOf()))) return false;
    context->CopyResource(staging.Get(), source);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return false;
    const size_t rowBytes = size_t(desc.Width) * (desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT ? 8 : 4);
    bool any = false;
    for (UINT row = 0; row < desc.Height && !any; ++row) {
        const auto* bytes = static_cast<const unsigned char*>(mapped.pData) + size_t(row) * mapped.RowPitch;
        for (size_t i = 0; i < rowBytes; ++i) any |= bytes[i] != 0;
    }
    context->Unmap(staging.Get(), 0);
    return any;
}

struct FlightSpec {
    UINT w = 128, h = 128, outW = 192, outH = 192;
    bool hdr = false;
    bool infinite = false;
    UINT pad = 0;               // the render-grid textures are (w + pad) x (h + pad): the engine box-copies the corner
    bool typelessDepth = false; // depth is R32_TYPELESS with a depth-stencil bind (a CopyResource of a depth texture)
    unsigned slot = 0;
    unsigned frames = 5;
};

// One slot, a few frames over a constant scene (colour 0.25/0.5/0.75, far reversed-Z depth, no motion), jittered. True when every
// frame was accepted and the output holds something. A refusal comes back through failureReason.
bool flight(ID3D11Device* device, ID3D11DeviceContext* context, const FlightSpec& spec, const char** failureReason = nullptr) {
    if (failureReason) *failureReason = nullptr;
    const DXGI_FORMAT colourFormat = spec.hdr ? DXGI_FORMAT_R11G11B10_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM;
    const DXGI_FORMAT outFormat = spec.hdr ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM;
    const UINT inBinds = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    const UINT outBinds = D3D11_BIND_RENDER_TARGET | D3D11_BIND_UNORDERED_ACCESS;
    const UINT gridW = spec.w + spec.pad, gridH = spec.h + spec.pad;
    auto colour = texture(device, gridW, gridH, colourFormat, inBinds);
    auto depth = spec.typelessDepth
        ? texture(device, gridW, gridH, DXGI_FORMAT_R32_TYPELESS, D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE)
        : texture(device, gridW, gridH, DXGI_FORMAT_R32_FLOAT, inBinds);
    auto motion = texture(device, gridW, gridH, DXGI_FORMAT_R16G16_FLOAT, inBinds);
    auto output = texture(device, spec.outW, spec.outH, outFormat, outBinds);
    if (!check(colour && depth && motion && output, "create input/output textures")) return false;
    constexpr FLOAT lit[4] = {0.25f, 0.5f, 0.75f, 1.0f};
    constexpr FLOAT zero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    const bool depthCleared = spec.typelessDepth ? clearDepth(device, context, depth.Get(), 0.0f)
                                                 : clear(device, context, depth.Get(), zero);
    if (!check(clear(device, context, colour.Get(), lit) && depthCleared && clear(device, context, motion.Get(), zero) &&
               clear(device, context, output.Get(), zero), "clear render-grid inputs and output"))
        return false;
    for (unsigned frame = 0; frame < spec.frames; ++frame) {
        const char* why = nullptr;
        const float jx = (frame % 2) ? 0.25f : -0.25f, jy = (frame % 2) ? -0.125f : 0.125f;   // never a null vector
        if (!edvr::fsr4Evaluate(context, spec.slot, colour.Get(), depth.Get(), motion.Get(), output.Get(), spec.w, spec.h,
                                spec.outW, spec.outH, jx, jy, frame == 0, 11.1f, 0.1f, 1000.0f, 1.0f, &why, spec.infinite, spec.hdr)) {
            if (failureReason) *failureReason = why;
            std::fprintf(stderr, "fsr4_engine_test: frame %u refused: %s\n", frame, why ? why : "unspecified");
            return false;
        }
    }
    if (!check(anyNonZero(device, context, output.Get()), "D3D11 readback of the output holds a nonzero byte")) return false;
    std::fprintf(stderr, "fsr4_engine_test: PASS %s%s%s%s %ux%u -> %ux%u, %u frames slot=%u\n", spec.hdr ? "HDR" : "LDR",
                 spec.infinite ? " infinite-depth" : "", spec.pad ? " box-copy" : "", spec.typelessDepth ? " typeless-depth" : "",
                 spec.w, spec.h, spec.outW, spec.outH, spec.frames, spec.slot);
    return true;
}

// A depth texture bound as a depth-stencil and larger than the render size cannot be box-copied, so it is refused with a reason
// that names it, and nothing is queued.
bool depthStencilRefusal(ID3D11Device* device, ID3D11DeviceContext* context) {
    const UINT inBinds = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    auto colour = texture(device, 144, 144, DXGI_FORMAT_R8G8B8A8_UNORM, inBinds);
    auto depth = texture(device, 144, 144, DXGI_FORMAT_R32_TYPELESS, D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE);
    auto motion = texture(device, 144, 144, DXGI_FORMAT_R16G16_FLOAT, inBinds);
    auto output = texture(device, 192, 192, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_RENDER_TARGET | D3D11_BIND_UNORDERED_ACCESS);
    if (!check(colour && depth && motion && output, "create oversized depth-stencil case")) return false;
    const char* why = nullptr;
    const bool refused = !edvr::fsr4Evaluate(context, 0, colour.Get(), depth.Get(), motion.Get(), output.Get(), 128, 128, 192, 192,
                                             0.25f, 0.25f, false, 11.1f, 0.1f, 1000.0f, 1.0f, &why);
    return check(refused && why && std::strstr(why, "depth-stencil") != nullptr,
                 "a depth-stencil bound depth larger than the render size is refused with a reason");
}

// Releasing a slot frees its context without counting a rebuild; the next evaluation at the same key builds one more; a new
// size builds one more again. Counted from the backend's start, so every check is against a baseline taken after the release.
bool releaseAndRecreate(ID3D11Device* device, ID3D11DeviceContext* context) {
    edvr::fsr4ReleaseFeatures();
    const uint32_t base0 = edvr::fsr4TestContextCreations(0);
    const uint32_t base1 = edvr::fsr4TestContextCreations(1);
    if (!check(!edvr::fsr4TestContextLive(0) && !edvr::fsr4TestContextLive(1), "release leaves no slot live")) return false;
    FlightSpec spec;
    if (!flight(device, context, spec)) return false;
    if (!check(edvr::fsr4TestContextLive(0) && !edvr::fsr4TestContextLive(1) &&
               edvr::fsr4TestContextCreations(0) == base0 + 1 && edvr::fsr4TestContextCreations(1) == base1,
               "slot 0 builds alone")) return false;
    if (!flight(device, context, spec)) return false;
    if (!check(edvr::fsr4TestContextCreations(0) == base0 + 1, "the same key keeps its context")) return false;
    FlightSpec other = spec;
    other.slot = 1;
    other.w = 160; other.h = 160; other.outW = 240; other.outH = 240;
    if (!flight(device, context, other)) return false;
    if (!check(edvr::fsr4TestContextLive(0) && edvr::fsr4TestContextLive(1) &&
               edvr::fsr4TestContextCreations(0) == base0 + 1 && edvr::fsr4TestContextCreations(1) == base1 + 1,
               "slot 1 at another size builds without rebuilding slot 0")) return false;
    FlightSpec resized = spec;
    resized.outW = 256; resized.outH = 256;
    if (!flight(device, context, resized)) return false;
    if (!check(edvr::fsr4TestContextCreations(0) == base0 + 2 && edvr::fsr4TestContextCreations(1) == base1 + 1,
               "a new output size remakes the slot's context and no other")) return false;
    FlightSpec flipped = resized;
    flipped.hdr = true;
    if (!flight(device, context, flipped)) return false;
    if (!check(edvr::fsr4TestContextCreations(0) == base0 + 3, "flipping the HDR route remakes the context")) return false;
    const char* refusal = nullptr;
    FlightSpec outOfRange = spec;
    outOfRange.slot = edvr::kUpscalerSlots;
    if (!check(!flight(device, context, outOfRange, &refusal) && refusal && std::strstr(refusal, "slot 3") != nullptr,
               "an upscaler slot out of range is refused on the GPU path")) return false;
    edvr::fsr4ReleaseFeatures();
    if (!check(!edvr::fsr4TestContextLive(0) && !edvr::fsr4TestContextLive(1) &&
               edvr::fsr4TestContextCreations(0) == base0 + 3 && edvr::fsr4TestContextCreations(1) == base1 + 1,
               "release frees every slot without counting a rebuild")) return false;
    if (!flight(device, context, spec)) return false;
    return check(edvr::fsr4TestContextLive(0) && edvr::fsr4TestContextCreations(0) == base0 + 4,
                 "a released slot rebuilds on its next use");
}

// Initial availability fails on a device that supports nothing, then a supported device is offered with no fsr4Shutdown between:
// it must come up and run.
bool rebindAfterFailedProbe(ID3D11Device* device, ID3D11DeviceContext* context) {
    edvr::fsr4Shutdown();
    UnsupportedD3D11Device unsupported;
    const char* why = nullptr;
    if (!check(!edvr::fsr4Available(&unsupported, &why), "initial availability fails on the first device")) return false;
    why = nullptr;
    if (!check(edvr::fsr4Available(device, &why),
               "a supported device is probed after a failed probe, with no fsr4Shutdown between")) return false;
    return flight(device, context, FlightSpec{});
}

// The GPU half. A refusal with a reason is a pass unless strict.
int gpuTest(bool optional) {
    if (!edvr::fsr4BuiltIn()) {
        std::puts("fsr4_engine_test: SKIP no AMD FSR 4 SDK in this build; the self-test covers the refusal");
        return 0;
    }
    if (optional) {
        const Probe adapters = hardwareAdapterPresent();
        if (adapters != Probe::Ready) return adapters == Probe::Skip ? 0 : 1;
    }
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL level{};
    const HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                                         device.GetAddressOf(), &level, context.GetAddressOf());
    if (optional && missingCapability(hr)) {
        std::fprintf(stdout, "fsr4_engine_test: SKIP hardware D3D11 device unavailable 0x%08X\n", unsigned(hr));
        return 0;
    }
    if (!check(SUCCEEDED(hr), "hardware D3D11 device")) return 1;
    if (optional && level < D3D_FEATURE_LEVEL_11_0) {
        std::puts("fsr4_engine_test: SKIP hardware D3D11 feature level below 11_0");
        return 0;
    }
    if (!check(level >= D3D_FEATURE_LEVEL_11_0, "D3D11 feature level 11_0")) return 1;

    edvr::fsr4Shutdown();
    const char* why = nullptr;
    if (!edvr::fsr4Available(device.Get(), &why)) {
        // The expected answer on every GPU AMD's runtime does not offer a 4.x provider for. It must say why.
        if (!check(why && why[0], "a refusal carries a reason")) return 1;
        if (!optional) {
            std::fprintf(stderr, "fsr4_engine_test: FAILED fsr4 unavailable on this GPU: %s\n", why);
            edvr::fsr4Shutdown();
            return 1;
        }
        std::fprintf(stdout, "fsr4_engine_test: PASS clean refusal (no 4.x provider here, or no AMD runtime beside the rig): %s\n", why);
        // The refusal is remembered with the device, not redone, and evaluating through it refuses with the same words.
        const char* again = nullptr;
        const bool cached = !edvr::fsr4Available(device.Get(), &again) && again && std::strcmp(again, why) == 0;
        FlightSpec spec;
        const char* evaluateWhy = nullptr;
        const bool refused = !flight(device.Get(), context.Get(), spec, &evaluateWhy) && evaluateWhy && evaluateWhy[0];
        edvr::fsr4Shutdown();
        return check(cached, "the refusal is cached for the device") && check(refused, "fsr4Evaluate refuses with a reason too") ? 0 : 1;
    }
    std::fprintf(stderr, "fsr4_engine_test: fsr4 available on this GPU, label \"%s\"\n", edvr::fsr4VersionLabel());

    FlightSpec ldr;
    FlightSpec hdr = ldr;
    hdr.hdr = true;
    FlightSpec infinite = ldr;
    infinite.infinite = true;
    FlightSpec boxed = ldr;
    boxed.pad = 16;
    FlightSpec typeless = ldr;
    typeless.typelessDepth = true;
    bool good = flight(device.Get(), context.Get(), ldr) && flight(device.Get(), context.Get(), hdr) &&
                flight(device.Get(), context.Get(), infinite) && flight(device.Get(), context.Get(), boxed) &&
                flight(device.Get(), context.Get(), typeless);
    // The provider label is what the price line prints: "fsr 4.x.y" once a context has reported its provider.
    good = good && check(std::strncmp(edvr::fsr4VersionLabel(), "fsr 4", 5) == 0, "the version label names the 4.x provider");
    uint32_t evaluations = 0, resets = 0;
    double avg = 0.0, max = 0.0;
    good = good && check(edvr::fsr4SlotTotals(0, &evaluations, &resets, &avg, &max) && evaluations >= 5 && avg > 0.0 && max >= avg,
                         "slot 0 reports its totals");
    good = good && depthStencilRefusal(device.Get(), context.Get());
    good = good && releaseAndRecreate(device.Get(), context.Get());
    good = good && rebindAfterFailedProbe(device.Get(), context.Get());
    edvr::fsr4Shutdown();
    return good ? 0 : 1;
}
}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    if (std::strcmp(argv[1], "--dry-run") == 0) {
        std::puts("fsr4_engine_test: plan -- --self-test: version pick (only 3.1.5 refuses; 3.1.5 and 4.1.1 picks 4.1.1; prefixed and garbage names), "
                  "refusal text, typed formats, creation and dispatch flags, camera planes, frame time, failure latch, slot range, "
                  "null device, failed probe; --gpu-test-if-available: availability or a clean refusal, then LDR and HDR 128x128 -> 192x192 "
                  "with a nonzero-output readback, box copy, release and re-create counts. No file or device is created by --dry-run.");
        return 0;
    }
    if (std::strcmp(argv[1], "--self-test") == 0) return selfTest() ? 0 : 1;
    if (std::strcmp(argv[1], "--gpu-test-if-available") == 0) return gpuTest(true);
    if (std::strcmp(argv[1], "--gpu-test") == 0) return gpuTest(false);
    return 2;
}
