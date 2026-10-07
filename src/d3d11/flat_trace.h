// Frame-contract trace: serialize the reducer's input stream (FlatRuntimeDraw
// events) per frame, so a diagnostic replay runs the SAME reducer
// (flatRuntimeObserve) over identical inputs and compares the produced
// FlatFrameContract's hash. Gate 1 of the staged program in
// docs/design-flat-temporal-aa-2026-09-23.md: consolidation without changing
// output. Pointers in the trace are opaque identity tokens, replayed
// verbatim; only per-draw-alias pointers (camera bytes, projection shadow
// bytes) are reduced to flags.
#pragma once
#include "flat_frame_contract.h"

namespace edvr {

// Event kinds. Draws carry a full FlatRuntimeDraw; the prefix-mutating paths
// that run BETWEEN draws (resource writes, dispatch UAV guards, uncertain
// markers) record as their own kinds so a replay applies every mutation in
// sequence order. Anything less diverges on the first dispatch-heavy frame.
constexpr uint32_t kFlatTraceEventDraw = 0;
constexpr uint32_t kFlatTraceEventWriteResource = 1;    // key.color = resource
constexpr uint32_t kFlatTraceEventDispatchWritten = 2;  // key.color = UAV resource
constexpr uint32_t kFlatTraceEventMarkUncertain = 3;
// Camera captures share prefix.sequence with draws (capture() in
// flat_runtime.cpp); each one advances the counter, so it must interleave
// in the trace or every later draw's q is low by the capture count.
constexpr uint32_t kFlatTraceEventCameraCapture = 4;
// EDVRFTR4 (the HDR route, section 81): where the route resolved. key.color = the HDR target, key.vs/ps the
// consumer's pair, key.sequence the consumer's place in the prefix (the model's q), key.count the route's
// verdict (a FlatMonoReason value). A replay skips it: the reducer never sees it. The rig reads it to pin what
// the live run decided against what the pure detector decides over the same draws.
constexpr uint32_t kFlatTraceEventResolve = 5;
constexpr uint32_t kFlatTraceEventOverlayFailed = 6;
constexpr uint32_t kFlatTraceEventOverlaySeal = 7;

// The EDVRFTR3 event, byte for byte: the corpus files are still this layout and are read through it.
struct FlatTraceEventV3 {
    FlatContractObservation key{};   // camera and projection[].bytes are null
    unsigned char camera[kFlatCameraBytes]{};
    uint32_t instances = 1;
    uint32_t flags = 0;
    uint32_t kind = kFlatTraceEventDraw;
};
// EDVRFTR4 adds the four pixel-shader resources a candidate consumer of the HDR target binds (t0..t3), which the
// contract observation's own srv slots cannot carry: they are filled for tone and copy draws only, and filling them
// for every draw would change which draws coalesce into a record. `hdrSrv` is valid only with
// kFlatTraceHdrSrvKnown; an EDVRFTR3 event has none and the trigger rule then applies without its SRV test.
struct FlatTraceEvent {
    FlatContractObservation key{};   // camera and projection[].bytes are null
    unsigned char camera[kFlatCameraBytes]{};
    uint32_t instances = 1;
    uint32_t flags = 0;
    uint32_t kind = kFlatTraceEventDraw;
    // FTR5 uses the former x64 alignment padding for the draw's reducer q.
    // key.sequence remains the original observation used by contract hashing.
    uint32_t drawSequence = 0;
    const void* hdrSrv[4] = {};
};
static_assert(offsetof(FlatTraceEvent,drawSequence)==468 && offsetof(FlatTraceEvent,hdrSrv)==472 && sizeof(FlatTraceEvent)==504,
              "FTR5 must preserve the FTR4 event size and resource offsets");
constexpr uint32_t kFlatTraceHasCamera = 1u << 0;
constexpr uint32_t kFlatTraceSupported = 1u << 1;
constexpr uint32_t kFlatTraceHdrCopyVerified = 1u << 2;
constexpr uint32_t kFlatTraceMenuCopyVerified = 1u << 3;
constexpr uint32_t kFlatTraceImageSourceVerified = 1u << 4;
// The draw ctor observed foreign (non-owner) work this frame; the replay sets
// prefix.uncertain before the reducer sees the draw, as the ctor does.
constexpr uint32_t kFlatTraceForeignWork = 1u << 5;
// The event's hdrSrv slots were read (the runtime resolved t0..t3 for this draw).
constexpr uint32_t kFlatTraceHdrSrvKnown = 1u << 6;
constexpr uint32_t kFlatTraceOverlayProtected = 1u << 7;
constexpr uint32_t kFlatTraceDepthWrite = 1u << 8;
constexpr uint32_t kFlatTraceStencilWrite = 1u << 9;
// The weapon's two passes on the copy route (FlatRuntimeDraw::firstPersonCohort and alternateHdr, flat_runtime_model.h). Appended
// bits, zero in every committed trace, so the corpus replays as it always did.
constexpr uint32_t kFlatTraceFirstPersonCohort = 1u << 10;
constexpr uint32_t kFlatTraceAlternateHdr = 1u << 11;
// A pool family's vertex shader left stock, drawn into the scene's depth (FlatRuntimeDraw::poolFamilyVs, section 104). Appended the same way.
constexpr uint32_t kFlatTracePoolFamilyVs = 1u << 12;

inline FlatTraceEvent flatTraceEventFromDraw(const FlatRuntimeDraw& d, bool foreignWork) {
    FlatTraceEvent e{};
    e.key = d.key;
    e.flags |= d.key.camera ? kFlatTraceHasCamera : 0;
    e.key.camera = nullptr;
    for (uint32_t slot = 0; slot < kFlatProjectionSlots; ++slot)
        e.key.projection[slot].bytes = nullptr;
    std::memcpy(e.camera, d.camera, sizeof(e.camera));
    e.instances = d.instances;
    e.flags |= d.supported ? kFlatTraceSupported : 0;
    e.flags |= d.hdrCopyVerified ? kFlatTraceHdrCopyVerified : 0;
    e.flags |= d.menuHdrCopyVerified ? kFlatTraceMenuCopyVerified : 0;
    e.flags |= d.imageSourceCameraIndependentVerified ? kFlatTraceImageSourceVerified : 0;
    e.flags |= foreignWork ? kFlatTraceForeignWork : 0;
    e.flags |= d.overlayProtected ? kFlatTraceOverlayProtected : 0;
    e.flags |= d.effectiveDepthWrite ? kFlatTraceDepthWrite : 0;
    e.flags |= d.effectiveStencilWrite ? kFlatTraceStencilWrite : 0;
    e.flags |= d.firstPersonCohort ? kFlatTraceFirstPersonCohort : 0;
    e.flags |= d.alternateHdr ? kFlatTraceAlternateHdr : 0;
    e.flags |= d.poolFamilyVs ? kFlatTracePoolFamilyVs : 0;
    e.kind = kFlatTraceEventDraw;
    return e;
}
// The non-draw prefix mutations carry only their resource token (or nothing).
inline FlatTraceEvent flatTraceEventMarker(uint32_t kind, const void* resource) {
    FlatTraceEvent e{};
    e.kind = kind;
    e.key.color = resource;
    return e;
}
// The HDR route's resolve marker (kFlatTraceEventResolve).
inline FlatTraceEvent flatTraceEventResolve(const void* hdr, uint64_t vs, uint64_t ps, uint32_t sequence,
                                            uint32_t reason) {
    FlatTraceEvent e = flatTraceEventMarker(kFlatTraceEventResolve, hdr);
    e.key.vs = vs; e.key.ps = ps; e.key.sequence = sequence; e.key.count = reason;
    return e;
}
// The pixel-shader resources a candidate consumer binds: the runtime reads them only for the draws the trigger
// detector asked about (flatHdrCouldConsume), so most events carry none. Null srv means "not read".
inline void flatTraceEventSetSrv(FlatTraceEvent& e, const void* const* srv) {
    if (!srv) return;
    for (uint32_t i = 0; i < 4; ++i) e.hdrSrv[i] = srv[i];
    e.flags |= kFlatTraceHdrSrvKnown;
}
inline FlatRuntimeDraw flatTraceEventToDraw(const FlatTraceEvent& e) {
    FlatRuntimeDraw d{};
    d.key = e.key;
    std::memcpy(d.camera, e.camera, sizeof(d.camera));
    d.key.camera = (e.flags & kFlatTraceHasCamera) ? d.camera : nullptr;
    d.supported = (e.flags & kFlatTraceSupported) != 0;
    d.hdrCopyVerified = (e.flags & kFlatTraceHdrCopyVerified) != 0;
    d.menuHdrCopyVerified = (e.flags & kFlatTraceMenuCopyVerified) != 0;
    d.imageSourceCameraIndependentVerified = (e.flags & kFlatTraceImageSourceVerified) != 0;
    d.overlayProtected = (e.flags & kFlatTraceOverlayProtected) != 0;
    d.effectiveDepthWrite = (e.flags & kFlatTraceDepthWrite) != 0;
    d.effectiveStencilWrite = (e.flags & kFlatTraceStencilWrite) != 0;
    d.firstPersonCohort = (e.flags & kFlatTraceFirstPersonCohort) != 0;
    d.alternateHdr = (e.flags & kFlatTraceAlternateHdr) != 0;
    d.poolFamilyVs = (e.flags & kFlatTracePoolFamilyVs) != 0;
    d.instances = e.instances;
    return d;
}

struct FlatTraceHeader {
    char magic[8] = {'E','D','V','R','F','T','R','5'};
    uint32_t frameCount = 0;
    uint32_t reserved = 0;
};
struct FlatTraceFrameHeader {
    uint64_t frame = 0;
    const void* output = nullptr;   // identity token within the trace
    uint32_t width = 0, height = 0, format = 0;
    uint32_t eventCount = 0;        // events following this header
    uint32_t truncated = 0;         // the frame's draws exceeded the slot
    uint32_t produced = 0;          // a copy draw produced a contract
    uint64_t contractHash = 0;      // flatFrameContractHash of the produced contract
};

// The runtime's bounded ring: the last few complete frames, frame-atomic so a
// dump never holds half a frame. Fixed storage, no allocation on the draw
// path; a frame with more events than the slot marks itself truncated and is
// skipped by the dump.
constexpr uint32_t kFlatTraceFrames = 4;
constexpr uint32_t kFlatTraceEventsPerFrame = 65536;
constexpr uint64_t kFlatTraceStorageBudgetBytes = 256ull * 1024ull * 1024ull;
// Outside an armed capture the runtime keeps the pre-v0.18.2 window: every
// recorded event is a 504-byte store on the render thread, and a settlement
// frame has ~11k draws. The full window opens only while a capture reads it.
constexpr uint32_t kFlatTraceIdleEventsPerFrame = 4096;
struct FlatTraceRing {
    FlatTraceEvent events[kFlatTraceFrames][kFlatTraceEventsPerFrame];
    uint32_t eventLimit = kFlatTraceEventsPerFrame; // the runtime narrows it per frame
    FlatTraceFrameHeader headers[kFlatTraceFrames]{};
    uint32_t attempted[kFlatTraceFrames]{}; // live-only; never serialized
    uint32_t slot = 0;
    bool slotUsed[kFlatTraceFrames]{};
};
static_assert(sizeof(FlatTraceRing) <= kFlatTraceStorageBudgetBytes,
              "flat trace ring exceeds its fixed storage budget");

struct FlatTraceStats {
    uint32_t capacity = kFlatTraceEventsPerFrame;
    uint32_t peakAttempted = 0;
    uint32_t overflowSlots = 0;
};
inline FlatTraceStats flatTraceStats(const FlatTraceRing& r) {
    FlatTraceStats stats{};
    stats.capacity = r.eventLimit;
    for (uint32_t i = 0; i < kFlatTraceFrames; ++i) {
        if (!r.slotUsed[i]) continue;
        if (r.attempted[i] > stats.peakAttempted) stats.peakAttempted = r.attempted[i];
        if (r.headers[i].truncated) ++stats.overflowSlots;
    }
    return stats;
}
inline bool flatTraceCanAppend(FlatTraceRing& r) {
    if (!r.slotUsed[r.slot]) return false;
    auto& attempted = r.attempted[r.slot];
    if (attempted != 0xffffffffu) ++attempted;
    auto& h = r.headers[r.slot];
    if (h.eventCount >= r.eventLimit) { h.truncated = 1; return false; }
    return true;
}

inline void flatTraceBeginFrame(FlatTraceRing& r, uint64_t frame, const void* output,
                                uint32_t width, uint32_t height, uint32_t format) {
    r.slot = (r.slot + 1) % kFlatTraceFrames;
    auto& h = r.headers[r.slot];
    h = FlatTraceFrameHeader{};
    h.frame = frame; h.output = output; h.width = width; h.height = height; h.format = format;
    r.attempted[r.slot] = 0;
    r.slotUsed[r.slot] = true;
}
inline void flatTraceRecord(FlatTraceRing& r, const FlatRuntimeDraw& d, bool foreignWork,
                            const void* const* hdrSrv = nullptr, uint32_t drawSequence = 0) {
    if (!flatTraceCanAppend(r)) return;
    auto& h = r.headers[r.slot];
    auto& e = r.events[r.slot][h.eventCount++];
    e = flatTraceEventFromDraw(d, foreignWork);
    e.drawSequence = drawSequence ? drawSequence : d.key.sequence;
    flatTraceEventSetSrv(e, hdrSrv);
}
inline void flatTraceMark(FlatTraceRing& r, uint32_t kind, const void* resource) {
    if (!flatTraceCanAppend(r)) return;
    auto& h = r.headers[r.slot];
    r.events[r.slot][h.eventCount++] = flatTraceEventMarker(kind, resource);
}
// The HDR route's resolve marker, after the trigger draw's own event.
inline void flatTraceResolve(FlatTraceRing& r, const void* hdr, uint64_t vs, uint64_t ps, uint32_t sequence,
                             uint32_t reason) {
    if (!flatTraceCanAppend(r)) return;
    auto& h = r.headers[r.slot];
    r.events[r.slot][h.eventCount++] = flatTraceEventResolve(hdr, vs, ps, sequence, reason);
}
inline void flatTraceSeal(FlatTraceRing& r, bool produced, uint64_t contractHash) {
    if (!r.slotUsed[r.slot]) return;
    r.headers[r.slot].produced = produced ? 1u : 0u;
    r.headers[r.slot].contractHash = contractHash;
}

// Serialize complete frames oldest-first, skipping the current slot: its
// frame is mid-flight, unsealed and partial. Returns total bytes written.
template <class Write>
inline uint32_t flatTraceDump(const FlatTraceRing& r, Write&& write) {
    FlatTraceHeader header{};
    for (uint32_t i = 0; i < kFlatTraceFrames; ++i) {
        const uint32_t slot = (r.slot + 1 + i) % kFlatTraceFrames;
        const auto& h = r.headers[slot];
        if (slot != r.slot && r.slotUsed[slot] && !h.truncated && h.eventCount)
            ++header.frameCount;
    }
    uint32_t bytes = write(&header, sizeof(header));
    for (uint32_t i = 0; i < kFlatTraceFrames; ++i) {
        const uint32_t slot = (r.slot + 1 + i) % kFlatTraceFrames;
        if (slot == r.slot || !r.slotUsed[slot]) continue;
        const auto& h = r.headers[slot];
        if (h.truncated || !h.eventCount) continue;
        bytes += write(&h, sizeof(h));
        bytes += write(r.events[slot], h.eventCount * sizeof(FlatTraceEvent));
    }
    return bytes;
}

// An EDVRFTR3 event widened to the current layout: the new fields are zero, which is what "not read" is.
inline FlatTraceEvent flatTraceEventFromV3(const FlatTraceEventV3& v) {
    FlatTraceEvent e{};
    e.key = v.key;
    std::memcpy(e.camera, v.camera, sizeof(e.camera));
    e.instances = v.instances; e.flags = v.flags; e.kind = v.kind;
    return e;
}
// Parse one trace document, invoking onFrame(header) then onEvent(event)
// per event in order. FTR3/4 retain their original contract keys. FTR5 adds
// independent draw correlation; legacy alignment padding is never read as q.
template <class OnFrame, class OnEvent>
inline bool flatTraceParse(const unsigned char* data, size_t size,
                           OnFrame&& onFrame, OnEvent&& onEvent) {
    if (!data || size < sizeof(FlatTraceHeader)) return false;
    FlatTraceHeader header{};
    std::memcpy(&header, data, sizeof(header));
    const bool v4 = std::memcmp(header.magic, "EDVRFTR4", 8) == 0;
    const bool v5 = std::memcmp(header.magic, "EDVRFTR5", 8) == 0;
    if (!v4 && !v5 && std::memcmp(header.magic, "EDVRFTR3", 8) != 0) return false;
    const size_t eventSize = v4 || v5 ? sizeof(FlatTraceEvent) : sizeof(FlatTraceEventV3);
    size_t at = sizeof(FlatTraceHeader);
    for (uint32_t f = 0; f < header.frameCount; ++f) {
        if (size - at < sizeof(FlatTraceFrameHeader)) return false;
        FlatTraceFrameHeader fh{};
        std::memcpy(&fh, data + at, sizeof(fh));
        at += sizeof(fh);
        if (!fh.eventCount || fh.eventCount > kFlatTraceEventsPerFrame) return false;
        if (size - at < fh.eventCount * eventSize) return false;
        onFrame(fh);
        for (uint32_t i = 0; i < fh.eventCount; ++i) {
            if (v4 || v5) {
                FlatTraceEvent e{};
                std::memcpy(&e, data + at, sizeof(e));
                if(!v5)e.drawSequence=0;
                onEvent(e);
            } else {
                FlatTraceEventV3 v{};
                std::memcpy(&v, data + at, sizeof(v));
                onEvent(flatTraceEventFromV3(v));
            }
            at += eventSize;
        }
    }
    return at == size;
}
} // namespace edvr
