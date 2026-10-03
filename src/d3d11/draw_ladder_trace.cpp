#include "draw_ladder_trace.h"

#ifndef EDVR_VERSION_STRING
#define EDVR_VERSION_STRING "unversioned test build"
#endif

#include <windows.h>
#include <strsafe.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <new>

namespace edvr::draw_ladder_trace {
namespace {

constexpr std::uint32_t kIdentitySlots = kMaxDraws * 6;
constexpr std::size_t kPathChars = 1024;
constexpr std::size_t kNameChars = 260;

struct SiteEvent final {
    std::uint16_t id = 0;
    std::uint16_t subsite = 0;
    std::uint8_t kind = 0;
    std::uint8_t outcome = 0;
    std::uint8_t flow = 0;
    std::uint8_t reserved = 0;
    std::int16_t verdict = -1;
};

struct DrawRecord final {
    DrawFacts facts{};
    SiteEvent sites[kMaxSiteEventsPerDraw]{};
    draw_ladder::ActionRecord actions[kMaxActionEventsPerDraw]{};
    std::uint16_t actionIds[kMaxActionEventsPerDraw]{};
    PredicateFact predicateFacts[kMaxPredicateFactsPerDraw]{};
    ForwardFacts forwardFacts{};
    std::uint16_t siteCount = 0;
    std::uint16_t actionCount = 0;
    std::uint8_t predicateFactCount = 0;
    std::int16_t winnerSiteId = -1;
    std::int16_t verdictOrdinal = -1;
    bool finalized = false;
    bool hasForwardFacts = false;
};

DrawRecord* g_records = nullptr;
std::uintptr_t* g_identities = nullptr;
std::uint32_t g_drawCount = 0;
std::uint32_t g_identityCount = 0;
std::uint32_t g_generation = 1;
FrameFacts g_frame{};
std::atomic<bool> g_enabled{false};
std::atomic<Status> g_status{Status::Disabled};
bool g_isCapturing = false;
bool g_wasOverflowed = false;
bool g_lastWriteSucceeded = false;
std::atomic<bool> g_armPending{false};
wchar_t g_directory[kPathChars]{};
wchar_t g_logFileName[kNameChars]{};
wchar_t g_logStem[kNameChars]{};

void setCopy(wchar_t* destination, std::size_t capacity,
             const wchar_t* source) noexcept {
    if (!destination || capacity == 0) return;
    destination[0] = L'\0';
    if (!source || !source[0]) return;
    wcsncpy_s(destination, capacity, source, _TRUNCATE);
}

void rejectInvalidToken() noexcept {
    if (g_isCapturing) g_wasOverflowed = true;
}

bool setLogPath(const wchar_t* logFilePath) noexcept {
    g_logFileName[0] = L'\0';
    g_logStem[0] = L'\0';
    g_directory[0] = L'\0';
    if (!logFilePath || !logFilePath[0]) return false;
    const wchar_t* slash = wcsrchr(logFilePath, L'\\');
    if (!slash || slash == logFilePath || !slash[1]) return false;
    const std::size_t directoryLength = static_cast<std::size_t>(slash - logFilePath);
    if (directoryLength >= kPathChars ||
        FAILED(StringCchCopyNW(g_directory, kPathChars, logFilePath,
                               directoryLength))) return false;
    setCopy(g_logFileName, kNameChars, slash + 1);
    const std::size_t fileLength = wcslen(g_logFileName);
    if (fileLength <= 4 || _wcsicmp(g_logFileName + fileLength - 4, L".log") != 0 ||
        _wcsnicmp(g_logFileName, L"edvr_gfx_", 9) != 0) return false;
    setCopy(g_logStem, kNameChars, g_logFileName);
    wchar_t* extension = wcsrchr(g_logStem, L'.');
    if (extension) *extension = L'\0';
    const DWORD attributes = GetFileAttributesW(logFilePath);
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

std::uint32_t moduleBuildStamp() noexcept {
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&moduleBuildStamp), &module) ||
        !module) return 0;
    const auto* base = reinterpret_cast<const std::uint8_t*>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
    return nt->FileHeader.TimeDateStamp;
}

struct Writer final {
    explicit Writer(HANDLE h) noexcept : file(h) {}
    HANDLE file;
    char buffer[64 * 1024]{};
    std::size_t used = 0;
    bool failed = false;
};

bool flush(Writer& writer) noexcept {
    if (writer.failed) return false;
    std::size_t offset = 0;
    while (offset < writer.used) {
        const DWORD part = static_cast<DWORD>(std::min<std::size_t>(
            writer.used - offset, 0x7fffffffu));
        DWORD written = 0;
        if (!WriteFile(writer.file, writer.buffer + offset, part, &written, nullptr) ||
            written == 0) {
            writer.failed = true;
            return false;
        }
        offset += written;
    }
    writer.used = 0;
    return true;
}

bool writeBytes(Writer& writer, const char* bytes, std::size_t length) noexcept {
    if (writer.failed) return false;
    while (length != 0) {
        const std::size_t room = sizeof(writer.buffer) - writer.used;
        if (room == 0 && !flush(writer)) return false;
        const std::size_t count = std::min(length, sizeof(writer.buffer) - writer.used);
        std::memcpy(writer.buffer + writer.used, bytes, count);
        writer.used += count;
        bytes += count;
        length -= count;
    }
    return true;
}

bool writeText(Writer& writer, const char* text) noexcept {
    return writeBytes(writer, text, std::strlen(text));
}

bool writeFmt(Writer& writer, const char* format, ...) noexcept {
    char scratch[1024];
    va_list args;
    va_start(args, format);
    const int n = _vsnprintf_s(scratch, sizeof(scratch), _TRUNCATE, format, args);
    va_end(args);
    return n >= 0 && writeBytes(writer, scratch, static_cast<std::size_t>(n));
}

bool writeDrawArgs(Writer& writer, const DrawArgs& args) noexcept {
    return writeFmt(writer, "{\"start\":%u,\"base\":%ld,\"startInstance\":%u}",
                    args.start, static_cast<long>(args.base), args.startInstance);
}

const char* triName(TriState value) noexcept {
    switch (value) {
    case TriState::No: return "no";
    case TriState::Yes: return "yes";
    default: return "unknown";
    }
}

const char* observationTriName(holo_scrim_observation::Tri value) noexcept {
    using holo_scrim_observation::Tri;
    switch (value) {
    case Tri::No: return "no";
    case Tri::Yes: return "yes";
    default: return "unknown";
    }
}

bool writeObservationGates(Writer& writer,
    const holo_scrim_observation::Gates& gates) noexcept {
    return writeFmt(writer,
        "{\"enabled\":\"%s\",\"shapeReached\":\"%s\","
        "\"shapeMatched\":\"%s\",\"helperReached\":\"%s\","
        "\"helperEnabled\":\"%s\",\"helperShapeReached\":\"%s\","
        "\"helperShapeMatched\":\"%s\"}",
        observationTriName(gates.enabled),
        observationTriName(gates.shapeReached),
        observationTriName(gates.shapeMatched),
        observationTriName(gates.helperReached),
        observationTriName(gates.helperEnabled),
        observationTriName(gates.helperShapeReached),
        observationTriName(gates.helperShapeMatched));
}

bool writeResourceObservation(Writer& writer,
    const holo_scrim_observation::ResourceObservation& resource) noexcept {
    return writeFmt(writer,
        "{\"source\":%u,\"resolveReached\":\"%s\","
        "\"resolved\":\"%s\",\"rawAvailable\":\"%s\","
        "\"texture2D\":\"%s\",\"a\":%u,\"b\":%u,\"fmt\":%u}",
        static_cast<unsigned>(resource.source),
        observationTriName(resource.resolveReached),
        observationTriName(resource.resolved),
        observationTriName(resource.rawAvailable),
        observationTriName(resource.texture2D), resource.a, resource.b,
        resource.fmt);
}

bool writeEyeSizeObservation(Writer& writer,
    const holo_scrim_observation::EyeSizeObservation& eye) noexcept {
    return writeFmt(writer,
        "{\"reached\":\"%s\",\"statePresent\":\"%s\","
        "\"result\":\"%s\",\"readMask\":%u,\"depthW\":%u,"
        "\"depthH\":%u,\"eyeW\":%u,\"eyeH\":%u,"
        "\"renderW\":%u,\"renderH\":%u}",
        observationTriName(eye.reached),
        observationTriName(eye.statePresent),
        observationTriName(eye.result), static_cast<unsigned>(eye.readMask),
        eye.depthW, eye.depthH, eye.eyeW, eye.eyeH, eye.renderW, eye.renderH);
}

bool validObservationTri(holo_scrim_observation::Tri value) noexcept {
    return static_cast<std::uint8_t>(value) <=
           static_cast<std::uint8_t>(holo_scrim_observation::Tri::Yes);
}

bool validObservationGates(
    const holo_scrim_observation::Gates& gates) noexcept {
    return validObservationTri(gates.enabled) &&
        validObservationTri(gates.shapeReached) &&
        validObservationTri(gates.shapeMatched) &&
        validObservationTri(gates.helperReached) &&
        validObservationTri(gates.helperEnabled) &&
        validObservationTri(gates.helperShapeReached) &&
        validObservationTri(gates.helperShapeMatched);
}

bool validResourceObservation(
    const holo_scrim_observation::ResourceObservation& resource) noexcept {
    using holo_scrim_observation::ResourceSource;
    return static_cast<std::uint8_t>(resource.source) <=
            static_cast<std::uint8_t>(ResourceSource::WarmCacheWithoutRawShadow) &&
        validObservationTri(resource.resolveReached) &&
        validObservationTri(resource.resolved) &&
        validObservationTri(resource.rawAvailable) &&
        validObservationTri(resource.texture2D) &&
        (resource.rawAvailable == holo_scrim_observation::Tri::Yes ||
         (resource.a == 0 && resource.b == 0 && resource.fmt == 0));
}

bool validEyeSizeObservation(
    const holo_scrim_observation::EyeSizeObservation& eye) noexcept {
    return validObservationTri(eye.reached) &&
        validObservationTri(eye.statePresent) &&
        validObservationTri(eye.result) && (eye.readMask & 0xC0u) == 0;
}

bool validHoloScrimFact(const PredicateFact& fact) noexcept {
    using namespace holo_scrim_observation;
    if (fact.known != TriState::Yes) return false;
    if (fact.kind == PredicateFactKind::Holo53) {
        const HoloObservation& observation = fact.holo;
        return fact.siteId == 53 && validObservationGates(observation.gates) &&
            validResourceObservation(observation.pattern) &&
            validResourceObservation(observation.depth) &&
            validEyeSizeObservation(observation.eyeSize) &&
            validObservationTri(observation.predicateResult) &&
            validObservationTri(observation.missNotedBefore) &&
            validObservationTri(observation.missNotedAfter) &&
            (observation.missedDeltaKnown || observation.missedDelta == 0) &&
            observation.missedDelta == observation.missedAfter - observation.missedBefore &&
            (!fact.detailsFinalized ||
             (observation.predicateResult == Tri::Yes ||
              observation.predicateResult == Tri::No));
    }
    if (fact.kind == PredicateFactKind::Scrim55) {
        const ScrimObservation& observation = fact.scrim;
        return fact.siteId == 55 && validObservationGates(observation.gates) &&
            validResourceObservation(observation.wash) &&
            validResourceObservation(observation.ui) &&
            validObservationTri(observation.predicateResult) &&
            (!fact.detailsFinalized ||
             observation.predicateResult == Tri::Yes ||
             observation.predicateResult == Tri::No);
    }
    return false;
}

const char* commandName(std::uint8_t kind) noexcept {
    switch (kind) {
    case 'D': return "draw";
    case 'I': return "draw-indexed";
    case 'N': return "draw-instanced";
    case 'X': return "draw-indexed-instanced";
    case 'A': return "draw-auto";
    case 'Z': return "draw-indexed-instanced-indirect";
    case 'Y': return "draw-instanced-indirect";
    default: return "unknown";
    }
}

std::uint32_t resourceOrdinal(std::uintptr_t identity) noexcept {
    if (identity == 0 || g_identityCount == 0) return 0;
    const auto* found = std::lower_bound(g_identities,
                                         g_identities + g_identityCount,
                                         identity);
    if (found == g_identities + g_identityCount || *found != identity) return 0;
    return static_cast<std::uint32_t>(found - g_identities) + 1;
}

void normalizeResourceIdentities() noexcept {
    g_identityCount = 0;
    for (std::uint32_t i = 0; i < g_drawCount; ++i) {
        const DrawFacts& f = g_records[i].facts;
        const std::uintptr_t values[6] = {
            f.vsIdentity, f.psIdentity, f.rtv0Identity, f.dsv0Identity,
            f.argumentBufferKnown ? f.argumentBufferIdentity : 0, 0};
        for (std::uintptr_t value : values) {
            if (value == 0) continue;
            if (g_identityCount >= kIdentitySlots) {
                g_wasOverflowed = true;
                continue;
            }
            g_identities[g_identityCount++] = value;
        }
    }
    std::sort(g_identities, g_identities + g_identityCount);
    g_identityCount = static_cast<std::uint32_t>(
        std::unique(g_identities, g_identities + g_identityCount) - g_identities);
}

bool writeTrace(Writer& writer, std::uint32_t completedFrameNo) noexcept {
    normalizeResourceIdentities();
    const std::uint32_t stamp = moduleBuildStamp();
    bool ok = writeText(writer,
        "{\"format\":\"edvr.draw-ladder-trace\",\"schemaVersion\":2,"
        "\"predicateFactVersion\":5,"
        "\"buildVersion\":\"");
    ok = ok && writeText(writer, EDVR_VERSION_STRING);
    ok = ok && writeFmt(writer,
        "\",\"buildStamp\":\"%08X\",\"logFile\":\"%S\",\"semantics\":{",
        stamp, g_logFileName);
    ok = ok && writeText(writer,
        "\"equivalence\":\"observed-selector-and-action-order\","
        "\"predicateEquivalence\":false,"
        "\"predicateNote\":\"Predicate fact version 5 independently re-evaluates DrawGateDisabledNone, EyeRangeSkip, the frozen 14a NightVisionClaim selector, WitchspaceStarsSkip site 6, offscreen census/quad skips, HoloClaim site 53, and ScrimClaim site 55 from raw consumed source facts; observed helper outputs are consistency checks, cached matches and SiteEvents are not selector inputs, and whole-ladder predicate equivalence is not established. No extra D3D queries or constant-buffer reads were performed.\","
        "\"identityNote\":\"Resource identities are per-capture ordinals; raw pointers are never serialized.\","
        "\"flagBits\":{\"frame\":{\"pluginDispatch\":1,\"runtimeFlat\":2,\"drawGateSubscribed\":4},"
        "\"draw\":{\"pluginDispatchEnabled\":1,\"distanceEnabled\":2,\"fssHealOn\":4,\"quadSkipArmed\":8},"
        "\"action\":{\"generatedDrawArgsUnavailable\":8192,\"gpuDrawArgsUnavailable\":16384,\"issueCountUnknown\":32768},"
        "\"forwardFacts\":{\"owner\":1,\"verdict\":2,\"verdictForwards\":4,\"familyAvailable\":8,"
        "\"initialUiTake\":16,\"afterUiTake\":32,\"worldReissue\":64,"
        "\"curveThisDraw\":128,\"introCurveThisDraw\":256,\"uiDepth\":512,"
        "\"holoDepth\":1024,\"composite\":2048,\"crispPending\":4096,\"issueBlocked\":8192}}},");
    ok = ok && writeFmt(writer,
        "\"frame\":{\"frameNo\":%u,\"completedFrameNo\":%u,"
        "\"configEpoch\":%u,\"eyeDrawsThisFrame\":%u,"
        "\"eyeDrawsLastFrame\":%u,\"sceneDrawsThisFrame\":%u,"
        "\"stateFlags\":%u,\"sceneCounters\":[%u,%u,%u,%u]},\"draws\":[",
        g_frame.frameNo, completedFrameNo,
        g_frame.configEpoch, g_frame.eyeDrawsThisFrame,
        g_frame.eyeDrawsLastFrame, g_frame.sceneDrawsThisFrame,
        g_frame.stateFlags, g_frame.sceneCounters[0], g_frame.sceneCounters[1],
        g_frame.sceneCounters[2], g_frame.sceneCounters[3]);
    if (!ok) return false;

    for (std::uint32_t i = 0; i < g_drawCount; ++i) {
        const DrawRecord& r = g_records[i];
        const DrawFacts& f = r.facts;
        if (i && !writeText(writer, ",")) return false;
        ok = writeFmt(writer,
            "{\"index\":%u,\"eyeDrawIndex\":%u,\"kind\":%u,"
            "\"command\":\"%s\",\"count\":%u,\"instances\":%u,\"args\":",
            i, f.eyeDrawIndex, static_cast<unsigned>(f.kind), commandName(f.kind),
            f.count, f.instances);
        ok = ok && writeDrawArgs(writer, f.args);
        ok = ok && writeFmt(writer,
            ",\"vsHash\":\"%016llX\",\"psHash\":\"%016llX\","
            "\"resources\":{\"vs\":%u,\"ps\":%u,\"rtv0\":%u,\"dsv0\":%u,\"argumentBuffer\":%u},"
            "\"argumentBufferKnown\":%s,\"argumentByteOffset\":%u,"
            "\"drawParametersKnown\":%s,"
            "\"rtv0Generation\":%u,\"dsv0Generation\":%u,"
            "\"rtv0Width\":%u,\"rtv0Height\":%u,\"route\":%u,"
            "\"sequence\":%u,\"candidateMask\":\"%016llX\",\"flags\":%u,"
            "\"sites\":[",
            static_cast<unsigned long long>(f.vsHash),
            static_cast<unsigned long long>(f.psHash),
            resourceOrdinal(f.vsIdentity), resourceOrdinal(f.psIdentity),
            resourceOrdinal(f.rtv0Identity), resourceOrdinal(f.dsv0Identity),
            resourceOrdinal(f.argumentBufferKnown ? f.argumentBufferIdentity : 0),
            f.argumentBufferKnown ? "true" : "false", f.argumentByteOffset,
            f.drawParametersKnown ? "true" : "false",
            f.rtv0Generation, f.dsv0Generation, f.rtv0Width, f.rtv0Height,
            static_cast<unsigned>(f.route), static_cast<unsigned>(f.sequence),
            static_cast<unsigned long long>(f.candidateMask), f.flags);
        if (!ok) return false;
        for (std::uint16_t j = 0; j < r.siteCount; ++j) {
            const SiteEvent& e = r.sites[j];
            if (j && !writeText(writer, ",")) return false;
            if (!writeFmt(writer,
                "{\"id\":%u,\"kind\":%u,\"outcome\":%u,\"flow\":%u,"
                "\"subsite\":%u,\"verdict\":%d}",
                e.id, e.kind, e.outcome, e.flow, e.subsite, e.verdict)) return false;
        }
        ok = writeText(writer, "],\"predicateFacts\":[");
        if (!ok) return false;
        for (std::uint8_t j = 0; j < r.predicateFactCount; ++j) {
            const PredicateFact& fact = r.predicateFacts[j];
            if (j && !writeText(writer, ",")) return false;
            if (fact.kind == PredicateFactKind::DrawGateWanted) {
                if (!writeFmt(writer,
                    "{\"siteId\":%u,\"kind\":1,\"known\":\"%s\","
                    "\"gateWanted\":\"%s\"}",
                    fact.siteId, triName(fact.known), triName(fact.gateWanted))) return false;
            } else if (fact.kind == PredicateFactKind::EyeRangeSkip) {
                if (!writeFmt(writer,
                    "{\"siteId\":%u,\"kind\":2,\"known\":\"%s\","
                    "\"eyeDrawIndex\":%u,\"ranges\":[",
                    fact.siteId, triName(fact.known), fact.eyeDrawIndex)) return false;
                for (std::uint8_t k = 0; k < fact.rangeCount; ++k) {
                    if (k && !writeText(writer, ",")) return false;
                    if (!writeFmt(writer, "[%u,%u]", fact.ranges[k].lo,
                                  fact.ranges[k].hi)) return false;
                }
                if (!writeFmt(writer,
                    "],\"censusSkippedDeltaKnown\":%s,"
                    "\"censusSkippedDelta\":%u}",
                    fact.censusSkippedDeltaKnown ? "true" : "false",
                    fact.censusSkippedDelta)) return false;
            } else if (fact.kind == PredicateFactKind::NightVisionClaim) {
                if (!writeFmt(writer,
                    "{\"siteId\":%u,\"kind\":3,\"known\":\"%s\","
                    "\"dispatchEnabled\":\"%s\",\"activeMaskKnown\":\"%s\","
                    "\"activePluginMask\":\"%016llX\",\"candidateKnown\":\"%s\","
                    "\"candidatePresent\":\"%s\",\"modeKnown\":\"%s\",\"mode\":%u,"
                    "\"shapeReached\":\"%s\",\"shapeMatched\":\"%s\","
                    "\"callbackReached\":\"%s\",\"callbackModeKnown\":\"%s\","
                    "\"callbackMode\":%u,\"failedKnown\":\"%s\",\"failed\":\"%s\"}",
                    fact.siteId, triName(fact.known), triName(fact.dispatchEnabled),
                    triName(fact.activeMaskKnown),
                    static_cast<unsigned long long>(fact.activePluginMask),
                    triName(fact.candidateKnown), triName(fact.candidatePresent),
                    triName(fact.modeKnown), fact.mode, triName(fact.shapeReached),
                    triName(fact.shapeMatched), triName(fact.callbackReached),
                    triName(fact.callbackModeKnown), fact.callbackMode,
                    triName(fact.failedKnown), triName(fact.failed))) return false;
            } else if (fact.kind == PredicateFactKind::WitchspaceStarsSkip) {
                if (!writeFmt(writer,
                    "{\"siteId\":%u,\"kind\":4,\"known\":\"%s\","
                    "\"interestMaskKnown\":\"%s\",\"legacyInterestMask\":\"%016llX\","
                    "\"helperReached\":\"%s\",\"hiddenKnown\":\"%s\",\"hidden\":\"%s\","
                    "\"contextKnown\":\"%s\",\"contextValid\":\"%s\","
                    "\"shapeReached\":\"%s\",\"shapeMatched\":\"%s\","
                    "\"hashKnown\":\"%s\",\"hashSource\":%u,\"vsHash\":\"%016llX\","
                    "\"skippedDeltaKnown\":%s,\"skippedDelta\":%u}",
                    fact.siteId, triName(fact.known), triName(fact.interestMaskKnown),
                    static_cast<unsigned long long>(fact.legacyInterestMask),
                    triName(fact.starsHelperReached),
                    triName(fact.hiddenKnown), triName(fact.hidden),
                    triName(fact.contextKnown), triName(fact.contextValid),
                    triName(fact.starsShapeReached), triName(fact.starsShapeMatched),
                    triName(fact.starsHashKnown), fact.starsHashSource,
                    static_cast<unsigned long long>(fact.starsVsHash),
                    fact.starsSkippedDeltaKnown ? "true" : "false",
                    fact.starsSkippedDelta)) return false;
            } else if (fact.kind == PredicateFactKind::OffscreenCensusSkip ||
                       fact.kind == PredicateFactKind::OffscreenQuadSkip) {
                if (!writeFmt(writer,
                    "{\"siteId\":%u,\"kind\":%u,\"known\":\"%s\","
                    "\"offscreenRuleCount\":%u,\"offscreenRules\":[",
                    fact.siteId, static_cast<unsigned>(fact.kind),
                    triName(fact.known), fact.offscreenRuleCount)) return false;
                for (std::uint8_t k = 0; k < fact.offscreenRuleCount; ++k) {
                    const PredicateOffscreenRule& rule = fact.offscreenRules[k];
                    if (k && !writeText(writer, ",")) return false;
                    if (!writeFmt(writer,
                        "{\"kind\":%u,\"count\":%u,\"w\":%u,\"h\":%u}",
                        static_cast<unsigned>(rule.kind), rule.count,
                        rule.w, rule.h)) return false;
                }
                if (!writeFmt(writer,
                    "],\"quadArmed\":\"%s\","
                    "\"offscreenEyeDrawsLastFrame\":%u,"
                    "\"offscreenProbeReached\":\"%s\","
                    "\"offscreenProbeResolved\":\"%s\","
                    "\"offscreenProbeTexture2D\":\"%s\","
                    "\"offscreenTargetW\":%u,\"offscreenTargetH\":%u,"
                    "\"censusSkippedDeltaKnown\":%s,"
                    "\"censusSkippedDelta\":%u}",
                    triName(fact.quadArmed), fact.offscreenEyeDrawsLastFrame,
                    triName(fact.offscreenProbeReached),
                    triName(fact.offscreenProbeResolved),
                    triName(fact.offscreenProbeTexture2D),
                    fact.offscreenTargetW, fact.offscreenTargetH,
                    fact.censusSkippedDeltaKnown ? "true" : "false",
                    fact.censusSkippedDelta)) return false;
            } else if (fact.kind == PredicateFactKind::Holo53) {
                const holo_scrim_observation::HoloObservation& observed = fact.holo;
                if (!writeFmt(writer,
                    "{\"siteId\":53,\"kind\":7,\"known\":\"%s\",\"gates\":",
                    triName(fact.known)) ||
                    !writeObservationGates(writer, observed.gates) ||
                    !writeText(writer, ",\"pattern\":") ||
                    !writeResourceObservation(writer, observed.pattern) ||
                    !writeText(writer, ",\"depth\":") ||
                    !writeResourceObservation(writer, observed.depth) ||
                    !writeText(writer, ",\"eyeSize\":") ||
                    !writeEyeSizeObservation(writer, observed.eyeSize) ||
                    !writeFmt(writer,
                        ",\"predicateResult\":\"%s\",\"missedBefore\":%llu,"
                        "\"missedAfter\":%llu,\"missNotedBefore\":\"%s\","
                        "\"missNotedAfter\":\"%s\",\"missedDeltaKnown\":%s,"
                        "\"missedDelta\":%llu}",
                        observationTriName(observed.predicateResult),
                        static_cast<unsigned long long>(observed.missedBefore),
                        static_cast<unsigned long long>(observed.missedAfter),
                        observationTriName(observed.missNotedBefore),
                        observationTriName(observed.missNotedAfter),
                        observed.missedDeltaKnown ? "true" : "false",
                        static_cast<unsigned long long>(observed.missedDelta))) return false;
            } else if (fact.kind == PredicateFactKind::Scrim55) {
                const holo_scrim_observation::ScrimObservation& observed = fact.scrim;
                if (!writeFmt(writer,
                    "{\"siteId\":55,\"kind\":8,\"known\":\"%s\",\"gates\":",
                    triName(fact.known)) ||
                    !writeObservationGates(writer, observed.gates) ||
                    !writeText(writer, ",\"wash\":") ||
                    !writeResourceObservation(writer, observed.wash) ||
                    !writeText(writer, ",\"ui\":") ||
                    !writeResourceObservation(writer, observed.ui) ||
                    !writeFmt(writer, ",\"predicateResult\":\"%s\"}",
                        observationTriName(observed.predicateResult))) return false;
            } else {
                return false;
            }
        }
        ok = writeFmt(writer,
            "],\"winnerSiteId\":%d,\"verdict\":%d,\"forwardFacts\":",
            r.winnerSiteId, r.verdictOrdinal);
        if (!ok) return false;
        if (!r.hasForwardFacts) {
            if (!writeText(writer, "null,\"actions\":[")) return false;
        } else {
            const ForwardFacts& d = r.forwardFacts;
            ok = writeFmt(writer,
                "{\"presentMask\":%u,\"verdictOrdinal\":%d,"
                "\"family\":%u,\"familyAvailable\":\"%s\","
                "\"owner\":\"%s\",\"verdictForwards\":\"%s\","
                "\"initialUiTake\":\"%s\",\"afterUiTake\":\"%s\","
                "\"worldReissue\":\"%s\",\"curveThisDraw\":\"%s\","
                "\"introCurveThisDraw\":\"%s\",\"uiDepth\":\"%s\","
                "\"holoDepth\":\"%s\",\"composite\":\"%s\","
                "\"crispPending\":\"%s\",\"issueBlocked\":\"%s\"},"
                "\"actions\":[",
                d.presentMask, d.verdictOrdinal, d.family,
                triName(d.familyAvailable), triName(d.owner),
                triName(d.verdictForwards),
                triName(d.initialUiTake), triName(d.afterUiTake),
                triName(d.worldReissue), triName(d.curveThisDraw),
                triName(d.introCurveThisDraw), triName(d.uiDepth),
                triName(d.holoDepth), triName(d.composite),
                triName(d.crispPending), triName(d.issueBlocked));
            if (!ok) return false;
        }
        for (std::uint16_t j = 0; j < r.actionCount; ++j) {
            const draw_ladder::ActionRecord& a = r.actions[j];
            if (j && !writeText(writer, ",")) return false;
            if (!writeFmt(writer,
                "{\"id\":%u,\"phase\":%u,\"outcome\":%u,\"call\":%u,"
                "\"flags\":%u,\"issueCount\":%u,\"issueCountKnown\":%s,\"count\":%u,"
                "\"instances\":%u,\"start\":%u,\"startInstance\":%u,"
                "\"baseVertex\":%ld}",
                static_cast<unsigned>(r.actionIds[j]),
                static_cast<unsigned>(a.phase), static_cast<unsigned>(a.outcome),
                static_cast<unsigned>(a.call), static_cast<unsigned>(a.flags),
                static_cast<unsigned>(a.issueCount),
                (a.flags & draw_ladder::kActionIssueCountUnknown) ? "false" : "true",
                a.count, a.instances,
                a.start, a.startInstance, static_cast<long>(a.baseVertex))) return false;
        }
        if (!writeText(writer, "]}")) return false;
    }
    if (!writeFmt(writer,
        "],\"footer\":{\"complete\":%s,\"truncated\":%s,"
        "\"overflow\":%s,\"drawCount\":%u}}",
        (!g_wasOverflowed && completedFrameNo == g_frame.frameNo) ? "true" : "false",
        g_wasOverflowed ? "true" : "false",
        g_wasOverflowed ? "true" : "false", g_drawCount)) return false;
    return true;
}

bool validToken(Token token) noexcept {
    return g_isCapturing && token.generation == g_generation &&
           token.drawIndex < g_drawCount && token.drawIndex < kMaxDraws;
}

}  // namespace

namespace detail {
std::atomic<bool> g_captureActive{false};
}  // namespace detail

void configure(bool enabled, const wchar_t* logFilePath) noexcept {
    // Config reloads can run while a one-frame capture is in progress. The
    // caller must defer them until the next owner boundary after frameEnd;
    // never free the active fixed-capacity arrays mid-capture.
    if (g_isCapturing) return;
    const bool wasEnabled = g_enabled.load(std::memory_order_acquire);
    if (!enabled && !wasEnabled && !g_records && !g_identities) return;
    if (enabled && wasEnabled && logFilePath && logFilePath[0]) {
        wchar_t oldPath[kPathChars]{};
        if (SUCCEEDED(StringCchCopyW(oldPath, kPathChars, g_directory)) &&
            SUCCEEDED(StringCchCatW(oldPath, kPathChars, L"\\")) &&
            SUCCEEDED(StringCchCatW(oldPath, kPathChars, g_logFileName)) &&
            _wcsicmp(oldPath, logFilePath) == 0) return;
    }
    detail::g_captureActive.store(false, std::memory_order_relaxed);
    g_armPending.store(false, std::memory_order_relaxed);
    g_isCapturing = false;
    g_enabled.store(false, std::memory_order_relaxed);
    g_status.store(enabled ? Status::PathInvalid : Status::Disabled,
                   std::memory_order_relaxed);
    g_wasOverflowed = false;
    g_lastWriteSucceeded = false;
    if (g_records) { delete[] g_records; g_records = nullptr; }
    if (g_identities) { delete[] g_identities; g_identities = nullptr; }
    g_directory[0] = L'\0';
    g_logFileName[0] = L'\0';
    g_logStem[0] = L'\0';
    if (!enabled) return;
    if (!setLogPath(logFilePath)) return;

    // This is the only allocation site. It runs during cold initialization,
    // never from a hook or from an armed draw.
    g_records = new (std::nothrow) DrawRecord[kMaxDraws];
    g_identities = new (std::nothrow) std::uintptr_t[kIdentitySlots];
    if (!g_records || !g_identities) {
        if (g_records) { delete[] g_records; g_records = nullptr; }
        if (g_identities) { delete[] g_identities; g_identities = nullptr; }
        g_directory[0] = L'\0';
        g_logFileName[0] = L'\0';
        g_logStem[0] = L'\0';
        g_status.store(Status::AllocationFailed, std::memory_order_relaxed);
        return;
    }
    g_enabled.store(true, std::memory_order_release);
    g_status.store(Status::Ready, std::memory_order_relaxed);
}

ShutdownResult shutdown() noexcept {
    // The caller has already removed the draw hooks and quiesced callbacks.
    // Snapshot the visible state before clearing it so a cold lifecycle
    // breadcrumb can report an armed/partial capture as discarded.
    const ShutdownResult result{
        status(),
        g_armPending.load(std::memory_order_relaxed),
        g_isCapturing,
    };

    detail::g_captureActive.store(false, std::memory_order_relaxed);
    g_armPending.store(false, std::memory_order_relaxed);
    g_enabled.store(false, std::memory_order_release);
    g_isCapturing = false;
    g_wasOverflowed = false;
    g_lastWriteSucceeded = false;
    g_drawCount = 0;
    g_identityCount = 0;
    g_frame = FrameFacts{};
    if (g_records) {
        delete[] g_records;
        g_records = nullptr;
    }
    if (g_identities) {
        delete[] g_identities;
        g_identities = nullptr;
    }
    g_directory[0] = L'\0';
    g_logFileName[0] = L'\0';
    g_logStem[0] = L'\0';
    ++g_generation;
    if (g_generation == 0) ++g_generation;
    g_status.store(Status::Disabled, std::memory_order_release);
    return result;
}

void armManual() noexcept {
    if (!g_enabled.load(std::memory_order_acquire) ||
        detail::g_captureActive.load(std::memory_order_relaxed)) return;
    bool expected = false;
    // status() derives Armed from g_armPending. Publishing a separate Armed
    // value here could race the owner thread consuming the arm and leave a
    // stale Armed breadcrumb after capture has started or completed.
    g_armPending.compare_exchange_strong(expected, true,
                                         std::memory_order_relaxed);
}

void frameBegin(const FrameFacts& facts) noexcept {
    if (!g_enabled.load(std::memory_order_relaxed) || g_isCapturing ||
        !g_armPending.exchange(false, std::memory_order_relaxed)) return;
    g_frame = facts;
    g_drawCount = 0;
    g_identityCount = 0;
    g_wasOverflowed = false;
    g_isCapturing = true;
    g_status.store(Status::Capturing, std::memory_order_relaxed);
    ++g_generation;
    if (g_generation == 0) ++g_generation;
    detail::g_captureActive.store(true, std::memory_order_relaxed);
}

Token beginDraw(const DrawFacts& facts) noexcept {
    // The call is reachable only through the single outer draw-level guard;
    // same-thread owner-frame boundaries cannot change capture state here.
    if (!g_isCapturing) return {};
    if (g_drawCount >= kMaxDraws) {
        g_wasOverflowed = true;
        return {};
    }
    const std::uint32_t index = g_drawCount++;
    DrawRecord& record = g_records[index];
    record = DrawRecord{};
    record.facts = facts;
    return {index, g_generation};
}

void appendSite(Token token, std::uint16_t id, std::uint8_t kind,
                std::uint8_t outcome, std::uint8_t flow,
                std::uint16_t subsite, std::int16_t siteVerdict) noexcept {
    if (!validToken(token)) { rejectInvalidToken(); return; }
    DrawRecord& record = g_records[token.drawIndex];
    if (record.finalized) { rejectInvalidToken(); return; }
    if (record.siteCount >= kMaxSiteEventsPerDraw) {
        g_wasOverflowed = true;
        return;
    }
    SiteEvent& event = record.sites[record.siteCount++];
    event.id = id;
    event.kind = kind;
    event.outcome = outcome;
    event.flow = flow;
    event.subsite = subsite;
    event.verdict = siteVerdict;
}

void appendAction(Token token, std::uint16_t id,
                  const draw_ladder::ActionRecord& action) noexcept {
    if (!validToken(token)) { rejectInvalidToken(); return; }
    DrawRecord& record = g_records[token.drawIndex];
    if (record.finalized) { rejectInvalidToken(); return; }
    if (record.actionCount >= kMaxActionEventsPerDraw) {
        g_wasOverflowed = true;
        return;
    }
    // Keep the stable ActionId beside the shared pointer-free action facts.
    record.actions[record.actionCount] = action;
    // ActionRecord intentionally carries the call facts; ActionId is separate.
    record.actionIds[record.actionCount] = id;
    ++record.actionCount;
}

void updateCandidates(Token token, std::uint64_t mask) noexcept {
    if (!validToken(token)) { rejectInvalidToken(); return; }
    DrawRecord& record = g_records[token.drawIndex];
    if (record.finalized) { rejectInvalidToken(); return; }
    record.facts.candidateMask = mask;
}

void updateRoute(Token token, draw_ladder::RouteId route,
                 draw_ladder::SequenceId sequence) noexcept {
    if (!validToken(token)) { rejectInvalidToken(); return; }
    DrawRecord& record = g_records[token.drawIndex];
    if (record.finalized) { rejectInvalidToken(); return; }
    record.facts.route = route;
    record.facts.sequence = sequence;
}

void recordForwardFacts(Token token, const ForwardFacts& facts) noexcept {
    if (!validToken(token)) { rejectInvalidToken(); return; }
    DrawRecord& record = g_records[token.drawIndex];
    if (record.finalized || record.hasForwardFacts) { rejectInvalidToken(); return; }
    record.forwardFacts = facts;
    record.hasForwardFacts = true;
}

namespace {
bool validWitchspaceFact(const PredicateFact& f) noexcept {
    const auto validTri = [](TriState value) noexcept {
        return static_cast<std::uint8_t>(value) <=
               static_cast<std::uint8_t>(TriState::Yes);
    };
    if (f.siteId != 6 || f.known != TriState::Yes ||
        f.interestMaskKnown != TriState::Yes ||
        (f.legacyInterestMask >> 8) != 0 ||
        !validTri(f.starsHelperReached) ||
        !validTri(f.hiddenKnown) || !validTri(f.hidden) ||
        !validTri(f.contextKnown) || !validTri(f.contextValid) ||
        !validTri(f.starsShapeReached) || !validTri(f.starsShapeMatched) ||
        !validTri(f.starsHashKnown) || f.starsHashSource > 3 ||
        (!f.starsSkippedDeltaKnown && f.starsSkippedDelta != 0) ||
        f.starsSkippedDelta > 1) return false;
    if ((f.hiddenKnown == TriState::Yes &&
         f.hidden != TriState::Yes && f.hidden != TriState::No) ||
        (f.hiddenKnown == TriState::Unknown && f.hidden != TriState::Unknown) ||
        (f.contextKnown == TriState::Yes &&
         f.contextValid != TriState::Yes && f.contextValid != TriState::No) ||
        (f.contextKnown == TriState::Unknown &&
         f.contextValid != TriState::Unknown) ||
        f.starsShapeReached == TriState::No ||
        (f.starsShapeReached == TriState::Unknown &&
         f.starsShapeMatched != TriState::Unknown) ||
        (f.starsShapeReached == TriState::Yes &&
         f.starsShapeMatched != TriState::Yes && f.starsShapeMatched != TriState::No))
        return false;
    if ((f.starsHashKnown == TriState::Unknown &&
         (f.starsHashSource != 0 || f.starsVsHash != 0)) ||
        (f.starsHashKnown == TriState::Yes && f.starsHashSource == 1 &&
         f.starsVsHash == 0) ||
        (f.starsHashKnown == TriState::Yes && f.starsHashSource == 2 &&
         f.starsVsHash != 0)) return false;
    const bool interested = (f.legacyInterestMask & (1ull << 1)) != 0;
    if (!interested) {
        const bool priorMiss = f.hidden == TriState::No ||
                               f.contextValid == TriState::No ||
                               f.starsShapeMatched == TriState::No;
        const bool shapeExpected = f.hidden == TriState::Yes &&
                                  f.contextValid == TriState::Yes;
        const bool cachedHashKnown = f.starsHashKnown == TriState::Yes &&
            f.starsHashSource == 1 && f.starsVsHash != 0;
        const bool hashUnavailable = f.starsHashKnown == TriState::Unknown &&
            f.starsHashSource == 0 && f.starsVsHash == 0;
        return f.detailsFinalized && f.starsHelperReached == TriState::No &&
            f.hiddenKnown == TriState::Yes &&
            f.contextKnown == TriState::Yes &&
            ((shapeExpected && f.starsShapeReached == TriState::Yes &&
              (f.starsShapeMatched == TriState::Yes ||
               f.starsShapeMatched == TriState::No)) ||
             (!shapeExpected && f.starsShapeReached == TriState::Unknown &&
              f.starsShapeMatched == TriState::Unknown)) &&
            (priorMiss || f.starsShapeMatched == TriState::Yes) &&
            (!cachedHashKnown || (shapeExpected &&
                                  f.starsShapeMatched == TriState::Yes)) &&
            (hashUnavailable || (f.starsShapeMatched == TriState::Yes &&
                                 cachedHashKnown)) &&
            !f.starsSkippedDeltaKnown;
    }
    if (!f.detailsFinalized) {
        return f.starsHelperReached == TriState::Unknown &&
            f.hiddenKnown == TriState::Unknown && f.hidden == TriState::Unknown &&
            f.contextKnown == TriState::Unknown && f.contextValid == TriState::Unknown &&
            f.starsShapeReached == TriState::Unknown &&
            f.starsShapeMatched == TriState::Unknown &&
            f.starsHashKnown == TriState::Unknown && f.starsHashSource == 0 &&
            f.starsVsHash == 0 && !f.starsSkippedDeltaKnown;
    }
    if (f.starsHelperReached != TriState::Yes ||
        f.hiddenKnown != TriState::Yes ||
        !f.starsSkippedDeltaKnown) return false;
    if (f.hidden == TriState::No) {
        return f.contextKnown == TriState::Unknown &&
            f.contextValid == TriState::Unknown &&
            f.starsShapeReached == TriState::Unknown &&
            f.starsShapeMatched == TriState::Unknown &&
            f.starsHashKnown == TriState::Unknown && f.starsHashSource == 0 &&
            f.starsVsHash == 0 && f.starsSkippedDelta == 0;
    }
    if (f.contextKnown != TriState::Yes) return false;
    if (f.contextValid == TriState::No) {
        return f.starsShapeReached == TriState::Unknown &&
            f.starsShapeMatched == TriState::Unknown &&
            f.starsHashKnown == TriState::Unknown && f.starsHashSource == 0 &&
            f.starsVsHash == 0 && f.starsSkippedDelta == 0;
    }
    if (f.contextValid != TriState::Yes ||
        f.starsShapeReached != TriState::Yes) return false;
    if (f.starsShapeMatched == TriState::No) {
        return f.starsHashKnown == TriState::Unknown && f.starsHashSource == 0 &&
            f.starsVsHash == 0 && f.starsSkippedDelta == 0;
    }
    return f.starsShapeMatched == TriState::Yes &&
        f.starsHashKnown == TriState::Yes && f.starsHashSource >= 1 &&
        f.starsSkippedDelta <= 1;
}

bool validOffscreenFact(const PredicateFact& f) noexcept {
    const auto validTri = [](TriState value) noexcept {
        return static_cast<std::uint8_t>(value) <=
               static_cast<std::uint8_t>(TriState::Yes);
    };
    if (f.known != TriState::Yes || !f.detailsFinalized ||
        f.offscreenRuleCount > 4 ||
        !validTri(f.quadArmed) || !validTri(f.offscreenProbeReached) ||
        !validTri(f.offscreenProbeResolved) ||
        !validTri(f.offscreenProbeTexture2D) ||
        (!f.censusSkippedDeltaKnown && f.censusSkippedDelta != 0) ||
        f.censusSkippedDelta > 1) return false;
    for (std::uint8_t i = 0; i < f.offscreenRuleCount; ++i) {
        const PredicateOffscreenRule& rule = f.offscreenRules[i];
        const bool validKind = rule.kind == 0 || rule.kind == 'D' ||
            rule.kind == 'I' || rule.kind == 'N' || rule.kind == 'X';
        const bool zeroUnarmedQuadRule =
            f.kind == PredicateFactKind::OffscreenQuadSkip &&
            f.quadArmed == TriState::No && i == 0 &&
            !rule.kind && !rule.count && !rule.w && !rule.h;
        if (!validKind || (!zeroUnarmedQuadRule && (!rule.w || !rule.h)) ||
            (rule.kind == 0 && rule.count != 0)) return false;
    }
    for (std::uint8_t i = f.offscreenRuleCount; i < 4; ++i) {
        const PredicateOffscreenRule& rule = f.offscreenRules[i];
        if (rule.kind || rule.count || rule.w || rule.h) return false;
    }

    if (f.kind == PredicateFactKind::OffscreenCensusSkip) {
        if (f.siteId != 24 || f.quadArmed != TriState::Unknown ||
            f.offscreenEyeDrawsLastFrame != 0 ||
            !f.censusSkippedDeltaKnown ||
            f.offscreenProbeReached != (f.offscreenRuleCount
                ? TriState::Yes : TriState::No)) return false;
        if (f.offscreenProbeReached == TriState::No) {
            return f.offscreenProbeResolved == TriState::Unknown &&
                f.offscreenProbeTexture2D == TriState::Unknown &&
                f.offscreenTargetW == 0 && f.offscreenTargetH == 0 &&
                f.censusSkippedDelta == 0;
        }
        if (f.offscreenProbeResolved == TriState::No) {
            return f.offscreenProbeTexture2D == TriState::Unknown &&
                f.offscreenTargetW == 0 && f.offscreenTargetH == 0 &&
                f.censusSkippedDelta == 0;
        }
        if (f.offscreenProbeResolved != TriState::Yes ||
            (f.offscreenProbeTexture2D != TriState::Yes &&
             f.offscreenProbeTexture2D != TriState::No)) return false;
        if (f.offscreenProbeTexture2D == TriState::No &&
            (f.offscreenTargetW != 0 || f.offscreenTargetH != 0)) return false;
        if (f.offscreenProbeTexture2D == TriState::Yes &&
            (!f.offscreenTargetW || !f.offscreenTargetH)) return false;
        return true;
    }

    if (f.kind == PredicateFactKind::OffscreenQuadSkip) {
        if (f.siteId != 26 || f.offscreenRuleCount != 1 ||
            (f.quadArmed != TriState::No && f.quadArmed != TriState::Yes) ||
            f.censusSkippedDeltaKnown || f.censusSkippedDelta != 0) return false;
        const PredicateOffscreenRule& rule = f.offscreenRules[0];
        if (f.quadArmed == TriState::No &&
            (rule.kind || rule.count || rule.w || rule.h)) return false;
        if (f.quadArmed == TriState::Yes &&
            (!rule.kind || !rule.count || !rule.w || !rule.h)) return false;
        if (f.offscreenProbeReached == TriState::No) {
            return f.offscreenProbeResolved == TriState::Unknown &&
                f.offscreenProbeTexture2D == TriState::Unknown &&
                f.offscreenTargetW == 0 && f.offscreenTargetH == 0;
        }
        if (f.offscreenProbeReached != TriState::Yes) return false;
        if (f.offscreenProbeResolved == TriState::No) {
            return f.offscreenProbeTexture2D == TriState::Unknown &&
                f.offscreenTargetW == 0 && f.offscreenTargetH == 0;
        }
        if (f.offscreenProbeResolved != TriState::Yes ||
            (f.offscreenProbeTexture2D != TriState::Yes &&
             f.offscreenProbeTexture2D != TriState::No)) return false;
        if (f.offscreenProbeTexture2D == TriState::No &&
            (f.offscreenTargetW != 0 || f.offscreenTargetH != 0)) return false;
        if (f.offscreenProbeTexture2D == TriState::Yes &&
            (!f.offscreenTargetW || !f.offscreenTargetH)) return false;
        return true;
    }
    return false;
}
}  // namespace

void appendPredicateFact(Token token, const PredicateFact& fact) noexcept {
    if (!validToken(token)) { rejectInvalidToken(); return; }
    DrawRecord& record = g_records[token.drawIndex];
    if (record.finalized || record.predicateFactCount >= kMaxPredicateFactsPerDraw ||
        static_cast<std::uint8_t>(fact.known) > static_cast<std::uint8_t>(TriState::Yes) ||
        fact.known == TriState::No ||
        static_cast<std::uint8_t>(fact.gateWanted) > static_cast<std::uint8_t>(TriState::Yes) ||
        (fact.kind == PredicateFactKind::DrawGateWanted && fact.siteId != 3) ||
        (fact.kind == PredicateFactKind::EyeRangeSkip &&
         (fact.siteId != 49 || fact.rangeCount > 4)) ||
        (fact.kind == PredicateFactKind::NightVisionClaim && fact.siteId != 50) ||
        (fact.kind == PredicateFactKind::WitchspaceStarsSkip &&
         !validWitchspaceFact(fact)) ||
        ((fact.kind == PredicateFactKind::OffscreenCensusSkip ||
          fact.kind == PredicateFactKind::OffscreenQuadSkip) &&
         !validOffscreenFact(fact)) ||
        ((fact.kind == PredicateFactKind::Holo53 ||
          fact.kind == PredicateFactKind::Scrim55) &&
         !validHoloScrimFact(fact)) ||
        (fact.kind == PredicateFactKind::DrawGateWanted &&
         ((fact.known == TriState::Yes && fact.gateWanted == TriState::Unknown) ||
          (fact.known == TriState::Unknown && fact.gateWanted != TriState::Unknown))) ||
        (fact.kind == PredicateFactKind::EyeRangeSkip &&
         (fact.known == TriState::Unknown && fact.rangeCount != 0)) ||
        (fact.kind == PredicateFactKind::EyeRangeSkip &&
         ((!fact.censusSkippedDeltaKnown && fact.censusSkippedDelta != 0) ||
          fact.censusSkippedDelta > 1)) ||
        (fact.kind == PredicateFactKind::NightVisionClaim &&
         (static_cast<std::uint8_t>(fact.dispatchEnabled) > 2 ||
          fact.dispatchEnabled == TriState::Unknown ||
          static_cast<std::uint8_t>(fact.activeMaskKnown) > 2 ||
          static_cast<std::uint8_t>(fact.candidateKnown) > 2 ||
          static_cast<std::uint8_t>(fact.candidatePresent) > 2 ||
          static_cast<std::uint8_t>(fact.modeKnown) > 2 || fact.mode > 3 ||
          (fact.modeKnown == TriState::Unknown && fact.mode != 0) ||
          static_cast<std::uint8_t>(fact.shapeReached) > 2 ||
          static_cast<std::uint8_t>(fact.shapeMatched) > 2 ||
          static_cast<std::uint8_t>(fact.callbackReached) > 2 ||
          static_cast<std::uint8_t>(fact.callbackModeKnown) > 2 ||
          fact.callbackMode > 3 || static_cast<std::uint8_t>(fact.failedKnown) > 2 ||
          static_cast<std::uint8_t>(fact.failed) > 2 ||
          (fact.dispatchEnabled == TriState::No &&
           (fact.activeMaskKnown != TriState::Unknown ||
            fact.candidateKnown != TriState::Unknown ||
            fact.shapeReached != TriState::No ||
            fact.callbackReached != TriState::No)) ||
          (fact.dispatchEnabled == TriState::Yes &&
           (fact.activeMaskKnown != TriState::Yes ||
            fact.candidateKnown != TriState::Yes ||
            fact.candidatePresent == TriState::Unknown)) ||
          (fact.candidatePresent == TriState::No &&
           (fact.shapeReached != TriState::No ||
            fact.callbackReached != TriState::No)) ||
          (fact.candidatePresent == TriState::Yes && fact.detailsFinalized &&
           (fact.shapeReached != TriState::Yes ||
            fact.shapeMatched == TriState::Unknown ||
            (fact.shapeMatched == TriState::No &&
             fact.callbackReached != TriState::No) ||
            (fact.shapeMatched == TriState::Yes &&
             fact.callbackReached == TriState::No) ||
            (fact.callbackReached == TriState::Yes &&
             fact.callbackModeKnown != TriState::Yes) ||
            (fact.callbackModeKnown == TriState::Yes &&
             fact.callbackReached != TriState::Yes) ||
            (fact.failedKnown == TriState::Yes &&
             (fact.callbackReached != TriState::Yes ||
              fact.failed == TriState::Unknown)) ||
            (fact.failedKnown != TriState::Yes &&
             fact.failed != TriState::Unknown))))) ||
        (fact.kind != PredicateFactKind::DrawGateWanted &&
        fact.kind != PredicateFactKind::EyeRangeSkip &&
        fact.kind != PredicateFactKind::NightVisionClaim &&
         fact.kind != PredicateFactKind::WitchspaceStarsSkip &&
         fact.kind != PredicateFactKind::OffscreenCensusSkip &&
         fact.kind != PredicateFactKind::OffscreenQuadSkip &&
         fact.kind != PredicateFactKind::Holo53 &&
         fact.kind != PredicateFactKind::Scrim55)) {
        g_wasOverflowed = true;
        return;
    }
    for (std::uint8_t i = 0; i < record.predicateFactCount; ++i) {
        if (record.predicateFacts[i].siteId == fact.siteId) {
            g_wasOverflowed = true;
            return;
        }
    }
    if (fact.kind == PredicateFactKind::EyeRangeSkip) {
        for (std::uint8_t i = 0; i < fact.rangeCount; ++i) {
            if (fact.ranges[i].lo == 0 || fact.ranges[i].hi < fact.ranges[i].lo) {
                g_wasOverflowed = true;
                return;
            }
        }
    }
    record.predicateFacts[record.predicateFactCount++] = fact;
}

void completeNightVisionFact(Token token, const PredicateFact& fact) noexcept {
    if (!validToken(token)) { rejectInvalidToken(); return; }
    DrawRecord& record = g_records[token.drawIndex];
    if (record.finalized || fact.kind != PredicateFactKind::NightVisionClaim ||
        fact.siteId != 50) { rejectInvalidToken(); return; }
    for (std::uint8_t i = 0; i < record.predicateFactCount; ++i) {
        PredicateFact& stored = record.predicateFacts[i];
        if (stored.siteId != 50 || stored.kind != PredicateFactKind::NightVisionClaim)
            continue;
        if (stored.detailsFinalized || stored.candidatePresent != TriState::Yes ||
            fact.shapeReached != TriState::Yes ||
            (fact.shapeMatched != TriState::Yes && fact.shapeMatched != TriState::No) ||
            (fact.shapeMatched == TriState::No &&
             fact.callbackReached != TriState::No) ||
            (fact.shapeMatched == TriState::Yes &&
             fact.callbackReached == TriState::No) ||
            (fact.callbackReached == TriState::Yes &&
             fact.callbackModeKnown != TriState::Yes) ||
            (fact.callbackModeKnown == TriState::Yes &&
             fact.callbackReached != TriState::Yes) ||
            (fact.failedKnown == TriState::Yes &&
             (fact.callbackReached != TriState::Yes ||
              fact.failed == TriState::Unknown)) ||
            (fact.failedKnown != TriState::Yes &&
             fact.failed != TriState::Unknown)) {
            g_wasOverflowed = true;
            return;
        }
        stored.shapeReached = fact.shapeReached;
        stored.shapeMatched = fact.shapeMatched;
        stored.callbackReached = fact.callbackReached;
        stored.callbackModeKnown = fact.callbackModeKnown;
        stored.callbackMode = fact.callbackMode;
        stored.failedKnown = fact.failedKnown;
        stored.failed = fact.failed;
        stored.detailsFinalized = true;
        return;
    }
    g_wasOverflowed = true;
}

void completeWitchspaceStarsFact(Token token, const PredicateFact& fact) noexcept {
    if (!validToken(token)) { rejectInvalidToken(); return; }
    DrawRecord& record = g_records[token.drawIndex];
    if (record.finalized || fact.kind != PredicateFactKind::WitchspaceStarsSkip ||
        !validWitchspaceFact(fact) || !fact.detailsFinalized) {
        rejectInvalidToken(); return;
    }
    for (std::uint8_t i = 0; i < record.predicateFactCount; ++i) {
        PredicateFact& stored = record.predicateFacts[i];
        if (stored.siteId != 6 || stored.kind != PredicateFactKind::WitchspaceStarsSkip)
            continue;
        if (stored.detailsFinalized ||
            stored.legacyInterestMask != fact.legacyInterestMask ||
            (stored.legacyInterestMask & (1ull << 1)) == 0) {
            g_wasOverflowed = true;
            return;
        }
        stored = fact;
        return;
    }
    g_wasOverflowed = true;
}

void finishDraw(Token token, std::int16_t winnerSiteId,
                std::int16_t verdictOrdinal) noexcept {
    if (!validToken(token)) { rejectInvalidToken(); return; }
    DrawRecord& record = g_records[token.drawIndex];
    if (record.finalized) { rejectInvalidToken(); return; }
    for (std::uint16_t i = 0; i < record.siteCount; ++i) {
        const std::uint16_t siteId = record.sites[i].id;
        if (siteId != 3 && siteId != 6 && siteId != 24 && siteId != 26 &&
            siteId != 49 && siteId != 50 && siteId != 53 && siteId != 55) continue;
        bool found = false;
        for (std::uint8_t j = 0; j < record.predicateFactCount; ++j) {
            if (record.predicateFacts[j].siteId == siteId) {
                found = true;
                if ((siteId == 6 || siteId == 24 || siteId == 26 || siteId == 50 ||
                     siteId == 53 || siteId == 55) &&
                    !record.predicateFacts[j].detailsFinalized)
                    g_wasOverflowed = true;
                if (siteId == 53 || siteId == 55) {
                    const SiteEvent& event = record.sites[i];
                    const PredicateFact& fact = record.predicateFacts[j];
                    const auto result = siteId == 53 ? fact.holo.predicateResult
                                                     : fact.scrim.predicateResult;
                    const bool claimed = result == holo_scrim_observation::Tri::Yes;
                    const std::int16_t expectedVerdict = siteId == 53 ? 4 : 17;
                    if (event.kind != static_cast<std::uint8_t>(draw_ladder::SiteKind::Claim) ||
                        event.subsite != 0 ||
                        event.outcome != static_cast<std::uint8_t>(claimed
                            ? draw_ladder::SiteOutcome::Claimed
                            : draw_ladder::SiteOutcome::Declined) ||
                        event.flow != static_cast<std::uint8_t>(claimed
                            ? draw_ladder::Flow::Stop
                            : draw_ladder::Flow::Continue) ||
                        event.verdict != (claimed ? expectedVerdict : -1) ||
                        (claimed && (winnerSiteId != static_cast<std::int16_t>(siteId) ||
                                     verdictOrdinal != expectedVerdict)))
                        g_wasOverflowed = true;
                }
            }
        }
        if (!found) g_wasOverflowed = true;
    }
    for (std::uint8_t i = 0; i < record.predicateFactCount; ++i) {
        bool visited = false;
        for (std::uint16_t j = 0; j < record.siteCount; ++j) {
            if (record.sites[j].id == record.predicateFacts[i].siteId) visited = true;
        }
        if (!visited) g_wasOverflowed = true;
    }
    record.winnerSiteId = winnerSiteId;
    record.verdictOrdinal = verdictOrdinal;
    record.finalized = true;
}

void frameEnd(std::uint32_t completedFrameNo) noexcept {
    if (!g_isCapturing) return;
    detail::g_captureActive.store(false, std::memory_order_relaxed);
    g_isCapturing = false;
    if (completedFrameNo != g_frame.frameNo) g_wasOverflowed = true;
    for (std::uint32_t i = 0; i < g_drawCount; ++i) {
        if (!g_records[i].finalized) g_wasOverflowed = true;
    }

    wchar_t sidecar[kPathChars]{};
    wchar_t leaf[kNameChars]{};
    _snwprintf_s(leaf, kNameChars, _TRUNCATE, L"%s.draw-ladder-%u.json",
                 g_logStem, completedFrameNo);
    if (FAILED(StringCchCopyW(sidecar, kPathChars, g_directory)) ||
        FAILED(StringCchCatW(sidecar, kPathChars, L"\\")) ||
        FAILED(StringCchCatW(sidecar, kPathChars, leaf))) {
        g_lastWriteSucceeded = false;
    } else {
        HANDLE file = CreateFileW(sidecar, GENERIC_WRITE, FILE_SHARE_READ,
                                  nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL,
                                  nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            g_lastWriteSucceeded = false;
        } else {
            Writer writer(file);
            g_lastWriteSucceeded = writeTrace(writer, completedFrameNo) &&
                                   flush(writer);
            if (!CloseHandle(file)) g_lastWriteSucceeded = false;
            if (!g_lastWriteSucceeded) DeleteFileW(sidecar);
        }
    }
    if (!g_lastWriteSucceeded) {
        g_status.store(Status::WriteFailed, std::memory_order_relaxed);
    } else if (g_wasOverflowed) {
        g_status.store(Status::InvalidCapture, std::memory_order_relaxed);
    } else {
        g_status.store(Status::CompleteWritten, std::memory_order_relaxed);
    }
    g_drawCount = 0;
    g_identityCount = 0;
    ++g_generation;
    if (g_generation == 0) ++g_generation;
}

void invalidateActiveCapture() noexcept {
    if (g_isCapturing) g_wasOverflowed = true;
}

bool configured() noexcept { return g_enabled.load(std::memory_order_acquire); }
bool capturing() noexcept {
    return detail::g_captureActive.load(std::memory_order_relaxed);
}
bool overflowed() noexcept { return g_wasOverflowed; }
Status status() noexcept {
    if (detail::g_captureActive.load(std::memory_order_relaxed))
        return Status::Capturing;
    if (g_armPending.load(std::memory_order_relaxed)) return Status::Armed;
    return g_status.load(std::memory_order_relaxed);
}
const char* statusName(Status value) noexcept {
    switch (value) {
    case Status::Disabled: return "disabled";
    case Status::PathInvalid: return "path-invalid";
    case Status::AllocationFailed: return "allocation-failed";
    case Status::Ready: return "ready";
    case Status::Armed: return "armed";
    case Status::Capturing: return "capturing";
    case Status::CompleteWritten: return "complete-written";
    case Status::InvalidCapture: return "invalid-capture";
    case Status::WriteFailed: return "write-failed";
    }
    return "unknown";
}

}  // namespace edvr::draw_ladder_trace
