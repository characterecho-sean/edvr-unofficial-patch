#include "../common/native_frame.h"

#include "../common/config.h"
#include "../common/frame_flag.h"
#include "../common/log.h"
#include "../common/openxr_resolution_entries.h"
#include "native_render_labels.h"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace {
constexpr unsigned kPoolSize = 16;
constexpr float kPi = 3.14159265358979323846f;

struct State {
    ID3D11Device* device = nullptr; // borrowed; the host owns its lifetime
    uint64_t generation = 0;
    uint64_t sequenceFloor = 0;
    bool active = false;
    bool invalidated = false;
    bool began = false;
    bool latched = false;
    bool consumerAnnounced = false;
    bool held = false;
    bool transitionEnabled = true;
    bool configNoted = false;
    uint32_t beginCount = 0;
    uint32_t latchCount = 0;
    uint32_t invalidationCount = 0;
    uint32_t lastOffsetEnabled = 0;
    uint32_t lastTransitionEnabled = 0;
    uint32_t lastResubmitEnabled = 0;
    uint32_t lastCullMode = 0;
    EdvrNativeFrameDecision cachedDecision{};
};

State g_pool[kPoolSize];
unsigned g_used = 0;
State* g_current = nullptr;
std::mutex g_mutex;

State* identify(void* context) {
    for (unsigned i = 0; i < g_used; ++i) {
        if (context == &g_pool[i]) return &g_pool[i];
    }
    return nullptr;
}

bool finiteValues(const float* values, size_t count) {
    if (!values) return false;
    for (size_t i = 0; i < count; ++i) {
        if (!std::isfinite(values[i])) return false;
    }
    return true;
}

bool sameDevice(ID3D11Device* expected, void* candidate) {
    if (!expected || !candidate) return false;
    IUnknown* a = nullptr;
    IUnknown* b = nullptr;
    const bool aOk = SUCCEEDED(expected->QueryInterface(
        IID_IUnknown, reinterpret_cast<void**>(&a)));
    const bool bOk = SUCCEEDED(static_cast<IUnknown*>(candidate)->QueryInterface(
        IID_IUnknown, reinterpret_cast<void**>(&b)));
    const bool equal = aOk && bOk && a == b;
    if (a) a->Release();
    if (b) b->Release();
    return equal;
}

bool rigidPose(const float* m) {
    if (!finiteValues(m, 12)) return false;
    constexpr float kTolerance = 0.0025f;
    for (int r = 0; r < 3; ++r) {
        float norm = 0.0f;
        for (int c = 0; c < 3; ++c) norm += m[r * 4 + c] * m[r * 4 + c];
        if (std::fabs(norm - 1.0f) > kTolerance) return false;
    }
    for (int a = 0; a < 3; ++a) {
        for (int b = a + 1; b < 3; ++b) {
            float dot = 0.0f;
            for (int c = 0; c < 3; ++c) dot += m[a * 4 + c] * m[b * 4 + c];
            if (std::fabs(dot) > kTolerance) return false;
        }
    }
    const float determinant =
        m[0] * (m[5] * m[10] - m[6] * m[9]) -
        m[1] * (m[4] * m[10] - m[6] * m[8]) +
        m[2] * (m[4] * m[9] - m[5] * m[8]);
    return std::fabs(determinant - 1.0f) <= 0.004f;
}

float boundedOffset(float value) {
    if (!std::isfinite(value)) return 0.0f;
    if (value < -10.0f) return -10.0f;
    return value > 10.0f ? 10.0f : value;
}

float wrappedYawRadians(float degrees) {
    if (!std::isfinite(degrees)) degrees = 0.0f;
    return std::fmod(degrees, 360.0f) * kPi / 180.0f;
}

uint32_t cullMode(const std::string& value) {
    if (_stricmp(value.c_str(), "symmetric") == 0) return 1;
    if (_stricmp(value.c_str(), "percent") == 0) return 2;
    return 0;
}

bool parseSignature(const std::string& token, uint32_t* width,
                    uint32_t* height) {
    if (!width || !height) return false;
    size_t p = 0;
    while (p < token.size() && std::isspace(static_cast<unsigned char>(token[p]))) ++p;
    const size_t widthStart = p;
    uint64_t w = 0;
    while (p < token.size() && token[p] >= '0' && token[p] <= '9') {
        w = w * 10 + static_cast<unsigned>(token[p] - '0');
        if (w > 0xffffffffu) return false;
        ++p;
    }
    if (p == widthStart || p >= token.size() || token[p] != 'x') return false;
    ++p;
    const size_t heightStart = p;
    uint64_t h = 0;
    while (p < token.size() && token[p] >= '0' && token[p] <= '9') {
        h = h * 10 + static_cast<unsigned>(token[p] - '0');
        if (h > 0xffffffffu) return false;
        ++p;
    }
    if (p == heightStart) return false;
    while (p < token.size() && std::isspace(static_cast<unsigned char>(token[p]))) ++p;
    if (p != token.size() || w <= 10 || w >= 360 || h <= 10 || h >= 360) return false;
    *width = static_cast<uint32_t>(w);
    *height = static_cast<uint32_t>(h);
    return true;
}

void readSignatures(const std::string& raw, EdvrNativeFrameOutput* output) {
    size_t begin = 0;
    while (begin <= raw.size() && output->cullSignatureCount < 8) {
        size_t end = raw.find(',', begin);
        if (end == std::string::npos) end = raw.size();
        uint32_t width = 0, height = 0;
        if (parseSignature(raw.substr(begin, end - begin), &width, &height)) {
            const uint32_t index = output->cullSignatureCount++;
            output->cullSignatures[index][0] = width;
            output->cullSignatures[index][1] = height;
        }
        if (end == raw.size()) break;
        begin = end + 1;
    }
}

float clampPercent(float value) {
    if (!std::isfinite(value) || value < 0.0f) return 0.0f;
    return value > 50.0f ? 50.0f : value;
}

float clampFraction(float value) {
    if (!std::isfinite(value)) return 1.0f;
    if (value < 0.0f) return 0.0f;
    return value > 1.0f ? 1.0f : value;
}

// ---------------------------------------------------------------------------
// The field-of-view trim, per headset.
//
// fix.fov_trim_vertical / _outer / _nasal are per-headset lists keyed exactly
// like fix.openxr_resolution -- `runtime[/system]:degrees`, degrees 0..30, at
// most eight entries -- resolved against the worn headset's tokens as the last
// v2 render-settings query saw them (nativeRenderLabels). A trim tuned on one
// headset is wrong on another, so a headset with no entry of its own gets no
// trim rather than somebody else's.
//
// beginFrame runs on every frame, and three lists a frame is waste: the lists
// are parsed only when a key's text or the worn headset changes, the resolved
// triple is cached, and the log line goes out once per change of it.
constexpr size_t kTrimCount = 3;
const char* const kTrimKeys[kTrimCount] = {
    "fix.fov_trim_vertical", "fix.fov_trim_outer", "fix.fov_trim_nasal"};
const char* const kTrimNames[kTrimCount] = {"vertical", "outer", "nasal"};
// Enough distinct malformed tokens to name a hand-edited file's worth and
// stop; past that the log would repeat itself on every edit.
constexpr size_t kTrimNamedMax = 32;

struct TrimState {
    bool        resolved = false;            // the cache below has been filled
    std::string values[kTrimCount];          // the ini text it was filled from
    std::string rt, sys;                     // the worn tokens it was filled from
    bool        headset = false;
    uint32_t    degrees[kTrimCount] = {0, 0, 0};
    // What the log has already said, so a note goes out once per change of the
    // resolved triple rather than once per frame.
    bool        headsetNoted = false;
    bool        noHeadsetNoted = false;
    std::string notedKey;
    uint32_t    notedDegrees[kTrimCount] = {0, 0, 0};
    // "<key>|<token>" for every malformed token already named.
    std::vector<std::string> named;
};
// Guarded by g_mutex: every reader is inside beginFrame, which holds it.
TrimState g_trim;

void resolveTrim(uint32_t out[kTrimCount]) {
    using namespace edvr::native_render;
    NativeRenderLabels labels{};
    const bool haveHeadset = nativeRenderLabels(&labels);
    const std::string rt = haveHeadset ? std::string(labels.runtimeToken) : std::string();
    const std::string sys = haveHeadset ? std::string(labels.systemToken) : std::string();
    // Read literally, one call per key: the config contract and the settings
    // schema both find a key by the string in the call that reads it.
    const std::string values[kTrimCount] = {
        edvr::Config::get().getString("fix.fov_trim_vertical", ""),
        edvr::Config::get().getString("fix.fov_trim_outer", ""),
        edvr::Config::get().getString("fix.fov_trim_nasal", "")};
    if (g_trim.resolved && g_trim.headset == haveHeadset && g_trim.rt == rt &&
        g_trim.sys == sys && g_trim.values[0] == values[0] &&
        g_trim.values[1] == values[1] && g_trim.values[2] == values[2]) {
        for (size_t i = 0; i < kTrimCount; ++i) out[i] = g_trim.degrees[i];
        return;
    }

    HeadsetEntry entries[kTrimCount][kHeadsetEntryMax];
    size_t counts[kTrimCount] = {0, 0, 0};
    uint32_t degrees[kTrimCount] = {0, 0, 0};
    bool anyEntry = false;
    for (size_t i = 0; i < kTrimCount; ++i) {
        std::vector<std::string> skipped;
        counts[i] = parseHeadsetEntries(values[i].c_str(), entries[i], &skipped,
                                        0, kTrimDegreesMax);
        if (counts[i]) anyEntry = true;
        degrees[i] = haveHeadset
            ? resolveHeadsetValue(entries[i], counts[i], rt, sys, nullptr) : 0u;
        for (const std::string& token : skipped) {
            const std::string seen = std::string(kTrimKeys[i]) + "|" + token;
            if (g_trim.named.size() >= kTrimNamedMax ||
                std::find(g_trim.named.begin(), g_trim.named.end(), seen) !=
                    g_trim.named.end()) {
                continue;
            }
            g_trim.named.push_back(seen);
            // The old shipped form was a bare `5`, which names no headset:
            // say so once per distinct token per key, with the shape of an
            // entry, rather than silently dropping it.
            edvr::Log::get().note("fov trim: ignored \"%s\" in %s (entries look like "
                                  "pimax-openxr/pimax-crystal-super:5)",
                                  token.c_str(), kTrimKeys[i]);
        }
    }

    const std::string key = haveHeadset ? headsetKey(rt, sys) : std::string();
    if (haveHeadset) {
        const bool changed = !g_trim.headsetNoted || g_trim.notedKey != key ||
                             g_trim.notedDegrees[0] != degrees[0] ||
                             g_trim.notedDegrees[1] != degrees[1] ||
                             g_trim.notedDegrees[2] != degrees[2];
        if (changed) {
            std::string saved;
            for (size_t i = 0; i < kTrimCount; ++i) {
                if (i) saved += "; ";
                saved += kTrimNames[i];
                saved += " ";
                saved += counts[i] ? formatHeadsetEntries(entries[i], counts[i])
                                   : std::string("none");
            }
            edvr::Log::get().note("fov trim: this headset is %s; vertical %u, outer %u, "
                                  "nasal %u degrees (entries: %s).", key.c_str(),
                                  degrees[0], degrees[1], degrees[2], saved.c_str());
            g_trim.headsetNoted = true;
            g_trim.noHeadsetNoted = false;
            g_trim.notedKey = key;
            for (size_t i = 0; i < kTrimCount; ++i) g_trim.notedDegrees[i] = degrees[i];
        }
    } else if (anyEntry && !g_trim.noHeadsetNoted) {
        // Entries are saved but nothing has published a headset: on SteamVR or
        // OpenComposite nothing ever will, and the keys are inert.
        edvr::Log::get().note("fov trim: no headset is known yet, so no trim applies.");
        g_trim.noHeadsetNoted = true;
        g_trim.headsetNoted = false;
    }

    g_trim.resolved = true;
    g_trim.headset = haveHeadset;
    g_trim.rt = rt;
    g_trim.sys = sys;
    for (size_t i = 0; i < kTrimCount; ++i) {
        g_trim.values[i] = values[i];
        g_trim.degrees[i] = degrees[i];
        out[i] = degrees[i];
    }
}

HRESULT WINAPI beginFrame(void* context, const EdvrNativeFrameInput* input,
                          EdvrNativeFrameOutput* output) {
    std::lock_guard<std::mutex> lock(g_mutex);
    State* state = identify(context);
    // A version 1 caller is an openvr_api.dll from before the field-of-view
    // trim existed. It gets exactly the fields it knows about, and the trim
    // it cannot carry stays out of its struct entirely.
    const bool wantsTrim = output &&
        output->version == EDVR_NATIVE_FRAME_VERSION_2 &&
        output->size == sizeof(*output);
    const bool legacy = output && !wantsTrim &&
        output->version == EDVR_NATIVE_FRAME_VERSION_1 &&
        output->size == EDVR_NATIVE_FRAME_OUTPUT_SIZE_1;
    if (!state || state != g_current || !state->active || !input || !output ||
        input->size != sizeof(*input) || input->version != EDVR_NATIVE_FRAME_VERSION_1 ||
        (!wantsTrim && !legacy) ||
        input->generation != state->generation || input->referenceGeneration == 0 ||
        input->sequence == 0 || input->sequence <= state->sequenceFloor ||
        input->valid > 1 || (input->valid && !rigidPose(input->physicalHead))) return E_INVALIDARG;

    EdvrNativeFrameOutput result{};
    result.size = wantsTrim ? sizeof(result) : EDVR_NATIVE_FRAME_OUTPUT_SIZE_1;
    result.version = wantsTrim ? EDVR_NATIVE_FRAME_VERSION_2
                               : EDVR_NATIVE_FRAME_VERSION_1;
    result.headOffset[0] = boundedOffset(edvr::Config::get().getFloat(
        "openvr.head_offset_right", 0.0f));
    result.headOffset[1] = boundedOffset(edvr::Config::get().getFloat(
        "openvr.head_offset_up", 0.0f));
    result.headOffset[2] = -boundedOffset(edvr::Config::get().getFloat(
        "openvr.head_offset_forward", 0.0f));
    result.yawRadians = wrappedYawRadians(edvr::Config::get().getFloat(
        "openvr.head_yaw_degrees", 0.0f));
    result.offsetGamePoses = edvr::Config::get().getBool(
        "openvr.head_offset_game_poses", true) ? 1u : 0u;

    const bool externalOnly = edvr::Config::get().getBool(
        "openvr.head_offset_external_only", true);
    const uint32_t maxStale = static_cast<uint32_t>(edvr::Config::get().getIntInRange(
        "openvr.head_offset_max_stale_frames", 90, 2, 900));
    const bool modeGate = edvr::externalCameraOnFootLive(maxStale);
    const bool physicalValid = input->valid != 0;
    const bool anyOffset = result.headOffset[0] != 0.0f ||
                           result.headOffset[1] != 0.0f ||
                           result.headOffset[2] != 0.0f ||
                           result.yawRadians != 0.0f;
    result.offsetEnabled =
        (anyOffset && physicalValid && (!externalOnly || modeGate)) ? 1u : 0u;

    result.cullMode = cullMode(edvr::Config::get().getString(
        "fix.cull_guard", "off"));
    result.cullPercent = clampPercent(edvr::Config::get().getFloat(
        "fix.cull_guard_percent", 8.0f));
    result.cullHorizontalFraction = clampFraction(edvr::Config::get().getFloat(
        "fix.cull_guard_fraction_h", 1.0f));
    result.cullVerticalFraction = clampFraction(edvr::Config::get().getFloat(
        "fix.cull_guard_fraction_v", 1.0f));
    readSignatures(edvr::Config::get().getString("fix.cull_guard_headsets", ""), &result);
    // The worn headset's entry in each of the three lists, resolved from the
    // last render-settings query's labels and cached between changes.
    uint32_t trim[kTrimCount] = {0, 0, 0};
    resolveTrim(trim);
    result.trimVerticalDeg = static_cast<float>(trim[0]);
    result.trimOuterDeg = static_cast<float>(trim[1]);
    result.trimNasalDeg = static_cast<float>(trim[2]);
    result.sceneReady = edvr::sceneArrived() &&
                        sameDevice(state->device, edvr::gameDevice()) ? 1u : 0u;
    result.transitionEnabled = edvr::Config::get().getBool(
        "fix.transition_flash", true) ? 1u : 0u;
    result.resubmitEnabled = edvr::Config::get().getBool(
        "advanced.transition_flash_resubmit", true) ? 1u : 0u;

    if (physicalValid) {
        edvr::publishHeadPose(input->physicalHead);
        const float denominator = input->physicalHead[10] < 0.05f
                                      ? 0.05f : input->physicalHead[10];
        edvr::announceHeadForward(-input->physicalHead[8] / denominator,
                                  -input->physicalHead[9] / denominator);
    }
    edvr::clearGlitchFrame();
    // A frame may be prepared by the owner and then dropped before either eye
    // is submitted. Consume an explicit hold only when the first valid latch
    // makes the pair visible to the compositor.
    state->held = false;
    state->transitionEnabled = result.transitionEnabled != 0;
    state->sequenceFloor = input->sequence;
    state->invalidated = false;
    state->began = true;
    state->latched = false;
    state->cachedDecision = {};
    ++state->beginCount;
    if (!state->configNoted || state->lastOffsetEnabled != result.offsetEnabled ||
        state->lastTransitionEnabled != result.transitionEnabled ||
        state->lastResubmitEnabled != result.resubmitEnabled ||
        state->lastCullMode != result.cullMode) {
        edvr::Log::get().note(
            "native frame: begin #%u seq=%llu offsets=%s (%+.3f,%+.3f,%+.3f), "
            "cull=%u, transition=%s, resubmit=%s.", state->beginCount,
            static_cast<unsigned long long>(input->sequence),
            result.offsetEnabled ? "on" : "off", result.headOffset[0],
            result.headOffset[1], result.headOffset[2], result.cullMode,
            result.transitionEnabled ? "on" : "off",
            result.resubmitEnabled ? "on" : "off");
        state->configNoted = true;
        state->lastOffsetEnabled = result.offsetEnabled;
        state->lastTransitionEnabled = result.transitionEnabled;
        state->lastResubmitEnabled = result.resubmitEnabled;
        state->lastCullMode = result.cullMode;
    }
    // Only as many bytes as the caller's own struct holds.
    std::memcpy(output, &result, result.size);
    return S_OK;
}

HRESULT WINAPI setCullState(void* context, uint32_t stage, float factorH,
                            float factorV) {
    std::lock_guard<std::mutex> lock(g_mutex);
    State* state = identify(context);
    if (!state || state != g_current || !state->active || stage > 2 ||
        !std::isfinite(factorH) || !std::isfinite(factorV) ||
        (stage != 0 && (factorH < 1.0f || factorV < 1.0f))) return E_INVALIDARG;
    edvr::announceCullGuardState(stage, factorH, factorV);
    return S_OK;
}

HRESULT WINAPI latchSubmit(void* context, uint64_t sequence,
                           EdvrNativeFrameDecision* decision) {
    std::lock_guard<std::mutex> lock(g_mutex);
    State* state = identify(context);
    if (!state || state != g_current || !state->active || !state->began ||
        !decision || decision->size != sizeof(*decision) ||
        decision->version != EDVR_NATIVE_FRAME_VERSION_1 || sequence == 0 ||
        sequence != state->sequenceFloor || state->invalidated) return E_INVALIDARG;

    if (!state->latched) {
        const bool marked = edvr::glitchFrameMarked();
        state->held = edvr::takeSubmitHoldFrame();
        state->cachedDecision = {};
        state->cachedDecision.size = sizeof(state->cachedDecision);
        state->cachedDecision.version = EDVR_NATIVE_FRAME_VERSION_1;
        state->cachedDecision.withhold =
            ((state->transitionEnabled && marked) || state->held) ? 1u : 0u;
        state->cachedDecision.jumpOnly =
            (state->cachedDecision.withhold && marked && !state->held) ? 1u : 0u;
        state->cachedDecision.verdict = edvr::jumpVerdictPacked();
        edvr::announceGlitchConsumer();
        state->consumerAnnounced = true;
        state->latched = true;
    }
    ++state->latchCount;
    *decision = state->cachedDecision;
    return S_OK;
}

HRESULT WINAPI invalidate(void* context) {
    std::lock_guard<std::mutex> lock(g_mutex);
    State* state = identify(context);
    if (!state || state != g_current || !state->active) return E_INVALIDARG;
    state->invalidated = true;
    state->began = false;
    state->latched = false;
    state->cachedDecision = {};
    state->held = false;
    ++state->invalidationCount;
    edvr::clearGlitchFrame();
    edvr::announceCullGuardState(0, 1.0f, 1.0f);
    return S_OK;
}

HRESULT WINAPI close(void* context) {
    std::lock_guard<std::mutex> lock(g_mutex);
    State* state = identify(context);
    if (!state) return E_INVALIDARG;
    if (!state->active) return S_FALSE;
    edvr::clearGlitchFrame();
    edvr::announceCullGuardState(0, 1.0f, 1.0f);
    if (state->consumerAnnounced) {
        edvr::retireGlitchConsumer();
        state->consumerAnnounced = false;
    }
    state->active = false;
    state->device = nullptr;
    state->invalidated = true;
    state->began = false;
    state->latched = false;
    if (g_current == state) g_current = nullptr;
    edvr::Log::get().note(
        "native frame: close begins=%u latches=%u invalidations=%u.",
        state->beginCount, state->latchCount, state->invalidationCount);
    return S_OK;
}

} // namespace

extern "C" HRESULT WINAPI edvrAcquireNativeFrame(
    const EdvrNativeFrameRequest* request, EdvrNativeFrameTable* table) {
    if (!table || table->size != sizeof(*table) ||
        table->version != EDVR_NATIVE_FRAME_VERSION_1) return E_INVALIDARG;
    *table = {};
    table->size = sizeof(*table);
    table->version = EDVR_NATIVE_FRAME_VERSION_1;
    if (!request || request->size != sizeof(*request) ||
        request->version != EDVR_NATIVE_FRAME_VERSION_1 ||
        !request->gameDevice || request->generation == 0) return E_INVALIDARG;

    IUnknown* identity = nullptr;
    if (FAILED(request->gameDevice->QueryInterface(
            IID_IUnknown, reinterpret_cast<void**>(&identity)))) return E_INVALIDARG;
    identity->Release();
    if (FAILED(request->gameDevice->GetDeviceRemovedReason())) return E_INVALIDARG;

    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_current || g_used == kPoolSize) return E_PENDING;
    State& state = g_pool[g_used++];
    state = {};
    state.device = request->gameDevice;
    state.generation = request->generation;
    state.active = true;
    g_current = &state;
    table->context = &state;
    table->beginFrame = beginFrame;
    table->setCullState = setCullState;
    table->latchSubmit = latchSubmit;
    table->invalidate = invalidate;
    table->close = close;
    return S_OK;
}
