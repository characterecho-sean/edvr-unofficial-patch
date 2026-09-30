#pragma once
// CPU-only observation. None of these classifications decides freshness.
#include "ui_layer_math.h"
#include <array>
#include <cstdint>
#include <cstring>
#include <cstdio>

namespace edvr {
enum UiSeedReason : uint32_t {
    UiSeedOff = 0, UiSeedFirstObserved = 1, UiSeedNewFrame = 2,
    UiSeedSourceChanged = 4, UiSeedSizeChanged = 8, UiSeedStaleDraw = 16,
    UiSeedStaleClear = 32, UiSeedMissingDepth = 64, UiSeedMissingStencil = 128,
    UiSeedUnknownStale = 256
};
struct UiSeedPlan {
    uint32_t reason = 0;
    uint8_t mask = 0, passes = 0;
    bool depth = false, specified = false, stencil = false;
};
inline uint8_t uiSeedPlanes(const UiDsEffect& e) {
    return static_cast<uint8_t>((e.depthWrite ? 1 : 0) | (e.stencilWrite ? 2 : 0));
}
// Potentially reachable writes, not a promise of fragments or a depth-test
// result. Keep the production conservative predicate separately in the key.
inline uint8_t uiSeedReachablePlanes(const UiDsState& s, uint32_t count, uint32_t instances) {
    if (!count || !instances) return 0;
    const UiDsEffect e = uiLayerDsEffect(s, true);
    auto mayPass = [](uint8_t f) { return f != 1; }; // NEVER
    auto mayFail = [](uint8_t f) { return f != uids::kAlways; };
    bool depth = false, stencil = false;
    for (const UiDsFace* f : {&s.front, &s.back}) {
        const bool sp = !s.stencilEnable || !s.stencilPlane || mayPass(f->func);
        const bool sf = s.stencilEnable && s.stencilPlane && mayFail(f->func);
        const bool dp = !s.depthEnable || mayPass(s.depthFunc);
        const bool df = s.depthEnable && mayFail(s.depthFunc);
        depth = depth || (e.depthWrite && sp && dp);
        stencil = stencil || (e.stencilWrite &&
            ((sf && f->fail != uids::kKeep) ||
             (sp && df && f->depthFail != uids::kKeep) ||
             (sp && dp && f->pass != uids::kKeep)));
    }
    return static_cast<uint8_t>((depth ? 1 : 0) | (stencil ? 2 : 0));
}
struct UiSeedKey {
    uint64_t vs = 0, ps = 0;
    uint32_t count = 0, instances = 0, verdict = 0, flags = 0, value = 0, format = 0;
    uint32_t reason = 0;
    std::array<uint8_t, 18> ds{};
    uint8_t kind = 0, drawKind = 0, eye = 0, hdr = 0, planes = 0, reachable = 0, ref = 0;
    uint8_t forwarded = 0, substituted = 0, redirected = 0, outcomeKnown = 0;
    uint8_t mask = 0, depth = 0, specified = 0, passes = 0, success = 0;
    bool operator==(const UiSeedKey& b) const {
        return vs == b.vs && ps == b.ps && count == b.count && instances == b.instances &&
            verdict == b.verdict && flags == b.flags && value == b.value && format == b.format &&
            reason == b.reason && ds == b.ds && kind == b.kind && drawKind == b.drawKind && eye == b.eye && hdr == b.hdr &&
            planes == b.planes && reachable == b.reachable && ref == b.ref &&
            forwarded == b.forwarded && substituted == b.substituted && redirected == b.redirected &&
            outcomeKnown == b.outcomeKnown && mask == b.mask && depth == b.depth &&
            specified == b.specified && passes == b.passes && success == b.success;
    }
};
inline UiSeedKey uiSeedDrawKey(const UiDsState& s, uint32_t count, uint32_t instances,
                              uint32_t verdict, uint64_t vs, uint64_t ps, uint32_t ref,
                              uint32_t format, char drawKind = 0) {
    UiSeedKey k;
    k.kind = 1; k.count = count; k.instances = instances; k.verdict = verdict;
    k.drawKind = uint8_t(drawKind);
    k.vs = vs; k.ps = ps; k.ref = static_cast<uint8_t>(ref); k.format = format;
    k.ds = {{uint8_t(s.depthEnable), s.depthFunc, uint8_t(s.depthWriteAll),
        uint8_t(s.stencilEnable), s.readMask, s.writeMask,
        s.front.func, s.front.fail, s.front.depthFail, s.front.pass,
        s.back.func, s.back.fail, s.back.depthFail, s.back.pass,
        uint8_t(s.readOnlyDepth), uint8_t(s.readOnlyStencil), uint8_t(s.stencilPlane), 0}};
    k.planes = uiSeedPlanes(uiLayerDsEffect(s, true));
    k.reachable = uiSeedReachablePlanes(s, count, instances);
    k.flags = (s.readOnlyDepth ? 1u : 0u) | (s.readOnlyStencil ? 2u : 0u);
    return k;
}
inline UiSeedKey uiSeedClearKey(uint32_t flags, float depth, uint8_t stencil,
                               uint32_t viewFlags, uint32_t format, bool stencilPlane) {
    UiSeedKey k;
    k.kind = 2; k.flags = flags; std::memcpy(&k.value, &depth, sizeof(depth));
    k.ref = stencil; k.format = format;
    k.ds[14] = uint8_t((viewFlags & 1) != 0); k.ds[15] = uint8_t((viewFlags & 2) != 0);
    k.ds[16] = uint8_t(stencilPlane);
    k.planes = uint8_t((flags & 1) | ((flags & 2) && stencilPlane ? 2 : 0));
    k.reachable = uint8_t(k.planes & ~(viewFlags & 3));
    k.outcomeKnown = k.forwarded = 1;
    return k;
}
class UiSeedCensus {
public:
    static constexpr uint32_t kKeys = 16, kTimeline = 64;
    struct Aggregate { UiSeedKey key; uint64_t events = 0, invalidated = 0, afterStale = 0; };
    struct Event { UiSeedKey key; uint64_t seq = 0; bool invalidated = false; };
    struct Timeline {
        std::array<Event, kTimeline> events{};
        uint64_t seq = 0, firstObserved = 0;
        uint32_t n = 0, overflow = 0;
        bool active = false, complete = false;
    };
    struct Track {
        const void* source = nullptr;
        uint64_t seq = 0;
        uint32_t w = 0, h = 0, dirtyReason = 0;
        uint8_t dirtyPlanes = 0;
        bool known = false, stale = false;
    };
    std::array<Aggregate, kKeys> keys{};
    std::array<Timeline, 2> timelines{};
    std::array<std::array<Track, 2>, 2> tracks{};
    uint32_t n = 0;
    uint64_t overflow = 0, overflowInvalidated = 0, overflowAfterStale = 0, overflowSeeds = 0;
    uint64_t drawEvents = 0, clearEvents = 0, seedEvents = 0, toggles = 0;
    bool enabled = false, observedEnabled = false, observedDisabled = false;
private:
    struct Pending { Event event; uint32_t timelineIndex = kTimeline; };
    std::array<Pending, 4> pending_{};
    uint32_t pendingN_ = 0;
    void append(const Event& e, bool afterStale, bool captureTimeline = true) {
        for (uint32_t i = 0; i < n; ++i) if (keys[i].key == e.key) {
            ++keys[i].events; keys[i].invalidated += e.invalidated;
            keys[i].afterStale += afterStale; if (captureTimeline) timeline(e); return;
        }
        if (n < kKeys) { keys[n++] = {e.key, 1, uint64_t(e.invalidated), uint64_t(afterStale)}; }
        else {
            // Ordinary matching read-only draws must not crowd an actual
            // invalidator or seed out of the sixteen explicit key budget.
            uint32_t victim = kKeys;
            if (e.invalidated || e.key.planes || e.key.kind != 1) {
                for (uint32_t i = 0; i < n; ++i)
                    if (keys[i].key.kind == 1 && !keys[i].key.planes && !keys[i].invalidated) {
                        victim = i; break;
                    }
            }
            if (victim < kKeys) {
                overflow += keys[victim].events; overflowAfterStale += keys[victim].afterStale;
                keys[victim] = {e.key, 1, uint64_t(e.invalidated), uint64_t(afterStale)};
            } else {
                ++overflow; overflowInvalidated += e.invalidated;
                overflowAfterStale += afterStale; overflowSeeds += e.key.kind == 3;
            }
        }
        if (captureTimeline) timeline(e);
    }
    uint32_t timeline(const Event& e) {
        Timeline& t = timelines[e.key.eye];
        if (!t.active || t.seq != e.seq) return kTimeline;
        if (t.n < kTimeline) { const uint32_t i = t.n++; t.events[i] = e; return i; }
        ++t.overflow; return kTimeline;
    }
public:
    void configure(bool on) {
        if (on != enabled) {
            ++toggles; tracks = {}; pendingN_ = 0;
            for (Timeline& t : timelines) if (t.active) t.active = false;
        }
        enabled = on;
        observedEnabled = observedEnabled || on; observedDisabled = observedDisabled || !on;
    }
    void nextWindow() {
        keys = {}; n = 0;
        overflow = overflowInvalidated = overflowAfterStale = overflowSeeds = 0;
        drawEvents = clearEvents = seedEvents = toggles = 0;
        timelines = {}; pendingN_ = 0;
        observedEnabled = enabled; observedDisabled = !enabled;
    }
    bool matches(uint32_t eye, bool hdr, const void* source) const {
        const Track& t = tracks[eye][hdr ? 1 : 0];
        return enabled && t.known && source && t.source == source;
    }
    uint64_t trackedSeq(uint32_t eye, bool hdr) const { return tracks[eye][hdr ? 1 : 0].seq; }
    UiSeedPlan plan(uint32_t eye, bool hdr, uint64_t seq, const void* source,
                    uint32_t w, uint32_t h, bool stale, bool shortDepth, bool shortBits,
                    uint8_t mask, bool depth, bool specified, bool stencil) const {
        UiSeedPlan p; p.mask = mask; p.depth = depth; p.specified = specified; p.stencil = stencil;
        if (specified && stencil) p.passes = uint8_t(depth || mask);
        else { p.passes = uint8_t(depth); if (stencil) for (uint8_t b = mask; b; b &= uint8_t(b - 1)) ++p.passes; }
        const Track& t = tracks[eye][hdr ? 1 : 0];
        if (!t.known) p.reason |= UiSeedFirstObserved;
        else {
            if (t.seq != seq) p.reason |= UiSeedNewFrame;
            if (t.source != source) p.reason |= UiSeedSourceChanged;
            if (t.w != w || t.h != h) p.reason |= UiSeedSizeChanged;
            if (t.seq == seq && t.source == source) p.reason |= t.dirtyReason;
        }
        if (shortDepth) p.reason |= UiSeedMissingDepth;
        if (shortBits) p.reason |= UiSeedMissingStencil;
        if (stale && !p.reason) p.reason |= UiSeedUnknownStale;
        return p;
    }
    void seed(uint32_t eye, bool hdr, uint64_t seq, const void* source, uint32_t w, uint32_t h,
              const UiSeedPlan& p, bool success) {
        if (!enabled) return;
        ++seedEvents;
        Timeline& tl = timelines[eye];
        if (!tl.firstObserved) tl.firstObserved = seq;
        if (!tl.seq && seq != tl.firstObserved) { tl.seq = seq; tl.active = true; }
        UiSeedKey k; k.kind = 3; k.eye = uint8_t(eye); k.hdr = uint8_t(hdr);
        k.reason = p.reason; k.mask = p.mask; k.depth = uint8_t(p.depth);
        k.specified = uint8_t(p.specified); k.passes = p.passes; k.success = uint8_t(success);
        k.ds[16] = uint8_t(p.stencil);
        append({k, seq, false}, false);
        if (success) tracks[eye][hdr ? 1 : 0] = {source, seq, w, h, 0, 0, true, false};
    }
    void event(uint32_t eye, bool hdr, uint64_t seq, const UiSeedKey& input, bool invalidated) {
        if (!enabled) return;
        Track& t = tracks[eye][hdr ? 1 : 0];
        UiSeedKey k = input; k.eye = uint8_t(eye); k.hdr = uint8_t(hdr);
        // Dirty is the existing conservative predicate, including no-op or
        // unreachable writers. Preserve its exact stale reason for the seed.
        if (invalidated || (t.stale && k.planes)) {
            t.dirtyReason |= k.kind == 2 ? UiSeedStaleClear : UiSeedStaleDraw;
            t.dirtyPlanes |= k.planes;
        }
        const bool after = t.stale; t.stale = t.stale || invalidated;
        if (k.kind == 1) {
            ++drawEvents;
            if (pendingN_ < pending_.size()) {
                k.reason = uint32_t(after);
                const Event e{k, seq, invalidated};
                pending_[pendingN_++] = {e, timeline(e)};
            } else { ++overflow; overflowInvalidated += invalidated; overflowAfterStale += after; }
        } else { ++clearEvents; append({k, seq, invalidated}, after); }
    }
    void finishDraw(bool forwarded, bool substituted, bool redirected, bool known) {
        for (uint32_t i = 0; i < pendingN_; ++i) {
            Event e = pending_[i].event; const bool after = e.key.reason != 0; e.key.reason = 0;
            e.key.forwarded = uint8_t(forwarded); e.key.substituted = uint8_t(substituted);
            e.key.redirected = uint8_t(redirected); e.key.outcomeKnown = uint8_t(known);
            const uint32_t index = pending_[i].timelineIndex;
            Timeline& t = timelines[e.key.eye];
            if (index < kTimeline && t.seq == e.seq) t.events[index] = e;
            append(e, after, false);
        }
        pendingN_ = 0;
    }
    void door(uint32_t eye, uint64_t seq) {
        if (!enabled || eye > 1) return;
        Timeline& t = timelines[eye];
        if (t.active && t.seq == seq) { t.active = false; t.complete = true; }
    }
    template<class Emit> void report(Emit emit) const {
        char line[1200];
        std::snprintf(line, sizeof(line),
            "ui seed census: current=%u observed_on=%u observed_off=%u toggles=%llu "
            "draw_events=%llu clear_events=%llu seed_attempts=%llu keys=%u/16 overflow_events=%llu "
            "overflow_invalidated=%llu overflow_after_stale=%llu overflow_seed_attempts=%llu; "
            "CPU observation only, freshness unchanged; planes/reachable are state upper bounds, "
            "not pixel writes; draw count/instances name original arguments; outcomeKnown=0 "
            "means substitution command count unknown; redirected names successful private binding, not its decision; "
            "accepted HUD/write-back raw commands bypass this writer census.",
            unsigned(enabled), unsigned(observedEnabled), unsigned(observedDisabled),
            static_cast<unsigned long long>(toggles), static_cast<unsigned long long>(drawEvents),
            static_cast<unsigned long long>(clearEvents), static_cast<unsigned long long>(seedEvents),
            n, static_cast<unsigned long long>(overflow),
            static_cast<unsigned long long>(overflowInvalidated),
            static_cast<unsigned long long>(overflowAfterStale), static_cast<unsigned long long>(overflowSeeds));
        emit(line);
        auto detail = [&](const UiSeedKey& k, const char* prefix) {
            std::snprintf(line, sizeof(line),
                "%s kind=%u drawKind=%u eye=%u hdr=%u planes=%u reachable=%u vs=%016llX ps=%016llX "
                "verdict=%u count=%u instances=%u forwarded=%u substituted=%u redirected=%u outcomeKnown=%u "
                "format=%u flags=%u value_bits=%08X ref=%u "
                "DS=depth(%u,%u,%u) stencil(%u,%02X,%02X) front(%u,%u,%u,%u) back(%u,%u,%u,%u) readonly(%u,%u) plane=%u "
                "seed_reason=%u mask=%02X needsDepth=%u specified=%u passes=%u success=%u",
                prefix, k.kind, k.drawKind, k.eye, k.hdr, k.planes, k.reachable,
                static_cast<unsigned long long>(k.vs), static_cast<unsigned long long>(k.ps),
                k.verdict, k.count, k.instances, k.forwarded, k.substituted, k.redirected, k.outcomeKnown,
                k.format, k.flags, k.value, k.ref,
                k.ds[0], k.ds[1], k.ds[2], k.ds[3], k.ds[4], k.ds[5],
                k.ds[6], k.ds[7], k.ds[8], k.ds[9], k.ds[10], k.ds[11], k.ds[12], k.ds[13],
                k.ds[14], k.ds[15], k.ds[16], k.reason, k.mask, k.depth, k.specified, k.passes, k.success);
            emit(line);
        };
        char prefix[180];
        for (uint32_t i = 0; i < n; ++i) {
            const Aggregate& a = keys[i];
            std::snprintf(prefix, sizeof(prefix),
                "ui seed census key: id=%u events=%llu invalidated=%llu after_stale=%llu",
                i, static_cast<unsigned long long>(a.events),
                static_cast<unsigned long long>(a.invalidated), static_cast<unsigned long long>(a.afterStale));
            detail(a.key, prefix);
        }
        for (uint32_t eye = 0; eye < 2; ++eye) {
            const Timeline& t = timelines[eye];
            std::snprintf(line, sizeof(line),
                "ui seed census timeline: eye=%u seq=%llu complete_seed_to_door=%u door_seen=%u events=%u/64 overflow_events=%u; "
                "first observed sequence excluded (toggle/window may start mid-frame); one later seed-to-door capture per eye/window",
                eye, static_cast<unsigned long long>(t.seq), unsigned(t.complete && !t.overflow),
                unsigned(t.complete), t.n, t.overflow);
            emit(line);
            for (uint32_t i = 0; i < t.n; ++i) {
                const Event& e = t.events[i];
                std::snprintf(prefix, sizeof(prefix), "ui seed census event: eye=%u index=%u seq=%llu invalidated=%u",
                    eye, i, static_cast<unsigned long long>(e.seq), unsigned(e.invalidated));
                detail(e.key, prefix);
            }
        }
    }
};
} // namespace edvr
