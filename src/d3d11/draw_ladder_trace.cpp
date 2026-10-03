#include "draw_ladder_trace.h"
#include "fss_observation.h"

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
#include <type_traits>

namespace edvr::draw_ladder_trace {
namespace {

constexpr std::uint32_t kIdentitySlots = kMaxDraws * 8;
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
    std::uint32_t sunglareFactIndices[kMaxSunglareFactsPerDraw]{};
    std::uint32_t fssFactIndices[kMaxFssFactsPerDraw]{};
    std::uint32_t remlokFactIndices[kMaxRemlokFactsPerDraw]{};
    std::uint32_t basicFactIndices[kMaxBasicFactsPerDraw]{};
    std::uint32_t eyeCensusFactIndices[kMaxEyeCensusFactsPerDraw]{};
    std::uint32_t resolveBindFactIndices[kMaxResolveBindFactsPerDraw]{};
    ForwardFacts forwardFacts{};
    std::uint16_t siteCount = 0;
    std::uint16_t actionCount = 0;
    std::uint8_t predicateFactCount = 0;
    std::uint8_t sunglareFactCount = 0;
    std::uint8_t fssFactCount = 0;
    std::uint8_t remlokFactCount = 0;
    std::uint8_t basicFactCount = 0;
    std::uint8_t eyeCensusFactCount = 0;
    std::uint8_t resolveBindFactCount = 0;
    std::int16_t winnerSiteId = -1;
    std::int16_t verdictOrdinal = -1;
    bool finalized = false;
    bool hasForwardFacts = false;
};

DrawRecord* g_records = nullptr;
SunglareObservation* g_sunglareFacts = nullptr;
std::uint32_t g_sunglareFactCount = 0;
FssObservation* g_fssFacts = nullptr;
std::uint32_t g_fssFactCount = 0;
remlok_observation::Observation* g_remlokFacts = nullptr;
std::uint32_t g_remlokFactCount = 0;
BasicDrawObservation* g_basicFacts = nullptr;
std::uint32_t g_basicFactCount = 0;
EyeCensusObservation* g_eyeCensusFacts = nullptr;
ResolveBindObservation* g_resolveBindFacts = nullptr;
std::uint32_t g_eyeCensusFactCount = 0;
std::uint32_t g_resolveBindFactCount = 0;
std::uintptr_t* g_identities = nullptr;
std::uint32_t g_drawCount = 0;
std::uint32_t g_identityCount = 0;
std::uint32_t g_generation = 1;
FrameFacts g_frame{};
std::atomic<bool> g_enabled{false};
std::atomic<Status> g_status{Status::Disabled};
bool g_isCapturing = false;
bool g_wasOverflowed = false;
bool g_sunglareIndexOverflowed = false;
bool g_sunglarePoolMissing = false;
bool g_fssIndexOverflowed = false;
bool g_fssPoolMissing = false;
bool g_basicIndexOverflowed = false;
bool g_basicPoolMissing = false;
bool g_eyeCensusIndexOverflowed = false;
bool g_eyeCensusPoolMissing = false;
bool g_resolveBindIndexOverflowed = false;
bool g_resolveBindPoolMissing = false;
bool g_remlokIndexOverflowed = false;
bool g_remlokPoolMissing = false;
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

template <class T>
bool writeSunglareReadValue(Writer& writer, const T& value) noexcept;

template <>
bool writeSunglareReadValue<bool>(Writer& writer, const bool& value) noexcept {
    return writeText(writer, value ? "true" : "false");
}

template <>
bool writeSunglareReadValue<std::uint32_t>(Writer& writer,
                                            const std::uint32_t& value) noexcept {
    return writeFmt(writer, "%u", value);
}

template <>
bool writeSunglareReadValue<std::uint64_t>(Writer& writer,
                                            const std::uint64_t& value) noexcept {
    return writeFmt(writer, "%llu", static_cast<unsigned long long>(value));
}

template <>
bool writeSunglareReadValue<std::int32_t>(Writer& writer,
                                           const std::int32_t& value) noexcept {
    return writeFmt(writer, "%d", value);
}

template <>
bool writeSunglareReadValue<SunglareTraceMode>(
    Writer& writer, const SunglareTraceMode& value) noexcept {
    return writeFmt(writer, "%u", static_cast<unsigned>(value));
}

template <>
bool writeSunglareReadValue<SunglareTraceAction>(
    Writer& writer, const SunglareTraceAction& value) noexcept {
    return writeFmt(writer, "%u", static_cast<unsigned>(value));
}

template <class T>
bool writeSunglareRead(Writer& writer, const char* name,
                       const SunglareRead<T>& read,
                       bool& first) noexcept {
    if (!first && !writeText(writer, ",")) return false;
    first = false;
    if (!writeFmt(writer, "\"%s\":{\"reached\":%s,\"known\":%s,\"value\":",
                  name, read.reached ? "true" : "false",
                  read.known ? "true" : "false")) return false;
    if (!read.reached || !read.known) return writeText(writer, "null}");
    return writeSunglareReadValue(writer, read.value) && writeText(writer, "}");
}

bool writeSunglareTexture(Writer& writer, const char* name,
                          const SunglareTextureRead& texture,
                          bool& first) noexcept {
    if (!first && !writeText(writer, ",")) return false;
    first = false;
    if (!writeFmt(writer, "\"%s\":{", name)) return false;
    bool fieldFirst = true;
    return writeSunglareRead(writer, "resolveOk", texture.resolveOk, fieldFirst) &&
        writeSunglareRead(writer, "isTexture2D", texture.isTexture2D, fieldFirst) &&
        writeSunglareRead(writer, "width", texture.width, fieldFirst) &&
        writeSunglareRead(writer, "height", texture.height, fieldFirst) &&
        writeSunglareRead(writer, "format", texture.format, fieldFirst) &&
        writeText(writer, "}");
}

bool writeSunglareSelector(Writer& writer,
                           const SunglareSelectorObservation& selector) noexcept {
    if (!writeText(writer, "{")) return false;
    bool first = true;
    // Each read is emitted through a tiny object wrapper so the nullable
    // reached/known contract stays identical for every consumed source.
    if (!writeSunglareRead(writer, "outerWantsMode", selector.outerWantsMode, first) ||
        !writeSunglareRead(writer, "outerExposureDamping", selector.outerExposureDamping, first) ||
        !writeSunglareRead(writer, "outerProbe", selector.outerProbe, first) ||
        !writeSunglareRead(writer, "outerTrainShape", selector.outerTrainShape, first) ||
        !writeSunglareRead(writer, "outerWantsResult", selector.outerWantsResult, first) ||
        !writeSunglareRead(writer, "helperWantsMode", selector.helperWantsMode, first) ||
        !writeSunglareRead(writer, "helperExposureDamping", selector.helperExposureDamping, first) ||
        !writeSunglareRead(writer, "helperProbe", selector.helperProbe, first) ||
        !writeSunglareRead(writer, "helperWantsResult", selector.helperWantsResult, first) ||
        !writeSunglareRead(writer, "helperTrainShape", selector.helperTrainShape, first) ||
        !writeSunglareTexture(writer, "ps0", selector.ps0, first) ||
        !writeSunglareTexture(writer, "ps1", selector.ps1, first) ||
        !writeSunglareRead(writer, "lastSeenBeforeMs", selector.lastSeenBeforeMs, first) ||
        !writeSunglareRead(writer, "nowMs", selector.nowMs, first) ||
        !writeSunglareRead(writer, "lastSeenAfterMs", selector.lastSeenAfterMs, first) ||
        !writeSunglareRead(writer, "actionMode", selector.actionMode, first) ||
        !writeSunglareRead(writer, "action", selector.action, first)) return false;
    return writeText(writer, "}");
}

bool writeSunglareSite(Writer& writer,
                       const SunglareSiteObservation& site) noexcept {
    if (!writeText(writer, "{")) return false;
    bool first = true;
    return writeSunglareRead(writer, "source61ActionNotStock", site.source61ActionNotStock, first) &&
        writeSunglareRead(writer, "worldValue", site.worldValue, first) &&
        writeSunglareRead(writer, "probeValue", site.probeValue, first) &&
        writeSunglareRead(writer, "clampBefore", site.clampBefore, first) &&
        writeSunglareRead(writer, "clampAfter", site.clampAfter, first) &&
        writeSunglareRead(writer, "billboardReached", site.billboardReached, first) &&
        writeText(writer, "}");
}

bool writeSunglareFact(Writer& writer,
                       const SunglareObservation& fact) noexcept {
    const unsigned kind = static_cast<unsigned>(fact.kind);
    const unsigned siteId = kind == 9 ? 61 : kind == 10 ? 62 : 63;
    if (!writeFmt(writer, "{\"siteId\":%u,\"kind\":%u,\"known\":\"yes\"",
                  siteId, kind)) return false;
    if (kind == 9) {
        if (!writeText(writer, ",\"selector\":") ||
            !writeSunglareSelector(writer, fact.selector) ||
            !writeText(writer, ",")) return false;
        bool first = true;
        if (!writeSunglareRead(writer, "common2ClampBefore", fact.common2ClampBefore, first) ||
            !writeSunglareRead(writer, "common2ClampAfter", fact.common2ClampAfter, first)) return false;
    }
    return writeText(writer, ",\"site\":") && writeSunglareSite(writer, fact.site) &&
           writeText(writer, "}");
}

template <class T>
bool writeFssValue(Writer& writer, const T& value) noexcept {
    if constexpr (std::is_same<T, bool>::value)
        return writeText(writer, value ? "true" : "false");
    else if constexpr (std::is_enum<T>::value)
        return writeFmt(writer, "%u", static_cast<unsigned>(value));
    else if constexpr (std::is_same<T, std::uint64_t>::value)
        return writeFmt(writer, "%llu", static_cast<unsigned long long>(value));
    else
        return writeFmt(writer, "%u", static_cast<unsigned>(value));
}

template <class T>
bool writeFssRead(Writer& writer, const char* name, const FssRead<T>& read,
                  bool& first) noexcept {
    if (!first && !writeText(writer, ",")) return false;
    first = false;
    if (!writeFmt(writer, "\"%s\":{\"reached\":%s,\"known\":%s,\"value\":",
                  name, read.reached ? "true" : "false",
                  read.known ? "true" : "false")) return false;
    if (!read.reached || !read.known) return writeText(writer, "null}");
    return writeFssValue(writer, read.value) && writeText(writer, "}");
}

bool writeFssHelper(Writer& writer, const FssHelperObservation& h,
                    bool panel) noexcept {
    if (!writeText(writer, "{")) return false;
    bool first = true;
    if (panel && !writeFssRead(writer, "enabled", h.enabled, first)) return false;
    if (!panel && (!writeFssRead(writer, "steady", h.steady, first) ||
                   !writeFssRead(writer, "lockstep", h.lockstep, first))) return false;
    return writeFssRead(writer, "contextNonNull", h.contextNonNull, first) &&
        writeFssRead(writer, "guardCallReached", h.guardCallReached, first) &&
        writeFssRead(writer, "callbackEntered", h.callbackEntered, first) &&
        writeFssRead(writer, "vsGetShaderCompleted", h.vsGetShaderCompleted, first) &&
        writeFssRead(writer, "shaderNonNull", h.shaderNonNull, first) &&
        writeFssRead(writer, "lookupReached", h.lookupReached, first) &&
        writeFssRead(writer, "lookupCompleted", h.lookupCompleted, first) &&
        writeFssRead(writer, "assignedHash", h.assignedHash, first) &&
        writeFssRead(writer, "releaseReached", h.releaseReached, first) &&
        writeFssRead(writer, "releaseCompleted", h.releaseCompleted, first) &&
        writeFssRead(writer, "callbackCompleted", h.callbackCompleted, first) &&
        writeFssRead(writer, "guardReturned", h.guardReturned, first) &&
        writeFssRead(writer, "hashAfterGuard", h.hashAfterGuard, first) &&
        writeText(writer, "}");
}

bool writeFssFact(Writer& writer, const FssObservation& fact) noexcept {
    const bool panel = fact.kind == FssTraceFactKind::kPanel;
    const unsigned siteId = panel ? 57u : 58u;
    const unsigned kind = panel ? 12u : 13u;
    if (!writeFmt(writer, "{\"siteId\":%u,\"kind\":%u,\"known\":\"yes\","
                  "\"handlerInvoked\":%s,\"rawProbeReached\":%s,\"selector\":{",
                  siteId, kind, fact.handlerInvoked ? "true" : "false",
                  fact.rawProbeReached ? "true" : "false")) return false;
    bool first = true;
    if (panel) {
        const auto& p = fact.panel;
        if (!writeFssRead(writer, "outerEnabled", p.outerEnabled, first) ||
            !writeFssRead(writer, "bodyFrame", p.bodyFrame, first) ||
            !writeFssRead(writer, "frameNo", p.frameNo, first)) return false;
        if (!writeText(writer, "},\"helper\":" ) || !writeFssHelper(writer, p.helper, true) ||
            !writeText(writer, ",\"mutation\":{")) return false;
        first = true;
        return writeFssRead(writer, "matchedHashBefore", p.matchedHashBefore, first) &&
            writeFssRead(writer, "matchedHashAfter", p.matchedHashAfter, first) &&
            writeText(writer, "}}");
    }
    const auto& p = fact.reveal;
    if (!writeFssRead(writer, "outerSteady", p.outerSteady, first) ||
        !writeFssRead(writer, "outerLockstep", p.outerLockstep, first) ||
        !writeFssRead(writer, "bodyFrame", p.bodyFrame, first) ||
        !writeFssRead(writer, "bodyFrameNo", p.bodyFrameNo, first) ||
        !writeFssRead(writer, "jumpFrame", p.jumpFrame, first) ||
        !writeFssRead(writer, "jumpFrameNo", p.jumpFrameNo, first) ||
        !writeFssRead(writer, "modeLatch", p.modeLatch, first) ||
        !writeText(writer, "},\"helper\":" ) || !writeFssHelper(writer, p.helper, false) ||
        !writeText(writer, ",\"mutation\":{")) return false;
    first = true;
    return writeFssRead(writer, "arrivalOpen", p.arrivalOpen, first) &&
        writeFssRead(writer, "arrivalBefore", p.arrivalBefore, first) &&
        writeFssRead(writer, "arrivalAfter", p.arrivalAfter, first) &&
        writeText(writer, "}}");
}

template <class T>
bool writeRemlokRead(Writer& writer, const char* name,
                     const remlok_observation::Read<T>& read, bool& first) noexcept {
    const FssRead<T> value{read.reached, read.known, read.value};
    return writeFssRead(writer, name, value, first);
}

bool writeRemlokFact(Writer& writer, const remlok_observation::Observation& fact) noexcept {
    if (!writeText(writer, "{\"siteId\":51,\"kind\":14,\"known\":\"yes\",\"selector\":{"))
        return false;
    bool first = true;
    if (!writeRemlokRead(writer, "outerMode", fact.selector.outerMode, first) ||
        !writeText(writer, "},\"helper\":{")) return false;
    first = true;
    const auto& h = fact.helper;
    if (!writeRemlokRead(writer, "modeBeforeGate", h.modeBeforeGate, first) ||
        !writeRemlokRead(writer, "dsvNonNull", h.dsvNonNull, first) ||
        !writeRemlokRead(writer, "resolved", h.resolved, first) ||
        !writeRemlokRead(writer, "isTexture2D", h.isTexture2D, first) ||
        !writeRemlokRead(writer, "width", h.width, first) ||
        !writeRemlokRead(writer, "height", h.height, first) ||
        !writeRemlokRead(writer, "hideMode", h.hideMode, first) ||
        !writeRemlokRead(writer, "swap", h.swap, first) ||
        !writeText(writer, "},\"mutation\":{")) return false;
    first = true;
    const auto& m = fact.mutation;
    return writeRemlokRead(writer, "matchesBefore", m.matchesBefore, first) &&
        writeRemlokRead(writer, "matchesAfter", m.matchesAfter, first) &&
        writeRemlokRead(writer, "hiddenBefore", m.hiddenBefore, first) &&
        writeRemlokRead(writer, "hiddenAfter", m.hiddenAfter, first) &&
        writeRemlokRead(writer, "pendingRightBefore", m.pendingRightBefore, first) &&
        writeRemlokRead(writer, "pendingRightAfter", m.pendingRightAfter, first) &&
        writeText(writer, "}}");
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
        std::uintptr_t values[8] = {
            f.vsIdentity, f.psIdentity, f.rtv0Identity, f.dsv0Identity,
            f.argumentBufferKnown ? f.argumentBufferIdentity : 0, 0, 0, 0};
        const DrawRecord& record = g_records[i];
        for (std::uint8_t j = 0; j < record.basicFactCount; ++j) {
            if (!g_basicFacts || record.basicFactIndices[j] >= g_basicFactCount) {
                g_wasOverflowed = true;
                continue;
            }
            const auto& fact = g_basicFacts[record.basicFactIndices[j]];
            if (fact.kind != BasicDrawFactKind::kContext) continue;
            if (fact.context.contextIdentity.reached && fact.context.contextIdentity.known)
                values[6] = fact.context.contextIdentity.value;
            if (fact.context.ownerContextIdentity.reached && fact.context.ownerContextIdentity.known)
                values[7] = fact.context.ownerContextIdentity.value;
        }
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

template <typename T>
bool writeBasicRead(Writer& writer, const char* name,
                    const BasicDrawRead<T>& read, bool& first) noexcept {
    return writeFssRead(writer, name, FssRead<T>{read.reached, read.known, read.value}, first);
}

bool writeBasicFact(Writer& writer, const BasicDrawObservation& fact) noexcept {
    if (!writeFmt(writer, "{\"siteId\":%u,\"kind\":%u,\"known\":\"yes\",\"context\":{",
                  fact.siteId, static_cast<unsigned>(fact.kind))) return false;
    bool first = true;
    auto context = fact.context;
    if (context.contextIdentity.known)
        context.contextIdentity.value = resourceOrdinal(context.contextIdentity.value);
    if (context.ownerContextIdentity.known)
        context.ownerContextIdentity.value = resourceOrdinal(context.ownerContextIdentity.value);
    if (!writeBasicRead(writer, "contextIdentity", context.contextIdentity, first) ||
        !writeBasicRead(writer, "ownerContextIdentity", context.ownerContextIdentity, first) ||
        !writeBasicRead(writer, "glareClampBefore", context.glareClampBefore, first) ||
        !writeBasicRead(writer, "glareClampAfter", context.glareClampAfter, first) ||
        !writeText(writer, "},\"distance\":{")) return false;
    first = true;
    return writeBasicRead(writer, "distanceEnabled", fact.distance.distanceEnabled, first) &&
           writeText(writer, "}}");
}

bool validEyeCensusFact(const EyeCensusObservation& fact) noexcept {
    if (fact.siteId != 48 || fact.kind != 17) return false;
    const auto validRead = [](const auto& read) { return !read.known || read.reached; };
    const auto validCount = [&](const auto& read) {
        return validRead(read) && (!read.known || read.value <= 8);
    };
    if (!validCount(fact.skipCountGate) || !validCount(fact.terminalLoopCount) ||
        !validRead(fact.censusSkippedBefore) || !validRead(fact.censusSkippedAfter))
        return false;
    for (const auto& rule : fact.rules) {
        if (!validCount(rule.loopCount) ||
            !validRead(rule.vsHashGate) ||
            !validRead(rule.heldVsHash) ||
            !validRead(rule.vsHashCompareExpected) ||
            !validRead(rule.ruleKind) ||
            !validRead(rule.countHighGate) ||
            !validRead(rule.countMinimum) ||
            !validRead(rule.countHighBound) ||
            !validRead(rule.exactCount)) return false;
        for (const auto& filter : rule.filters) {
            if (!validRead(filter.modeOffGate) ||
                !validRead(filter.modeAnyGate) ||
                !validRead(filter.boundNonNull) ||
                !validRead(filter.modeAfterBound) ||
                !validRead(filter.resolved) ||
                !validRead(filter.isTexture2D) ||
                !validRead(filter.width) ||
                !validRead(filter.height) ||
                !validRead(filter.modeAfterResolve) ||
                !validRead(filter.configuredWidth) ||
                !validRead(filter.configuredHeight) ||
                !validRead(filter.eyeSizeAvailable) ||
                !validRead(filter.eyeWidth) ||
                !validRead(filter.eyeHeight)) return false;
        }
    }
    return true;
}

template <typename T>
bool writeEyeCensusRead(Writer& writer, const char* name,
                       const EyeCensusRead<T>& read, bool& first) noexcept {
    return writeFssRead(writer, name, FssRead<T>{read.reached, read.known, read.value}, first);
}

bool writeEyeCensusFact(Writer& writer, const EyeCensusObservation& fact) noexcept {
    if (!writeFmt(writer, "{\"siteId\":%u,\"kind\":%u,\"known\":\"yes\",",
                  fact.siteId, static_cast<unsigned>(fact.kind))) return false;
    bool first = true;
    if (!writeEyeCensusRead(writer, "skipCountGate", fact.skipCountGate, first) ||
        !writeText(writer, ",\"rules\":[")) return false;
    for (unsigned i = 0; i < 8; ++i) {
        if (i && !writeText(writer, ",")) return false;
        if (!writeText(writer, "{")) return false;
        const auto& rule = fact.rules[i];
        first = true;
        if (!writeEyeCensusRead(writer, "loopCount", rule.loopCount, first) ||
            !writeEyeCensusRead(writer, "vsHashGate", rule.vsHashGate, first) ||
            !writeEyeCensusRead(writer, "heldVsHash", rule.heldVsHash, first) ||
            !writeEyeCensusRead(writer, "vsHashCompareExpected", rule.vsHashCompareExpected, first) ||
            !writeEyeCensusRead(writer, "ruleKind", rule.ruleKind, first) ||
            !writeEyeCensusRead(writer, "countHighGate", rule.countHighGate, first) ||
            !writeEyeCensusRead(writer, "countMinimum", rule.countMinimum, first) ||
            !writeEyeCensusRead(writer, "countHighBound", rule.countHighBound, first) ||
            !writeEyeCensusRead(writer, "exactCount", rule.exactCount, first) ||
            !writeText(writer, ",\"filters\":[")) return false;
        for (unsigned j = 0; j < 4; ++j) {
            if (j && !writeText(writer, ",")) return false;
            if (!writeText(writer, "{")) return false;
            const auto& filter = rule.filters[j];
            first = true;
            if (!writeEyeCensusRead(writer, "modeOffGate", filter.modeOffGate, first) ||
                !writeEyeCensusRead(writer, "modeAnyGate", filter.modeAnyGate, first) ||
                !writeEyeCensusRead(writer, "boundNonNull", filter.boundNonNull, first) ||
                !writeEyeCensusRead(writer, "modeAfterBound", filter.modeAfterBound, first) ||
                !writeEyeCensusRead(writer, "resolved", filter.resolved, first) ||
                !writeEyeCensusRead(writer, "isTexture2D", filter.isTexture2D, first) ||
                !writeEyeCensusRead(writer, "width", filter.width, first) ||
                !writeEyeCensusRead(writer, "height", filter.height, first) ||
                !writeEyeCensusRead(writer, "modeAfterResolve", filter.modeAfterResolve, first) ||
                !writeEyeCensusRead(writer, "configuredWidth", filter.configuredWidth, first) ||
                !writeEyeCensusRead(writer, "configuredHeight", filter.configuredHeight, first) ||
                !writeEyeCensusRead(writer, "eyeSizeAvailable", filter.eyeSizeAvailable, first) ||
                !writeEyeCensusRead(writer, "eyeWidth", filter.eyeWidth, first) ||
                !writeEyeCensusRead(writer, "eyeHeight", filter.eyeHeight, first) ||
                !writeText(writer, "}")) return false;
        }
        if (!writeText(writer, "]}")) return false;
    }
    if (!writeText(writer, "],")) return false;
    first = true;
    if (!writeEyeCensusRead(writer, "terminalLoopCount", fact.terminalLoopCount, first) ||
        !writeText(writer, ",\"mutation\":{")) return false;
    first = true;
    return writeEyeCensusRead(writer, "censusSkippedBefore", fact.censusSkippedBefore, first) &&
           writeEyeCensusRead(writer, "censusSkippedAfter", fact.censusSkippedAfter, first) &&
           writeText(writer, "}}");
}

bool validResolveBindFact(const ResolveBindObservation& fact) noexcept {
    if (fact.siteId != 60 || fact.kind != 18) return false;
    const auto validRead = [](const auto& read) { return !read.known || read.reached; };
    return validRead(fact.outer.wants) && validRead(fact.outer.psPresent) &&
           validRead(fact.outer.psHash) && validRead(fact.helper.wants) &&
           validRead(fact.helper.contextNonNull) && validRead(fact.helper.psPresent) &&
           validRead(fact.helper.psHash) && validRead(fact.helper.lambdaEntered) &&
           validRead(fact.helper.psGetReached) && validRead(fact.helper.psGetCompleted) &&
           validRead(fact.helper.shaderNonNull) && validRead(fact.helper.lookupReached) &&
           validRead(fact.helper.lookupCompleted) && validRead(fact.helper.lookupHash) &&
           validRead(fact.helper.cacheBeforePresent) && validRead(fact.helper.cacheBeforeHash) &&
           validRead(fact.helper.cacheAfterPresent) && validRead(fact.helper.cacheAfterHash) &&
           validRead(fact.helper.releaseReached) && validRead(fact.helper.releaseCompleted) &&
           validRead(fact.helper.guardReturned);
}

template <typename T>
bool writeResolveBindRead(Writer& writer, const char* name,
                          const ResolveBindRead<T>& read, bool& first) noexcept {
    return writeFssRead(writer, name, FssRead<T>{read.reached, read.known, read.value}, first);
}

bool writeResolveBindFact(Writer& writer, const ResolveBindObservation& fact) noexcept {
    if (!writeFmt(writer, "{\"siteId\":%u,\"kind\":%u,\"known\":\"yes\",\"outer\":{",
                  fact.siteId, static_cast<unsigned>(fact.kind))) return false;
    bool first = true;
    if (!writeResolveBindRead(writer, "wants", fact.outer.wants, first) ||
        !writeResolveBindRead(writer, "psPresent", fact.outer.psPresent, first) ||
        !writeResolveBindRead(writer, "psHash", fact.outer.psHash, first) ||
        !writeText(writer, "},\"helper\":{")) return false;
    first = true;
    return writeResolveBindRead(writer, "wants", fact.helper.wants, first) &&
           writeResolveBindRead(writer, "contextNonNull", fact.helper.contextNonNull, first) &&
           writeResolveBindRead(writer, "psPresent", fact.helper.psPresent, first) &&
           writeResolveBindRead(writer, "psHash", fact.helper.psHash, first) &&
           writeResolveBindRead(writer, "lambdaEntered", fact.helper.lambdaEntered, first) &&
           writeResolveBindRead(writer, "psGetReached", fact.helper.psGetReached, first) &&
           writeResolveBindRead(writer, "psGetCompleted", fact.helper.psGetCompleted, first) &&
           writeResolveBindRead(writer, "shaderNonNull", fact.helper.shaderNonNull, first) &&
           writeResolveBindRead(writer, "lookupReached", fact.helper.lookupReached, first) &&
           writeResolveBindRead(writer, "lookupCompleted", fact.helper.lookupCompleted, first) &&
           writeResolveBindRead(writer, "lookupHash", fact.helper.lookupHash, first) &&
           writeResolveBindRead(writer, "cacheBeforePresent", fact.helper.cacheBeforePresent, first) &&
           writeResolveBindRead(writer, "cacheBeforeHash", fact.helper.cacheBeforeHash, first) &&
           writeResolveBindRead(writer, "cacheAfterPresent", fact.helper.cacheAfterPresent, first) &&
           writeResolveBindRead(writer, "cacheAfterHash", fact.helper.cacheAfterHash, first) &&
           writeResolveBindRead(writer, "releaseReached", fact.helper.releaseReached, first) &&
           writeResolveBindRead(writer, "releaseCompleted", fact.helper.releaseCompleted, first) &&
           writeResolveBindRead(writer, "guardReturned", fact.helper.guardReturned, first) &&
           writeText(writer, "}}");
}

bool writeTrace(Writer& writer, std::uint32_t completedFrameNo) noexcept {
    normalizeResourceIdentities();
    const std::uint32_t stamp = moduleBuildStamp();
    bool ok = writeText(writer,
        "{\"format\":\"edvr.draw-ladder-trace\",\"schemaVersion\":2,"
        "\"predicateFactVersion\":11,"
        "\"buildVersion\":\"");
    ok = ok && writeText(writer, EDVR_VERSION_STRING);
    ok = ok && writeFmt(writer,
        "\",\"buildStamp\":\"%08X\",\"logFile\":\"%S\",\"semantics\":{",
        stamp, g_logFileName);
    ok = ok && writeText(writer,
        "\"equivalence\":\"observed-selector-and-action-order\","
        "\"predicateEquivalence\":false,"
        "\"predicateNote\":\"Predicate fact version 11 independently re-evaluates the supported gate, stars, Holo, Scrim, NV, RemLok, FSS, Sunglare, context, distance, eye-census and resolve-binding predicates from raw consumed source facts. Observed helper outputs are consistency checks; cached matches and SiteEvents are not selector inputs. Whole-ladder predicate equivalence is not established. No extra D3D queries or constant-buffer reads were performed.\","
        "\"identityNote\":\"Resource and context identities share per-capture ordinals; raw pointers are never serialized.\","
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
        if (r.sunglareFactCount && r.predicateFactCount &&
            !writeText(writer, ",")) return false;
        for (std::uint8_t j = 0; j < r.sunglareFactCount; ++j) {
            if (j && !writeText(writer, ",")) return false;
            if (!g_sunglareFacts ||
                r.sunglareFactIndices[j] >= g_sunglareFactCount ||
                !writeSunglareFact(writer,
                    g_sunglareFacts[r.sunglareFactIndices[j]])) return false;
        }
        if (r.fssFactCount &&
            (r.predicateFactCount || r.sunglareFactCount) &&
            !writeText(writer, ",")) return false;
        for (std::uint8_t j = 0; j < r.fssFactCount; ++j) {
            if (j && !writeText(writer, ",")) return false;
            if (!g_fssFacts || r.fssFactIndices[j] >= g_fssFactCount ||
                !writeFssFact(writer, g_fssFacts[r.fssFactIndices[j]])) return false;
        }
        if (r.remlokFactCount &&
            (r.predicateFactCount || r.sunglareFactCount || r.fssFactCount) &&
            !writeText(writer, ",")) return false;
        for (std::uint8_t j = 0; j < r.remlokFactCount; ++j) {
            if (j && !writeText(writer, ",")) return false;
            if (!g_remlokFacts || r.remlokFactIndices[j] >= g_remlokFactCount ||
                !writeRemlokFact(writer, g_remlokFacts[r.remlokFactIndices[j]])) return false;
        }
        if (r.basicFactCount &&
            (r.predicateFactCount || r.sunglareFactCount || r.fssFactCount || r.remlokFactCount) &&
            !writeText(writer, ",")) return false;
        for (std::uint8_t j = 0; j < r.basicFactCount; ++j) {
            if (j && !writeText(writer, ",")) return false;
            if (!g_basicFacts || r.basicFactIndices[j] >= g_basicFactCount ||
                !writeBasicFact(writer, g_basicFacts[r.basicFactIndices[j]])) return false;
        }
        if (r.eyeCensusFactCount && (r.predicateFactCount || r.sunglareFactCount ||
            r.fssFactCount || r.remlokFactCount || r.basicFactCount) &&
            !writeText(writer, ",")) return false;
        for (std::uint8_t j = 0; j < r.eyeCensusFactCount; ++j) {
            if (j && !writeText(writer, ",")) return false;
            if (!g_eyeCensusFacts || r.eyeCensusFactIndices[j] >= g_eyeCensusFactCount ||
                !writeEyeCensusFact(writer, g_eyeCensusFacts[r.eyeCensusFactIndices[j]])) return false;
        }
        if (r.resolveBindFactCount && (r.predicateFactCount || r.sunglareFactCount ||
            r.fssFactCount || r.remlokFactCount || r.basicFactCount || r.eyeCensusFactCount) &&
            !writeText(writer, ",")) return false;
        for (std::uint8_t j = 0; j < r.resolveBindFactCount; ++j) {
            if (j && !writeText(writer, ",")) return false;
            if (!g_resolveBindFacts || r.resolveBindFactIndices[j] >= g_resolveBindFactCount ||
                !writeResolveBindFact(writer, g_resolveBindFacts[r.resolveBindFactIndices[j]])) return false;
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
    if (!enabled && !wasEnabled && !g_records && !g_sunglareFacts &&
        !g_fssFacts && !g_remlokFacts && !g_basicFacts && !g_eyeCensusFacts && !g_resolveBindFacts && !g_identities) return;
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
    g_sunglareIndexOverflowed = false;
    g_sunglarePoolMissing = false;
    g_fssIndexOverflowed = false;
    g_fssPoolMissing = false;
    g_remlokIndexOverflowed = false;
    g_remlokPoolMissing = false;
    g_basicIndexOverflowed = false;
    g_basicPoolMissing = false;
    g_eyeCensusIndexOverflowed = false;
    g_resolveBindIndexOverflowed = false;
    g_eyeCensusPoolMissing = false;
    g_resolveBindPoolMissing = false;
    g_lastWriteSucceeded = false;
    if (g_records) { delete[] g_records; g_records = nullptr; }
    if (g_sunglareFacts) { delete[] g_sunglareFacts; g_sunglareFacts = nullptr; }
    if (g_fssFacts) { delete[] g_fssFacts; g_fssFacts = nullptr; }
    if (g_remlokFacts) { delete[] g_remlokFacts; g_remlokFacts = nullptr; }
    if (g_basicFacts) { delete[] g_basicFacts; g_basicFacts = nullptr; }
    if (g_eyeCensusFacts) { delete[] g_eyeCensusFacts; g_eyeCensusFacts = nullptr; }
    if (g_resolveBindFacts) { delete[] g_resolveBindFacts; g_resolveBindFacts = nullptr; }
    if (g_identities) { delete[] g_identities; g_identities = nullptr; }
    g_sunglareFactCount = 0;
    g_fssFactCount = 0;
    g_remlokFactCount = 0;
    g_basicFactCount = 0;
    g_eyeCensusFactCount = 0;
    g_resolveBindFactCount = 0;
    g_directory[0] = L'\0';
    g_logFileName[0] = L'\0';
    g_logStem[0] = L'\0';
    if (!enabled) return;
    if (!setLogPath(logFilePath)) return;

    // This is the only allocation site. It runs during cold initialization,
    // never from a hook or from an armed draw.
    g_records = new (std::nothrow) DrawRecord[kMaxDraws];
    g_sunglareFacts = new (std::nothrow) SunglareObservation[kMaxSunglareFacts];
    g_fssFacts = new (std::nothrow) FssObservation[kMaxFssFacts];
    g_remlokFacts = new (std::nothrow) remlok_observation::Observation[kMaxRemlokFacts];
    g_basicFacts = new (std::nothrow) BasicDrawObservation[kMaxBasicFacts];
    g_eyeCensusFacts = new (std::nothrow) EyeCensusObservation[kMaxEyeCensusFacts];
    g_resolveBindFacts = new (std::nothrow) ResolveBindObservation[kMaxResolveBindFacts];
    g_identities = new (std::nothrow) std::uintptr_t[kIdentitySlots];
    if (!g_records || !g_sunglareFacts || !g_fssFacts || !g_remlokFacts || !g_basicFacts ||
        !g_eyeCensusFacts || !g_resolveBindFacts || !g_identities) {
        if (g_records) { delete[] g_records; g_records = nullptr; }
        if (g_sunglareFacts) { delete[] g_sunglareFacts; g_sunglareFacts = nullptr; }
        if (g_fssFacts) { delete[] g_fssFacts; g_fssFacts = nullptr; }
        if (g_remlokFacts) { delete[] g_remlokFacts; g_remlokFacts = nullptr; }
        if (g_basicFacts) { delete[] g_basicFacts; g_basicFacts = nullptr; }
        if (g_eyeCensusFacts) { delete[] g_eyeCensusFacts; g_eyeCensusFacts = nullptr; }
        if (g_resolveBindFacts) { delete[] g_resolveBindFacts; g_resolveBindFacts = nullptr; }
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
    g_sunglareIndexOverflowed = false;
    g_sunglarePoolMissing = false;
    g_fssIndexOverflowed = false;
    g_fssPoolMissing = false;
    g_lastWriteSucceeded = false;
    g_drawCount = 0;
    g_identityCount = 0;
    g_sunglareFactCount = 0;
    g_fssFactCount = 0;
    g_frame = FrameFacts{};
    g_remlokIndexOverflowed = false;
    g_remlokPoolMissing = false;
    g_remlokFactCount = 0;
    g_basicIndexOverflowed = false;
    g_basicPoolMissing = false;
    g_basicFactCount = 0;
    g_eyeCensusIndexOverflowed = false;
    g_resolveBindIndexOverflowed = false;
    g_eyeCensusPoolMissing = false;
    g_resolveBindPoolMissing = false;
    g_eyeCensusFactCount = 0;
    g_resolveBindFactCount = 0;
    if (g_records) {
        delete[] g_records;
        g_records = nullptr;
    }
    if (g_sunglareFacts) {
        delete[] g_sunglareFacts;
        g_sunglareFacts = nullptr;
    }
    if (g_fssFacts) {
        delete[] g_fssFacts;
        g_fssFacts = nullptr;
    }
    if (g_remlokFacts) {
        delete[] g_remlokFacts;
        g_remlokFacts = nullptr;
    }
    if (g_basicFacts) {
        delete[] g_basicFacts;
        g_basicFacts = nullptr;
    }
    if (g_eyeCensusFacts) {
        delete[] g_eyeCensusFacts;
        g_eyeCensusFacts = nullptr;
    }
    if (g_resolveBindFacts) {
        delete[] g_resolveBindFacts;
        g_resolveBindFacts = nullptr;
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
    g_sunglareFactCount = 0;
    g_fssFactCount = 0;
    g_remlokFactCount = 0;
    g_wasOverflowed = false;
    g_basicFactCount = 0;
    g_basicIndexOverflowed = false;
    g_basicPoolMissing = false;
    g_eyeCensusFactCount = 0;
    g_resolveBindFactCount = 0;
    g_eyeCensusIndexOverflowed = false;
    g_resolveBindIndexOverflowed = false;
    g_eyeCensusPoolMissing = false;
    g_resolveBindPoolMissing = false;
    g_sunglareIndexOverflowed = false;
    g_sunglarePoolMissing = false;
    g_fssIndexOverflowed = false;
    g_fssPoolMissing = false;
    g_isCapturing = true;
    g_remlokIndexOverflowed = false;
    g_remlokPoolMissing = false;
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
        record.predicateFactCount + record.sunglareFactCount +
            record.fssFactCount + record.remlokFactCount + record.basicFactCount +
            record.eyeCensusFactCount + record.resolveBindFactCount >= kMaxTotalPredicateFactsPerDraw ||
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
    if (record.sunglareFactCount && !g_sunglareFacts) {
        g_sunglarePoolMissing = true;
        g_wasOverflowed = true;
        return;
    }
    for (std::uint8_t i = 0; i < record.sunglareFactCount; ++i) {
        const unsigned kind = static_cast<unsigned>(
            g_sunglareFacts[record.sunglareFactIndices[i]].kind);
        const std::uint16_t siteId = static_cast<std::uint16_t>(kind + 52);
        if (siteId == fact.siteId) {
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

void appendSunglareFact(Token token, const SunglareObservation& fact) noexcept {
    if (!validToken(token)) { rejectInvalidToken(); return; }
    DrawRecord& record = g_records[token.drawIndex];
    if (record.finalized) { rejectInvalidToken(); return; }
    if (!g_sunglareFacts) {
        g_sunglarePoolMissing = true;
        g_wasOverflowed = true;
        return;
    }
    if (g_sunglareFactCount >= kMaxSunglareFacts) {
        g_sunglareIndexOverflowed = true;
        g_wasOverflowed = true;
        return;
    }
    if (record.sunglareFactCount >= kMaxSunglareFactsPerDraw ||
        record.predicateFactCount + record.sunglareFactCount +
            record.fssFactCount + record.remlokFactCount + record.basicFactCount +
            record.eyeCensusFactCount + record.resolveBindFactCount >= kMaxTotalPredicateFactsPerDraw) {
        g_wasOverflowed = true;
        return;
    }
    const unsigned kind = static_cast<unsigned>(fact.kind);
    const std::uint16_t siteId = kind == 9 ? 61 : kind == 10 ? 62 : kind == 11 ? 63 : 0;
    if (!siteId) { g_wasOverflowed = true; return; }
    for (std::uint8_t i = 0; i < record.predicateFactCount; ++i) {
        if (record.predicateFacts[i].siteId == siteId) {
            g_wasOverflowed = true;
            return;
        }
    }
    for (std::uint8_t i = 0; i < record.sunglareFactCount; ++i) {
        const SunglareObservation& old = g_sunglareFacts[record.sunglareFactIndices[i]];
        if (old.kind == fact.kind) {
            g_wasOverflowed = true;
            return;
        }
    }
    if (kind == 9) {
        if (!fact.selector.action.reached || !fact.selector.action.known ||
            fact.site.source61ActionNotStock.reached ||
            !fact.common2ClampBefore.reached || !fact.common2ClampBefore.known ||
            !fact.common2ClampAfter.reached || !fact.common2ClampAfter.known) {
            g_wasOverflowed = true;
            return;
        }
        if (fact.selector.action.value == SunglareTraceAction::kClamp ||
            fact.common2ClampAfter.value != 0 ||
            !fact.site.clampBefore.reached || !fact.site.clampBefore.known ||
            fact.site.clampBefore.value != fact.common2ClampAfter.value ||
            !fact.site.clampAfter.reached || !fact.site.clampAfter.known ||
            fact.site.clampAfter.value != 0) {
            g_wasOverflowed = true;
            return;
        }
    } else {
        const SunglareObservation* source61 = nullptr;
        for (std::uint8_t i = 0; i < record.sunglareFactCount; ++i) {
            const SunglareObservation& prior = g_sunglareFacts[record.sunglareFactIndices[i]];
            if (prior.kind == SunglareTraceFactKind::kKind9) source61 = &prior;
        }
        if (!source61 || !source61->selector.action.reached ||
            !source61->selector.action.known ||
            !fact.site.source61ActionNotStock.reached ||
            !fact.site.source61ActionNotStock.known ||
            fact.site.source61ActionNotStock.value !=
                (source61->selector.action.value != SunglareTraceAction::kStock)) {
            g_wasOverflowed = true;
            return;
        }
    }
    const auto validRead = [](const auto& read) {
        return !read.known || read.reached;
    };
#define EDVR_VALID_SUN_READ(read) if (!validRead(read)) { g_wasOverflowed = true; return; }
    const SunglareSelectorObservation& s = fact.selector;
    EDVR_VALID_SUN_READ(s.outerWantsMode); EDVR_VALID_SUN_READ(s.outerExposureDamping);
    EDVR_VALID_SUN_READ(s.outerProbe); EDVR_VALID_SUN_READ(s.outerTrainShape);
    EDVR_VALID_SUN_READ(s.outerWantsResult); EDVR_VALID_SUN_READ(s.helperWantsMode);
    EDVR_VALID_SUN_READ(s.helperExposureDamping); EDVR_VALID_SUN_READ(s.helperProbe);
    EDVR_VALID_SUN_READ(s.helperWantsResult); EDVR_VALID_SUN_READ(s.helperTrainShape);
    EDVR_VALID_SUN_READ(s.ps0.resolveOk); EDVR_VALID_SUN_READ(s.ps0.isTexture2D);
    EDVR_VALID_SUN_READ(s.ps0.width); EDVR_VALID_SUN_READ(s.ps0.height); EDVR_VALID_SUN_READ(s.ps0.format);
    EDVR_VALID_SUN_READ(s.ps1.resolveOk); EDVR_VALID_SUN_READ(s.ps1.isTexture2D);
    EDVR_VALID_SUN_READ(s.ps1.width); EDVR_VALID_SUN_READ(s.ps1.height); EDVR_VALID_SUN_READ(s.ps1.format);
    EDVR_VALID_SUN_READ(s.lastSeenBeforeMs); EDVR_VALID_SUN_READ(s.nowMs);
    EDVR_VALID_SUN_READ(s.lastSeenAfterMs); EDVR_VALID_SUN_READ(s.actionMode);
    EDVR_VALID_SUN_READ(s.action); EDVR_VALID_SUN_READ(fact.common2ClampBefore);
    EDVR_VALID_SUN_READ(fact.common2ClampAfter); EDVR_VALID_SUN_READ(fact.site.source61ActionNotStock);
    EDVR_VALID_SUN_READ(fact.site.worldValue); EDVR_VALID_SUN_READ(fact.site.probeValue);
    EDVR_VALID_SUN_READ(fact.site.clampBefore); EDVR_VALID_SUN_READ(fact.site.clampAfter);
    EDVR_VALID_SUN_READ(fact.site.billboardReached);
#undef EDVR_VALID_SUN_READ
    if ((s.actionMode.known && static_cast<unsigned>(s.actionMode.value) > 3) ||
        (s.outerWantsMode.known && static_cast<unsigned>(s.outerWantsMode.value) > 3) ||
        (s.helperWantsMode.known && static_cast<unsigned>(s.helperWantsMode.value) > 3) ||
        (s.action.known && static_cast<unsigned>(s.action.value) > 3)) {
        g_wasOverflowed = true;
        return;
    }
    g_sunglareFacts[g_sunglareFactCount] = fact;
    record.sunglareFactIndices[record.sunglareFactCount++] = g_sunglareFactCount++;
}

void appendFssFact(Token token, const FssObservation& fact) noexcept {
    if (!validToken(token)) { rejectInvalidToken(); return; }
    DrawRecord& record = g_records[token.drawIndex];
    if (record.finalized) { rejectInvalidToken(); return; }
    if (!g_fssFacts) { g_fssPoolMissing = true; g_wasOverflowed = true; return; }
    if (g_fssFactCount >= kMaxFssFacts) {
        g_fssIndexOverflowed = true;
        g_wasOverflowed = true;
        return;
    }
    if (record.fssFactCount >= kMaxFssFactsPerDraw ||
        record.predicateFactCount + record.sunglareFactCount +
            record.fssFactCount + record.remlokFactCount + record.basicFactCount +
            record.eyeCensusFactCount + record.resolveBindFactCount >= kMaxTotalPredicateFactsPerDraw) {
        g_wasOverflowed = true;
        return;
    }
    const std::uint16_t siteId = fact.kind == FssTraceFactKind::kPanel ? 57 :
                                 fact.kind == FssTraceFactKind::kReveal ? 58 : 0;
    if (!siteId) { g_wasOverflowed = true; return; }
    if (fact.handlerInvoked == fact.rawProbeReached) { g_wasOverflowed = true; return; }
    if (fact.rawProbeReached) {
        const auto touched = [](const auto& read) { return read.reached || read.known; };
        const auto& h = fact.kind == FssTraceFactKind::kPanel ? fact.panel.helper : fact.reveal.helper;
        if (touched(h.enabled) || touched(h.steady) || touched(h.lockstep) ||
            touched(h.contextNonNull) || touched(h.guardCallReached) || touched(h.callbackEntered) ||
            touched(h.vsGetShaderCompleted) || touched(h.shaderNonNull) || touched(h.lookupReached) ||
            touched(h.lookupCompleted) || touched(h.assignedHash) || touched(h.releaseReached) ||
            touched(h.releaseCompleted) || touched(h.callbackCompleted) || touched(h.guardReturned) ||
            touched(h.hashAfterGuard) ||
            (fact.kind == FssTraceFactKind::kPanel &&
             (touched(fact.panel.matchedHashBefore) || touched(fact.panel.matchedHashAfter))) ||
            (fact.kind == FssTraceFactKind::kReveal &&
             (touched(fact.reveal.arrivalOpen) || touched(fact.reveal.arrivalBefore) ||
              touched(fact.reveal.arrivalAfter)))) {
            g_wasOverflowed = true;
            return;
        }
    }
    for (std::uint8_t i = 0; i < record.predicateFactCount; ++i)
        if (record.predicateFacts[i].siteId == siteId) { g_wasOverflowed = true; return; }
    for (std::uint8_t i = 0; i < record.sunglareFactCount; ++i) {
        if (!g_sunglareFacts || record.sunglareFactIndices[i] >= g_sunglareFactCount) {
            g_wasOverflowed = true;
            return;
        }
        const auto kind = g_sunglareFacts[record.sunglareFactIndices[i]].kind;
        if (static_cast<unsigned>(kind) + 52 == siteId) {
            g_wasOverflowed = true;
            return;
        }
    }
    for (std::uint8_t i = 0; i < record.fssFactCount; ++i) {
        if (!g_fssFacts || record.fssFactIndices[i] >= g_fssFactCount) {
            g_wasOverflowed = true;
            return;
        }
        const auto oldKind = g_fssFacts[record.fssFactIndices[i]].kind;
        if ((oldKind == FssTraceFactKind::kPanel ? 57 : 58) == siteId) {
            g_wasOverflowed = true;
            return;
        }
    }
    const auto validRead = [](const auto& read) {
        return !read.known || read.reached;
    };
#define EDVR_VALID_FSS_READ(r) if (!validRead(r)) { g_wasOverflowed = true; return; }
    if (fact.kind == FssTraceFactKind::kPanel) {
        const auto& p = fact.panel;
        EDVR_VALID_FSS_READ(p.outerEnabled); EDVR_VALID_FSS_READ(p.bodyFrame);
        EDVR_VALID_FSS_READ(p.frameNo); EDVR_VALID_FSS_READ(p.helper.enabled);
        EDVR_VALID_FSS_READ(p.helper.contextNonNull); EDVR_VALID_FSS_READ(p.helper.guardCallReached);
        EDVR_VALID_FSS_READ(p.helper.callbackEntered); EDVR_VALID_FSS_READ(p.helper.vsGetShaderCompleted);
        EDVR_VALID_FSS_READ(p.helper.shaderNonNull); EDVR_VALID_FSS_READ(p.helper.lookupReached);
        EDVR_VALID_FSS_READ(p.helper.lookupCompleted); EDVR_VALID_FSS_READ(p.helper.assignedHash);
        EDVR_VALID_FSS_READ(p.helper.releaseReached); EDVR_VALID_FSS_READ(p.helper.releaseCompleted);
        EDVR_VALID_FSS_READ(p.helper.callbackCompleted); EDVR_VALID_FSS_READ(p.helper.guardReturned);
        EDVR_VALID_FSS_READ(p.helper.hashAfterGuard); EDVR_VALID_FSS_READ(p.matchedHashBefore);
        EDVR_VALID_FSS_READ(p.matchedHashAfter);
    } else {
        const auto& p = fact.reveal;
        EDVR_VALID_FSS_READ(p.outerSteady); EDVR_VALID_FSS_READ(p.outerLockstep);
        EDVR_VALID_FSS_READ(p.bodyFrame); EDVR_VALID_FSS_READ(p.bodyFrameNo);
        EDVR_VALID_FSS_READ(p.jumpFrame); EDVR_VALID_FSS_READ(p.jumpFrameNo);
        EDVR_VALID_FSS_READ(p.modeLatch); EDVR_VALID_FSS_READ(p.helper.steady);
        EDVR_VALID_FSS_READ(p.helper.lockstep); EDVR_VALID_FSS_READ(p.helper.contextNonNull);
        EDVR_VALID_FSS_READ(p.helper.guardCallReached); EDVR_VALID_FSS_READ(p.helper.callbackEntered);
        EDVR_VALID_FSS_READ(p.helper.vsGetShaderCompleted); EDVR_VALID_FSS_READ(p.helper.shaderNonNull);
        EDVR_VALID_FSS_READ(p.helper.lookupReached); EDVR_VALID_FSS_READ(p.helper.lookupCompleted);
        EDVR_VALID_FSS_READ(p.helper.assignedHash); EDVR_VALID_FSS_READ(p.helper.releaseReached);
        EDVR_VALID_FSS_READ(p.helper.releaseCompleted); EDVR_VALID_FSS_READ(p.helper.callbackCompleted);
        EDVR_VALID_FSS_READ(p.helper.guardReturned); EDVR_VALID_FSS_READ(p.helper.hashAfterGuard);
        EDVR_VALID_FSS_READ(p.arrivalOpen); EDVR_VALID_FSS_READ(p.arrivalBefore);
        EDVR_VALID_FSS_READ(p.arrivalAfter);
    }
#undef EDVR_VALID_FSS_READ
    g_fssFacts[g_fssFactCount] = fact;
    record.fssFactIndices[record.fssFactCount++] = g_fssFactCount++;
}

void appendRemlokFact(Token token, const remlok_observation::Observation& fact) noexcept {
    if (!validToken(token)) { rejectInvalidToken(); return; }
    DrawRecord& record = g_records[token.drawIndex];
    if (record.finalized) { rejectInvalidToken(); return; }
    if (!g_remlokFacts) { g_remlokPoolMissing = true; g_wasOverflowed = true; return; }
    if (g_remlokFactCount >= kMaxRemlokFacts) {
        g_remlokIndexOverflowed = true;
        g_wasOverflowed = true;
        return;
    }
    if (record.remlokFactCount >= kMaxRemlokFactsPerDraw ||
        record.predicateFactCount + record.sunglareFactCount + record.fssFactCount +
            record.remlokFactCount + record.basicFactCount + record.eyeCensusFactCount + record.resolveBindFactCount >= kMaxTotalPredicateFactsPerDraw) {
        g_wasOverflowed = true;
        return;
    }
    for (std::uint8_t i = 0; i < record.predicateFactCount; ++i)
        if (record.predicateFacts[i].siteId == 51) { g_wasOverflowed = true; return; }
    const auto validRead = [](const auto& read) { return !read.known || read.reached; };
#define EDVR_VALID_REMLOK_READ(r) if (!validRead(r)) { g_wasOverflowed = true; return; }
    EDVR_VALID_REMLOK_READ(fact.selector.outerMode);
    const auto& h = fact.helper;
    EDVR_VALID_REMLOK_READ(h.modeBeforeGate); EDVR_VALID_REMLOK_READ(h.dsvNonNull);
    EDVR_VALID_REMLOK_READ(h.resolved); EDVR_VALID_REMLOK_READ(h.isTexture2D);
    EDVR_VALID_REMLOK_READ(h.width); EDVR_VALID_REMLOK_READ(h.height);
    EDVR_VALID_REMLOK_READ(h.hideMode); EDVR_VALID_REMLOK_READ(h.swap);
    const auto& m = fact.mutation;
    EDVR_VALID_REMLOK_READ(m.matchesBefore); EDVR_VALID_REMLOK_READ(m.matchesAfter);
    EDVR_VALID_REMLOK_READ(m.hiddenBefore); EDVR_VALID_REMLOK_READ(m.hiddenAfter);
    EDVR_VALID_REMLOK_READ(m.pendingRightBefore); EDVR_VALID_REMLOK_READ(m.pendingRightAfter);
#undef EDVR_VALID_REMLOK_READ
    g_remlokFacts[g_remlokFactCount] = fact;
    record.remlokFactIndices[record.remlokFactCount++] = g_remlokFactCount++;
}

void appendBasicFact(Token token, const BasicDrawObservation& fact) noexcept {
    if (!validToken(token)) { rejectInvalidToken(); return; }
    DrawRecord& record = g_records[token.drawIndex];
    if (record.finalized) { rejectInvalidToken(); return; }
    if (!g_basicFacts) { g_basicPoolMissing = true; g_wasOverflowed = true; return; }
    if (g_basicFactCount >= kMaxBasicFacts) {
        g_basicIndexOverflowed = true;
        g_wasOverflowed = true;
        return;
    }
    if (record.basicFactCount >= kMaxBasicFactsPerDraw ||
        record.predicateFactCount + record.sunglareFactCount + record.fssFactCount +
            record.remlokFactCount + record.basicFactCount + record.eyeCensusFactCount + record.resolveBindFactCount >= kMaxTotalPredicateFactsPerDraw) {
        g_wasOverflowed = true;
        return;
    }
    const bool context = fact.kind == BasicDrawFactKind::kContext && fact.siteId == 2;
    const bool distance = fact.kind == BasicDrawFactKind::kDistance && fact.siteId == 67;
    if (!context && !distance) { g_wasOverflowed = true; return; }
    const auto validRead = [](const auto& read) { return !read.known || read.reached; };
    const auto touched = [](const auto& read) { return read.reached || read.known; };
    const auto& c = fact.context;
    const auto& d = fact.distance;
    if (!validRead(c.contextIdentity) || !validRead(c.ownerContextIdentity) ||
        !validRead(c.glareClampBefore) || !validRead(c.glareClampAfter) ||
        !validRead(d.distanceEnabled) ||
        (context && touched(d.distanceEnabled)) ||
        (distance && (touched(c.contextIdentity) || touched(c.ownerContextIdentity) ||
                      touched(c.glareClampBefore) || touched(c.glareClampAfter)))) {
        g_wasOverflowed = true;
        return;
    }
    for (std::uint8_t i = 0; i < record.basicFactCount; ++i) {
        if (!g_basicFacts || record.basicFactIndices[i] >= g_basicFactCount ||
            g_basicFacts[record.basicFactIndices[i]].siteId == fact.siteId) {
            g_wasOverflowed = true;
            return;
        }
    }
    for (std::uint8_t i = 0; i < record.predicateFactCount; ++i)
        if (record.predicateFacts[i].siteId == fact.siteId) { g_wasOverflowed = true; return; }
    g_basicFacts[g_basicFactCount] = fact;
    record.basicFactIndices[record.basicFactCount++] = g_basicFactCount++;
}

void appendEyeCensusFact(Token token, const EyeCensusObservation& fact) noexcept {
    if (!validToken(token)) { rejectInvalidToken(); return; }
    DrawRecord& record = g_records[token.drawIndex];
    if (record.finalized) { rejectInvalidToken(); return; }
    if (!g_eyeCensusFacts) {
        g_eyeCensusPoolMissing = true;
        g_wasOverflowed = true;
        return;
    }
    if (g_eyeCensusFactCount >= kMaxEyeCensusFacts) {
        g_eyeCensusIndexOverflowed = true;
        g_wasOverflowed = true;
        return;
    }
    if (record.eyeCensusFactCount >= kMaxEyeCensusFactsPerDraw ||
        record.predicateFactCount + record.sunglareFactCount + record.fssFactCount +
            record.remlokFactCount + record.basicFactCount + record.eyeCensusFactCount + record.resolveBindFactCount >=
            kMaxTotalPredicateFactsPerDraw || !validEyeCensusFact(fact)) {
        g_wasOverflowed = true;
        return;
    }
    for (std::uint8_t i = 0; i < record.predicateFactCount; ++i)
        if (record.predicateFacts[i].siteId == fact.siteId) { g_wasOverflowed = true; return; }
    g_eyeCensusFacts[g_eyeCensusFactCount] = fact;
    record.eyeCensusFactIndices[record.eyeCensusFactCount++] = g_eyeCensusFactCount++;
}

void appendResolveBindFact(Token token, const ResolveBindObservation& fact) noexcept {
    if (!validToken(token)) { rejectInvalidToken(); return; }
    DrawRecord& record = g_records[token.drawIndex];
    if (record.finalized) { rejectInvalidToken(); return; }
    if (!g_resolveBindFacts) {
        g_resolveBindPoolMissing = true;
        g_wasOverflowed = true;
        return;
    }
    if (g_resolveBindFactCount >= kMaxResolveBindFacts) {
        g_resolveBindIndexOverflowed = true;
        g_wasOverflowed = true;
        return;
    }
    if (record.resolveBindFactCount >= kMaxResolveBindFactsPerDraw ||
        record.predicateFactCount + record.sunglareFactCount + record.fssFactCount +
            record.remlokFactCount + record.basicFactCount + record.eyeCensusFactCount + record.resolveBindFactCount >=
            kMaxTotalPredicateFactsPerDraw || !validResolveBindFact(fact)) {
        g_wasOverflowed = true;
        return;
    }
    for (std::uint8_t i = 0; i < record.predicateFactCount; ++i)
        if (record.predicateFacts[i].siteId == fact.siteId) { g_wasOverflowed = true; return; }
    g_resolveBindFacts[g_resolveBindFactCount] = fact;
    record.resolveBindFactIndices[record.resolveBindFactCount++] = g_resolveBindFactCount++;
}

#if defined(EDVR_VSCREEN_PREDICATE_TEST)
std::uint8_t eyeCensusFactCountForTest(Token token) noexcept {
    return validToken(token) ? g_records[token.drawIndex].eyeCensusFactCount : 0;
}

bool readEyeCensusFactForTest(Token token, std::uint8_t ordinal, EyeCensusObservation* out) noexcept {
    if (!out || !validToken(token) || !g_eyeCensusFacts) return false;
    const DrawRecord& record = g_records[token.drawIndex];
    if (ordinal >= record.eyeCensusFactCount ||
        record.eyeCensusFactIndices[ordinal] >= g_eyeCensusFactCount) return false;
    *out = g_eyeCensusFacts[record.eyeCensusFactIndices[ordinal]];
    return true;
}

std::uint8_t resolveBindFactCountForTest(Token token) noexcept {
    return validToken(token) ? g_records[token.drawIndex].resolveBindFactCount : 0;
}

bool readResolveBindFactForTest(Token token, std::uint8_t ordinal, ResolveBindObservation* out) noexcept {
    if (!out || !validToken(token) || !g_resolveBindFacts) return false;
    const DrawRecord& record = g_records[token.drawIndex];
    if (ordinal >= record.resolveBindFactCount ||
        record.resolveBindFactIndices[ordinal] >= g_resolveBindFactCount) return false;
    *out = g_resolveBindFacts[record.resolveBindFactIndices[ordinal]];
    return true;
}

std::uint8_t basicFactCountForTest(Token token) noexcept {
    return validToken(token) ? g_records[token.drawIndex].basicFactCount : 0;
}

bool readBasicFactForTest(Token token, std::uint8_t ordinal, BasicDrawObservation* out) noexcept {
    if (!out || !validToken(token) || !g_basicFacts) return false;
    const DrawRecord& record = g_records[token.drawIndex];
    if (ordinal >= record.basicFactCount || record.basicFactIndices[ordinal] >= g_basicFactCount)
        return false;
    *out = g_basicFacts[record.basicFactIndices[ordinal]];
    return true;
}
#endif

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
        if (siteId == 2 || siteId == 67) {
            bool found = false;
            for (std::uint8_t j = 0; j < record.basicFactCount; ++j) {
                if (g_basicFacts && record.basicFactIndices[j] < g_basicFactCount &&
                    g_basicFacts[record.basicFactIndices[j]].siteId == siteId) found = true;
            }
            if (!found) g_wasOverflowed = true;
        }
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
    for (std::uint16_t i = 0; i < record.siteCount; ++i) {
        const std::uint16_t siteId = record.sites[i].id;
        if ((siteId == 51 || siteId == 52) && record.remlokFactCount != 1)
            g_wasOverflowed = true;
        if (siteId == 57 || siteId == 58) {
            if (!g_fssFacts) {
                g_fssPoolMissing = true;
                g_wasOverflowed = true;
            }
            bool found = false;
            for (std::uint8_t j = 0; j < record.fssFactCount; ++j) {
                if (g_fssFacts && record.fssFactIndices[j] < g_fssFactCount &&
                    static_cast<std::uint16_t>(
                        g_fssFacts[record.fssFactIndices[j]].kind == FssTraceFactKind::kPanel
                            ? 57 : 58) == siteId) found = true;
            }
            if (!found) g_wasOverflowed = true;
        }
        if (siteId < 61 || siteId > 63) continue;
        bool found = false;
        for (std::uint8_t j = 0; j < record.sunglareFactCount; ++j) {
            if (g_sunglareFacts &&
                record.sunglareFactIndices[j] < g_sunglareFactCount &&
                static_cast<unsigned>(g_sunglareFacts[
                    record.sunglareFactIndices[j]].kind) + 52 == siteId) {
                found = true;
                break;
            }
        }
        if (!found) g_wasOverflowed = true;
    }
    for (std::uint8_t i = 0; i < record.sunglareFactCount; ++i) {
        if (!g_sunglareFacts ||
            record.sunglareFactIndices[i] >= g_sunglareFactCount) {
            g_sunglarePoolMissing = !g_sunglareFacts;
            g_sunglareIndexOverflowed = g_sunglareIndexOverflowed ||
                                        !g_sunglarePoolMissing;
            g_wasOverflowed = true;
            continue;
        }
        const std::uint16_t siteId = static_cast<std::uint16_t>(
            static_cast<unsigned>(g_sunglareFacts[
                record.sunglareFactIndices[i]].kind) + 52);
        bool visited = false;
        for (std::uint16_t j = 0; j < record.siteCount; ++j)
            if (record.sites[j].id == siteId) visited = true;
        if (!visited) g_wasOverflowed = true;
    }
    for (std::uint8_t i = 0; i < record.fssFactCount; ++i) {
        if (!g_fssFacts || record.fssFactIndices[i] >= g_fssFactCount) {
            g_wasOverflowed = true;
            continue;
        }
        const FssObservation& fact = g_fssFacts[record.fssFactIndices[i]];
        const std::uint16_t siteId = fact.kind == FssTraceFactKind::kPanel ? 57 : 58;
        bool visited = false;
        for (std::uint16_t j = 0; j < record.siteCount; ++j)
            if (record.sites[j].id == siteId) {
                visited = true;
                const bool stagedOut = record.sites[j].outcome == static_cast<std::uint8_t>(
                    draw_ladder::SiteOutcome::NotEligible);
                if (fact.handlerInvoked == stagedOut || fact.rawProbeReached != stagedOut)
                    g_wasOverflowed = true;
            }
        if (!visited) g_wasOverflowed = true;
    }
    if (record.remlokFactCount) {
        bool visited = false;
        for (std::uint16_t j = 0; j < record.siteCount; ++j)
            if (record.sites[j].id == 51) visited = true;
        if (!visited) g_wasOverflowed = true;
        for (std::uint8_t i = 0; i < record.remlokFactCount; ++i)
            if (!g_remlokFacts || record.remlokFactIndices[i] >= g_remlokFactCount)
                g_wasOverflowed = true;
    }
    for (std::uint8_t i = 0; i < record.basicFactCount; ++i) {
        if (!g_basicFacts || record.basicFactIndices[i] >= g_basicFactCount) {
            g_basicPoolMissing = !g_basicFacts;
            g_basicIndexOverflowed = g_basicIndexOverflowed || !g_basicPoolMissing;
            g_wasOverflowed = true;
            continue;
        }
        const auto& fact = g_basicFacts[record.basicFactIndices[i]];
        bool visited = false;
        for (std::uint16_t j = 0; j < record.siteCount; ++j)
            if (record.sites[j].id == fact.siteId) visited = true;
        if (!visited) g_wasOverflowed = true;
        if (fact.kind != BasicDrawFactKind::kContext) continue;
        for (std::uint8_t j = 0; j < record.sunglareFactCount; ++j) {
            if (!g_sunglareFacts || record.sunglareFactIndices[j] >= g_sunglareFactCount) continue;
            const auto& sun = g_sunglareFacts[record.sunglareFactIndices[j]];
            if (sun.kind != SunglareTraceFactKind::kKind9) continue;
            if ((fact.context.glareClampBefore.known && sun.common2ClampBefore.known &&
                 fact.context.glareClampBefore.value != sun.common2ClampBefore.value) ||
                (fact.context.glareClampAfter.known && sun.common2ClampAfter.known &&
                 fact.context.glareClampAfter.value != sun.common2ClampAfter.value))
                g_wasOverflowed = true;
        }
    }
    bool eyeCensusVisited = false;
    for (std::uint16_t i = 0; i < record.siteCount; ++i)
        if (record.sites[i].id == 48) eyeCensusVisited = true;
    if (eyeCensusVisited && record.eyeCensusFactCount != 1) g_wasOverflowed = true;
    if (!eyeCensusVisited && record.eyeCensusFactCount) g_wasOverflowed = true;
    for (std::uint8_t i = 0; i < record.eyeCensusFactCount; ++i) {
        if (!g_eyeCensusFacts || record.eyeCensusFactIndices[i] >= g_eyeCensusFactCount) {
            g_eyeCensusPoolMissing = !g_eyeCensusFacts;
            g_eyeCensusIndexOverflowed = g_eyeCensusIndexOverflowed || !g_eyeCensusPoolMissing;
            g_wasOverflowed = true;
        }
    }
    bool resolveBindVisited = false;
    for (std::uint16_t i = 0; i < record.siteCount; ++i)
        if (record.sites[i].id == 60) resolveBindVisited = true;
    if (resolveBindVisited && record.resolveBindFactCount != 1) g_wasOverflowed = true;
    if (!resolveBindVisited && record.resolveBindFactCount) g_wasOverflowed = true;
    for (std::uint8_t i = 0; i < record.resolveBindFactCount; ++i) {
        if (!g_resolveBindFacts || record.resolveBindFactIndices[i] >= g_resolveBindFactCount) {
            g_resolveBindPoolMissing = !g_resolveBindFacts;
            g_resolveBindIndexOverflowed = g_resolveBindIndexOverflowed || !g_resolveBindPoolMissing;
            g_wasOverflowed = true;
        }
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
CaptureInvalidation invalidationReason() noexcept {
    if (g_resolveBindPoolMissing) return CaptureInvalidation::ResolveBindPoolMissing;
    if (g_resolveBindIndexOverflowed) return CaptureInvalidation::ResolveBindIndexOverflow;
    if (g_eyeCensusPoolMissing) return CaptureInvalidation::EyeCensusPoolMissing;
    if (g_eyeCensusIndexOverflowed) return CaptureInvalidation::EyeCensusIndexOverflow;
    if (g_basicPoolMissing) return CaptureInvalidation::BasicPoolMissing;
    if (g_basicIndexOverflowed) return CaptureInvalidation::BasicIndexOverflow;
    if (g_remlokPoolMissing) return CaptureInvalidation::RemlokPoolMissing;
    if (g_remlokIndexOverflowed) return CaptureInvalidation::RemlokIndexOverflow;
    if (g_fssPoolMissing) return CaptureInvalidation::FssPoolMissing;
    if (g_fssIndexOverflowed) return CaptureInvalidation::FssIndexOverflow;
    if (g_sunglarePoolMissing) return CaptureInvalidation::SunglarePoolMissing;
    if (g_sunglareIndexOverflowed) return CaptureInvalidation::SunglareIndexOverflow;
    return g_wasOverflowed ? CaptureInvalidation::Other : CaptureInvalidation::None;
}
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
