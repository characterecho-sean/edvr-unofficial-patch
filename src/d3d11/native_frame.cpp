#include "../common/native_frame.h"

#include "../common/comfort_fade.h"
#include "../common/config.h"
#include "../common/frame_flag.h"
#include "../common/log.h"
#include "../common/openxr_resolution_entries.h"
#include "journal_watch.h"
#include "native_on_foot.h"
#include "native_render_labels.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace {
constexpr unsigned kPoolSize = 16;

// Frame pairs latched by latchSubmit; read by nativeFrameLatchedFrames().
std::atomic<uint64_t> g_latchedFrames{0};

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
    bool configNoted = false;
    uint32_t beginCount = 0;
    uint32_t latchCount = 0;
    uint32_t invalidationCount = 0;
    uint32_t lastResubmitEnabled = 0;
    uint32_t lastDeferredPacing = 0;
    bool pacingNoted = false;
    bool weaponStabilityNoted = false;
    bool lastWeaponStability = true;
    bool lastStepAside = false;   // F36: weapon stability stepped aside (native on foot active), as last logged
    EdvrNativeFrameDecision cachedDecision{};
};

State g_pool[kPoolSize];
unsigned g_used = 0;
State* g_current = nullptr;
std::mutex g_mutex;
// F22: the look-to-aim total and the game frame it was taken at, for version 6 (set by nativeFrameSetLookYaw; guarded by g_mutex).
float g_lookYaw = 0.0f;
uint32_t g_lookYawFrame = 0;
uint32_t g_lookReset = 0;   // F25: the reset serial
// F33: the body-align translation (seated-space metres) and its serial, for version 8 (set by nativeFrameSetEyeAlign; guarded by g_mutex).
float g_eyeAlign[3] = {};
uint32_t g_eyeAlignSerial = 0;
std::atomic<bool> g_nowPoseRender{false};   // F40: fix.now_pose (render true, next false), carried in version 9 output
// F31: the latest beginFrame's render head and the runtime's "now" head (version 2 input), for the world markers (guarded by g_mutex).
float g_renderHead[12] = {};
float g_nowHeadPose[12] = {};
bool g_renderHeadValid = false, g_nowHeadValid = false, g_haveFrameInput = false;
uint32_t g_nowHeadSequence = 0;
uint64_t g_frameSequence = 0;
uint32_t g_inputVersion = 0;
// F37: the render head ring and the reference generation, from the last accepted input (guarded by g_mutex).
float g_ringHead[8][12] = {};
uint64_t g_ringSequence[8] = {};
uint64_t g_ringQpc[8] = {};
uint32_t g_ringNewest = 0, g_ringCount = 0, g_referenceGeneration = 0;

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

// ---------------------------------------------------------------------------
// The field-of-view trim, per headset.
//
// experimental.fov_trim_vertical / _outer / _nasal are per-headset lists keyed
// exactly like fix.openxr_resolution -- `runtime[/system]:degrees`, degrees
// 0..30, at most eight entries -- resolved against the worn headset's tokens as
// the last v2 render-settings query saw them (nativeRenderLabels). A trim tuned
// on one headset is wrong on another, so a headset with no entry of its own gets
// no trim rather than somebody else's.
//
// They lived under [fix] until 2026-09-29, with rows on the F8 menu; edvr.ini
// says where they went (`# moved-from: fix.fov_trim_*`) and Config reads an
// old-layout line as the new name until the installer migrates the file. The
// keys are set in the file only now.
//
// beginFrame runs on every frame, and three lists a frame is waste: the lists
// are parsed only when a key's text or the worn headset changes, the resolved
// triple is cached, and the log line goes out once per change of it.
constexpr size_t kTrimCount = 3;
const char* const kTrimKeys[kTrimCount] = {
    "experimental.fov_trim_vertical", "experimental.fov_trim_outer",
    "experimental.fov_trim_nasal"};
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
        edvr::Config::get().getString("experimental.fov_trim_vertical", ""),
        edvr::Config::get().getString("experimental.fov_trim_outer", ""),
        edvr::Config::get().getString("experimental.fov_trim_nasal", "")};
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
    // A version 1, 2, 3 or 4 caller is an openvr_api.dll from before the
    // comfort fade (or, earlier, the channel probe, turbo pacing or the
    // field-of-view trim) existed. Each gets exactly the fields it knows
    // about, and whatever it cannot carry stays out of its struct entirely.
    // F22: version 6 is the struct through the look's three words (after the fade); version 5 is the struct through the fade, which a version 5 caller has.
    // F33: version 8 is the whole struct (the eye alignment after the look); version 6 is now the struct through lookYawReset.
    const bool wantsEyeAlign = output &&
        (output->version == EDVR_NATIVE_FRAME_VERSION_8 || output->version == EDVR_NATIVE_FRAME_VERSION_9) &&
        output->size == sizeof(*output);
    const bool wantsLook = output && !wantsEyeAlign &&
        output->version == EDVR_NATIVE_FRAME_VERSION_6 &&
        output->size == EDVR_NATIVE_FRAME_OUTPUT_SIZE_6;
    const bool wantsFade = output && !wantsEyeAlign && !wantsLook &&
        output->version == EDVR_NATIVE_FRAME_VERSION_5 &&
        output->size == EDVR_NATIVE_FRAME_OUTPUT_SIZE_5;
    const bool wantsChannel = output && !wantsEyeAlign && !wantsFade && !wantsLook &&
        output->version == EDVR_NATIVE_FRAME_VERSION_4 &&
        output->size == EDVR_NATIVE_FRAME_OUTPUT_SIZE_4;
    const bool wantsPacing = output && !wantsEyeAlign && !wantsFade && !wantsChannel && !wantsLook &&
        output->version == EDVR_NATIVE_FRAME_VERSION_3 &&
        output->size == EDVR_NATIVE_FRAME_OUTPUT_SIZE_3;
    const bool wantsTrim = output && !wantsEyeAlign && !wantsFade && !wantsChannel && !wantsPacing && !wantsLook &&
        output->version == EDVR_NATIVE_FRAME_VERSION_2 &&
        output->size == EDVR_NATIVE_FRAME_OUTPUT_SIZE_2;
    const bool legacy = output && !wantsEyeAlign && !wantsFade && !wantsChannel && !wantsPacing && !wantsTrim && !wantsLook &&
        output->version == EDVR_NATIVE_FRAME_VERSION_1 &&
        output->size == EDVR_NATIVE_FRAME_OUTPUT_SIZE_1;
    // F31: input version 1 is the words through valid (its size is that prefix; the full size is accepted too, as the rigs send it). Version 2 is
    // the full struct with the "now" head. Nothing past the version 1 words is read for a version 1 input.
    const bool inputV1 = input && input->version == EDVR_NATIVE_FRAME_INPUT_VERSION_1 &&
        (input->size == EDVR_NATIVE_FRAME_INPUT_SIZE_1 || input->size == sizeof(*input));
    const bool inputV2 = input && input->version == EDVR_NATIVE_FRAME_INPUT_VERSION_2 && input->size == sizeof(*input);
    if (input && input->version == EDVR_NATIVE_FRAME_INPUT_VERSION_2 && !inputV2) {   // F32: the one way a version 2 input is refused
        static bool noted = false;
        if (!noted) {
            noted = true;
            edvr::Log::get().note("now head: frame input version 2 REFUSED: size %u, this build's version 2 size is %u (the runtime steps down to version 1)",
                                  static_cast<unsigned>(input->size), static_cast<unsigned>(sizeof(*input)));
        }
    }
    if (!state || state != g_current || !state->active || !input || !output ||
        (!inputV1 && !inputV2) ||
        (!wantsEyeAlign && !wantsLook && !wantsFade && !wantsChannel && !wantsPacing && !wantsTrim && !legacy) ||
        input->generation != state->generation || input->referenceGeneration == 0 ||
        input->sequence == 0 || input->sequence <= state->sequenceFloor ||
        input->valid > 1 || (input->valid && !rigidPose(input->physicalHead))) return E_INVALIDARG;

    EdvrNativeFrameOutput result{};
    result.size = wantsEyeAlign ? sizeof(result)
                 : wantsLook ? EDVR_NATIVE_FRAME_OUTPUT_SIZE_6
                 : wantsFade ? EDVR_NATIVE_FRAME_OUTPUT_SIZE_5
                 : wantsChannel ? EDVR_NATIVE_FRAME_OUTPUT_SIZE_4
                 : wantsPacing ? EDVR_NATIVE_FRAME_OUTPUT_SIZE_3
                 : wantsTrim  ? EDVR_NATIVE_FRAME_OUTPUT_SIZE_2
                              : EDVR_NATIVE_FRAME_OUTPUT_SIZE_1;
    result.version = wantsEyeAlign ? (output->version == EDVR_NATIVE_FRAME_VERSION_9 ? EDVR_NATIVE_FRAME_VERSION_9 : EDVR_NATIVE_FRAME_VERSION_8)
                    : wantsLook ? EDVR_NATIVE_FRAME_VERSION_6
                    : wantsFade ? EDVR_NATIVE_FRAME_VERSION_5
                    : wantsChannel ? EDVR_NATIVE_FRAME_VERSION_4
                    : wantsPacing ? EDVR_NATIVE_FRAME_VERSION_3
                    : wantsTrim  ? EDVR_NATIVE_FRAME_VERSION_2
                                 : EDVR_NATIVE_FRAME_VERSION_1;
    // Explorer Cam's comfort fade (comfort_fade.h): how black the view is
    // this frame, 0 unless the frame thread published a fresh level. Only a
    // version 5 caller has the slot; every older shape never learns of it.
    result.fadeAlpha = edvr::comfort::read(GetTickCount64());
    // F22: the look-to-aim total (version 6 only; every other shape never learns of it). The frame thread sets it once a frame.
    if (wantsLook || wantsEyeAlign) {
        result.lookYawTotal = g_lookYaw;
        result.lookYawGameFrame = g_lookYawFrame;
        result.lookYawReset = g_lookReset;
    }
    // F33: the body-align translation (version 8 only; every older shape never learns of it).
    if (wantsEyeAlign) {
        for (int i = 0; i < 3; ++i) result.eyeAlign[i] = g_eyeAlign[i];
        result.eyeAlignSerial = g_eyeAlignSerial;
    }
    // F40: the now-pose choice (version 9 only; every older shape never learns of it).
    if (wantsEyeAlign && output->version == EDVR_NATIVE_FRAME_VERSION_9)
        result.nowPoseRender = g_nowPoseRender.load(std::memory_order_relaxed) ? 1u : 0u;
    // The old Explorer Cam's headset offset (headOffset, yawRadians,
    // offsetEnabled, offsetGamePoses) is retired: those slots stay zero, so a
    // runtime built before 2026-10-07 reads "no offset" and applies none.
    const bool physicalValid = input->valid != 0;
    // F31: the frame's heads, kept for the world markers (nativeFrameNowPose). The "now" head only from a version 2 input.
    g_haveFrameInput = true;
    g_frameSequence = input->sequence;
    g_renderHeadValid = physicalValid;
    if (physicalValid) std::memcpy(g_renderHead, input->physicalHead, sizeof(g_renderHead));
    // F32: an optional head that is not usable is dropped for this frame, never a refusal of the frame (a refusal steps the runtime's ladder
    // down for the session). It is logged once.
    const bool nowUsable = inputV2 && input->nowValid == 1 && rigidPose(input->nowHead);
    if (inputV2 && input->nowValid != 0 && !nowUsable) {
        static bool noted = false;
        if (!noted) {
            noted = true;
            edvr::Log::get().note("now head: dropped for this frame (nowValid %u, not a rigid transform): the frame goes on without it", static_cast<unsigned>(input->nowValid));
        }
    }
    g_nowHeadValid = nowUsable;
    g_inputVersion = input->version;
    g_referenceGeneration = input->referenceGeneration;
    g_ringCount = 0;
    if (inputV2 && input->ringNewest < 8 && input->ringCount <= 8) {
        std::memcpy(g_ringHead, input->ringHead, sizeof(g_ringHead));
        std::memcpy(g_ringSequence, input->ringSequence, sizeof(g_ringSequence));
        std::memcpy(g_ringQpc, input->ringQpc, sizeof(g_ringQpc));
        g_ringNewest = input->ringNewest;
        g_ringCount = input->ringCount;
    }
    if (inputV2) {
        static bool accepted = false;
        if (!accepted) {
            accepted = true;
            edvr::Log::get().note("now head: frame input version 2 accepted (nowValid %u, answer #%u, geometry #%llu)", static_cast<unsigned>(input->nowValid),
                                  static_cast<unsigned>(input->nowSequence), static_cast<unsigned long long>(input->sequence));
        }
    }
    if (g_nowHeadValid) {
        std::memcpy(g_nowHeadPose, input->nowHead, sizeof(g_nowHeadPose));
        g_nowHeadSequence = input->nowSequence;
    }

    // The worn headset's entry in each of the three lists, resolved from the
    // last render-settings query's labels and cached between changes.
    uint32_t trim[kTrimCount] = {0, 0, 0};
    resolveTrim(trim);
    result.trimVerticalDeg = static_cast<float>(trim[0]);
    result.trimOuterDeg = static_cast<float>(trim[1]);
    result.trimNasalDeg = static_cast<float>(trim[2]);
    result.sceneReady = edvr::sceneArrived() &&
                        sameDevice(state->device, edvr::gameDevice()) ? 1u : 0u;
    // The setting that switched the hold off (retired 2026-10-13) and the one that
    // switched the compositor's re-submit of a withheld frame (retired 2026-10-09)
    // are gone; both fields stay in the ABI (native_frame.h) and are always on.
    result.transitionEnabled = 1u;
    result.resubmitEnabled = 1u;

    // fix.weapon_stability: while the journal watcher says Status.json
    // reports on foot, the frame wait moves from WaitGetPoses to the second
    // eye's Submit (deferred pacing, OpenXR Toolkit's "Turbo mode"), which
    // is what holds the weapon steady on mouse turns. Off, in a ship, or
    // with the watcher off, the wait stays in WaitGetPoses.
    const bool weaponStability = edvr::Config::get().getBool(
        "fix.weapon_stability", true);
    // F36: while native HMD stereo on foot is active, weapon stability steps aside (its pacing and its motion-vector gate); the key is untouched.
    const bool stepAside = edvr::nativeOnFootActive();
    if (weaponStability && stepAside != state->lastStepAside) {
        state->lastStepAside = stepAside;
        edvr::Log::get().note("weapon stability: %s", stepAside ? "stepped aside (native on foot active)" : "resumed");
    }
    result.deferredPacing =
        (weaponStability && !stepAside && edvr::journalOnFootKnown() && edvr::journalOnFoot())
            ? 1u : 0u;
    if (result.deferredPacing) edvr::nativeOnFootCountGate(edvr::NativeOnFootGate::DeferredPacing);   // F0 gate counter (log only)
    if (!state->weaponStabilityNoted ||
        state->lastWeaponStability != weaponStability) {
        edvr::Log::get().note(
            "weapon stability: %s (live; the frame wait moves to the eye "
            "submit while on foot).", weaponStability ? "on" : "off");
        state->weaponStabilityNoted = true;
        state->lastWeaponStability = weaponStability;
    }
    if (weaponStability && !state->pacingNoted && !edvr::journalWatchActive()) {
        edvr::Log::get().note(
            "weapon stability: needs the journal watcher (d3d11.journal_watch) "
            "to say when you are on foot, and it is off, so it never engages.");
        state->pacingNoted = true;
    }

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
    state->sequenceFloor = input->sequence;
    state->invalidated = false;
    state->began = true;
    state->latched = false;
    state->cachedDecision = {};
    ++state->beginCount;
    if (!state->configNoted ||
        state->lastResubmitEnabled != result.resubmitEnabled ||
        state->lastDeferredPacing != result.deferredPacing) {
        edvr::Log::get().note(
            "native frame: begin #%u seq=%llu resubmit=%s, "
            "pacing=%s.", state->beginCount,
            static_cast<unsigned long long>(input->sequence),
            result.resubmitEnabled ? "on" : "off",
            result.deferredPacing ? "turbo" : "runtime");
        state->configNoted = true;
        state->lastResubmitEnabled = result.resubmitEnabled;
        state->lastDeferredPacing = result.deferredPacing;
    }
    // Only as many bytes as the caller's own struct holds.
    std::memcpy(output, &result, result.size);
    return S_OK;
}

// RETIRED with the terrain guard (native_frame.h, EdvrNativeFrameRetiredCall): the slot stays in the
// table, so a runtime built before the removal still finds the provider's own entry there, and the
// entry does nothing.
HRESULT WINAPI retiredCall(void*, uint32_t, float, float) {
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
    // Counted before any fix is read: beginFrame sets `began` whatever the fixes, so this counts the frame pairs the game submits
    // (the mode watch's file-independent signal, mode_watch.cpp). Once per frame pair: the runtime latches only its first eye.
    g_latchedFrames.fetch_add(1, std::memory_order_relaxed);

    if (!state->latched) {
        const bool marked = edvr::glitchFrameMarked();
        state->held = edvr::takeSubmitHoldFrame();
        state->cachedDecision = {};
        state->cachedDecision.size = sizeof(state->cachedDecision);
        state->cachedDecision.version = EDVR_NATIVE_FRAME_VERSION_1;
        state->cachedDecision.withhold =
            (marked || state->held) ? 1u : 0u;
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
    return S_OK;
}

HRESULT WINAPI close(void* context) {
    std::lock_guard<std::mutex> lock(g_mutex);
    State* state = identify(context);
    if (!state) return E_INVALIDARG;
    if (!state->active) return S_FALSE;
    edvr::clearGlitchFrame();
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

namespace edvr {

// For temporal_pass.cpp's edges-mode fovea (same DLL, no table needed):
// the cached triple beginFrame already resolved above, vertical/outer/
// nasal degrees in kTrimNames' own order. Zero in every slot until a
// beginFrame has resolved a headset.
void nativeFrameFovTrimDegrees(uint32_t out[3]) {
    std::lock_guard<std::mutex> lock(g_mutex);
    out[0] = out[1] = out[2] = 0;
    if (g_trim.resolved) {
        for (size_t i = 0; i < kTrimCount; ++i) out[i] = g_trim.degrees[i];
    }
}

uint64_t nativeFrameLatchedFrames() { return g_latchedFrames.load(std::memory_order_relaxed); }

// F22: the look-to-aim total the next version 6 beginFrame hands the runtime.
void nativeFrameSetLookYaw(float total, uint32_t gameFrame, uint32_t resetSerial) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_lookYaw = total;
    g_lookYawFrame = gameFrame;
    g_lookReset = resetSerial;
}

// F33: the body-align translation the next version 8 beginFrame hands the runtime.
// F40: fix.now_pose from the config (readKey). Logged at the first value and at each change.
void nativeFrameSetNowPoseRender(bool render) {
    static bool logged = false;
    const bool was = g_nowPoseRender.exchange(render, std::memory_order_relaxed);
    if (!logged || was != render) {
        logged = true;
        edvr::Log::get().note("now pose: %s (the runtime answers Elite's now request %s; live, fix.now_pose)", render ? "render" : "next",
                              render ? "at the instant the frame draws at, the WaitGetPoses latch" : "one display period ahead, as today");
    }
}
void nativeFrameSetEyeAlign(const float o[3], uint32_t serial) {
    std::lock_guard<std::mutex> lock(g_mutex);
    for (int i = 0; i < 3; ++i) g_eyeAlign[i] = o[i];
    g_eyeAlignSerial = serial;
}

// F31: the latest frame's render head and "now" head (the d3d11 half's world markers).
bool nativeFrameNowPose(EdvrNativeNowPose& out) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_haveFrameInput) return false;
    std::memcpy(out.render, g_renderHead, sizeof(out.render));
    std::memcpy(out.now, g_nowHeadPose, sizeof(out.now));
    out.renderValid = g_renderHeadValid;
    out.nowValid = g_nowHeadValid;
    out.nowSequence = g_nowHeadSequence;
    out.frameSequence = g_frameSequence;
    out.inputVersion = g_inputVersion;
    out.referenceGeneration = g_referenceGeneration;
    std::memcpy(out.ringHead, g_ringHead, sizeof(out.ringHead));
    std::memcpy(out.ringSequence, g_ringSequence, sizeof(out.ringSequence));
    std::memcpy(out.ringQpc, g_ringQpc, sizeof(out.ringQpc));
    out.ringNewest = g_ringNewest;
    out.ringCount = g_ringCount;
    return true;
}

}  // namespace edvr

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
    table->reservedCall = retiredCall;
    table->latchSubmit = latchSubmit;
    table->invalidate = invalidate;
    table->close = close;
    return S_OK;
}
