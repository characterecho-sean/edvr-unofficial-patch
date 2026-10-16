#pragma once
// Built 2026-10-15: the anomaly-armed seal trace. Pure: no Log, no store, no game. The tick fills one TickRecord a frame and calls step(); the trace keeps
// the last kTraceRing records while the commander is docked, and when a trigger fires while the seal is held it hands the emitter a dump: a header, the
// records before the trigger (the trigger's own record last), the kTracePost records after it, and an end line. Silent unless it fires, so a healthy seal
// writes nothing. The triggers, evaluated only while held:
//   T1 the held cell kept stale this tick (its pose unreadable);
//   T2 the held cell's centre moved more than kTraceJumpM from the previous tick's (a logged origin shift does not excuse it);
//   T4 the own pad missing from kTraceAbsentPasses informative passes in a row.
// T3 (a cull or rig-veto count above a median of the held ticks) was removed: it fired on seal onset and on 10x per-frame cull swings with every other
// field steady (log edvr_gfx_20261015_165224). The culled and veto counts are still in every line.
// At most kTraceDumpsMax dumps a session, at least kTraceSpacingMs between two of them. The rig pins each trigger, the ring, the dump shape and the line bound.
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include "optimized_station_core.h"

namespace edvr::sealed {

constexpr size_t kTraceRing = 64;             // the records kept; a dump writes these before its trigger, the trigger's own record among them
constexpr size_t kTracePost = 64;             // the records a dump writes after its trigger
constexpr uint32_t kTraceDumpsMax = 4;        // dumps a session
constexpr uint64_t kTraceSpacingMs = 30000;   // at least this long between two dumps, measured between their triggers
constexpr float kTraceJumpM = 2.0f;           // T2: the held centre's move per tick
constexpr uint32_t kTraceAbsentPasses = 3;    // T4: informative passes in a row without the own pad

// The pass verdict of the tick, named for the line (station_pass_core.h's PassVerdict, mapped by the tick).
enum class TraceVerdict : uint8_t { kInformative = 0, kEmpty, kUnfinished };
// The held cell's refresh this tick: not held; rebuilt from the live pose and moved; rebuilt and unmoved; kept as it was because the pose was unreadable.
enum class HeldRefresh : uint8_t { kNotHeld = 0, kRefreshed, kKeptStale, kNoChange };
enum class TraceTrigger : uint8_t { kNone = 0, kStaleCell, kCellJump, kOwnAbsent };

inline const char* triggerName(TraceTrigger t) noexcept {
    switch (t) {
        case TraceTrigger::kStaleCell: return "T1";
        case TraceTrigger::kCellJump: return "T2";
        case TraceTrigger::kOwnAbsent: return "T4";
        case TraceTrigger::kNone: break;
    }
    return "-";
}
inline char verdictChar(TraceVerdict v) noexcept {
    return v == TraceVerdict::kInformative ? 'I' : v == TraceVerdict::kEmpty ? 'E' : 'U';
}
inline char refreshChar(HeldRefresh r) noexcept {
    return r == HeldRefresh::kRefreshed ? 'r' : r == HeldRefresh::kKeptStale ? 'k' : r == HeldRefresh::kNoChange ? 'c' : 'n';
}
// The release's short name for the line (the release text itself is in the log line the release writes).
inline const char* releaseTraceName(ReleaseWhy w) noexcept {
    switch (w) {
        case ReleaseWhy::kSettingOff: return "setting";
        case ReleaseWhy::kUndocked: return "undock";
        case ReleaseWhy::kCameraLeft: return "camera";
        case ReleaseWhy::kPadMoved: return "padmove";
        case ReleaseWhy::kHangarChanged: return "hangar";
        case ReleaseWhy::kLaunch: return "launch";
        case ReleaseWhy::kMechanism: return "mechanism";
        case ReleaseWhy::kInHangarLeft: return "r1";
        case ReleaseWhy::kOnFoot: return "onfoot";
        case ReleaseWhy::kOriginShift: return "origin";
        case ReleaseWhy::kNone: break;
    }
    return "none";
}

// One record per tick. The tick fills the fields without a comment on the derived side; step() fills the derived ones.
struct TickRecord {
    uint32_t frame = 0;
    uint64_t nowMs = 0;
    bool docked = false;
    bool held = false;
    TraceVerdict verdict = TraceVerdict::kEmpty;
    bool thin = false;               // informative, but under half the usual calls (the same rule as the 30 s passes line)
    uint64_t passCalls = 0;          // store callbacks the drained passes held
    bool ownSet = false;             // the sticky own pad exists
    bool ownInPass = false;          // its record was in this pass
    uint32_t ownUnseen = 0;          // passes since the own pad was last seen
    HeldRefresh refresh = HeldRefresh::kNotHeld;
    float centre[3] = {};            // the held cell's centre, world
    float frameShiftM = 0.0f;        // the held pad's move from the sealing centre
    bool camValid = false;
    float cam[3] = {};               // the frame's camera
    bool cameraOutside = false;      // outside the held box by more than kCameraLeaveM
    uint64_t rigVetoes = 0;          // the window's running totals (the trace takes the deltas)
    uint64_t producerVetoes = 0;
    uint64_t culled = 0;
    bool released = false;           // the seal was released this tick
    ReleaseWhy why = ReleaseWhy::kNone;
    bool originShift = false;        // a common-frame origin shift was read this tick
    bool holdStarted = false;        // the transition hold was set this tick
    // derived, by step()
    uint64_t ms = 0;                 // since the seal (held), else since this dock's trace started
    bool sealedNow = false;          // the seal was taken this tick
    float shiftM = 0.0f;             // the held centre's move from the previous tick's held centre
    float camJumpM = 0.0f;           // the camera's move from the previous tick
    uint64_t rigDelta = 0;           // this tick's rig vetoes, producer vetoes and culls (the window total's rise)
    uint64_t producerDelta = 0;
    uint64_t cullDelta = 0;
    TraceTrigger trigger = TraceTrigger::kNone;
};

// A metre (or a count-free float) for the line: finite and clamped to +-99999, so no value can widen the line; NaN prints as "nan".
inline int numText(char* out, size_t cap, float v, int decimals) noexcept {
    if (std::isnan(v)) return std::snprintf(out, cap, "nan");
    if (v > 99999.0f) v = 99999.0f;
    if (v < -99999.0f) v = -99999.0f;
    return decimals == 1 ? std::snprintf(out, cap, "%.1f", static_cast<double>(v)) : std::snprintf(out, cap, "%.2f", static_cast<double>(v));
}

// The event words of a line, joined by '+': seal, rel:<release>, origin, hold, and the trigger. "-" when there is none.
inline void eventText(const TickRecord& r, char* out, size_t cap) noexcept {
    out[0] = '\0';
    auto add = [&](const char* s) {
        const size_t used = std::strlen(out);
        if (used == 0) std::snprintf(out, cap, "%s", s);
        else if (used + 1 < cap) std::snprintf(out + used, cap - used, "+%s", s);
    };
    if (r.sealedNow) add("seal");
    if (r.released) {
        char rel[32];
        std::snprintf(rel, sizeof(rel), "rel:%s", releaseTraceName(r.why));
        add(rel);
    }
    if (r.originShift) add("origin");
    if (r.holdStarted) add("hold");
    if (r.trigger != TraceTrigger::kNone) add(triggerName(r.trigger));
    if (out[0] == '\0') std::snprintf(out, cap, "-");
}

// One tick as one line, keys short and in this order: f frame, t ms, h held, v verdict, th thin, pc pass calls, op own pad in pass, un own unseen,
// rf refresh, hc held centre, hs its shift, fs frame shift, cv camera valid, cj camera jump, co camera outside, rv rig vetoes, ov producer vetoes,
// cu culls (the three as per-tick deltas), ev the events. Under 300 characters with every integer at 20 digits.
inline int formatTickLine(const TickRecord& r, char* out, size_t cap) noexcept {
    char hx[16], hy[16], hz[16], hs[16], fs[16], cj[16], ev[64];
    numText(hx, sizeof(hx), r.centre[0], 1);
    numText(hy, sizeof(hy), r.centre[1], 1);
    numText(hz, sizeof(hz), r.centre[2], 1);
    numText(hs, sizeof(hs), r.shiftM, 2);
    numText(fs, sizeof(fs), r.frameShiftM, 2);
    numText(cj, sizeof(cj), r.camJumpM, 2);
    eventText(r, ev, sizeof(ev));
    return std::snprintf(out, cap,
                         "f=%u t=%llu h=%d v=%c th=%d pc=%llu op=%d un=%u rf=%c hc=%s,%s,%s hs=%s fs=%s cv=%d cj=%s co=%d rv=%llu ov=%llu cu=%llu ev=%s",
                         static_cast<unsigned>(r.frame), static_cast<unsigned long long>(r.ms), r.held ? 1 : 0, verdictChar(r.verdict), r.thin ? 1 : 0,
                         static_cast<unsigned long long>(r.passCalls), r.ownInPass ? 1 : 0, static_cast<unsigned>(r.ownUnseen), refreshChar(r.refresh),
                         hx, hy, hz, hs, fs, r.camValid ? 1 : 0, cj, r.cameraOutside ? 1 : 0, static_cast<unsigned long long>(r.rigDelta),
                         static_cast<unsigned long long>(r.producerDelta), static_cast<unsigned long long>(r.cullDelta), ev);
}

// The emitter: one line at a time (the tick passes its log writer; the rig passes a capture).
using TraceEmit = void (*)(void* ctx, const char* line);

struct SealTrace {
    TickRecord ring[kTraceRing];
    size_t head = 0;                  // the slot the next record takes
    size_t count = 0;                 // records held, up to kTraceRing
    // The previous tick's values: the centre and camera for the jumps, the window totals for the deltas, the held and active states.
    bool havePrevCentre = false;
    float prevCentre[3] = {};
    bool havePrevCam = false;
    float prevCam[3] = {};
    uint64_t prevRig = 0, prevProducer = 0, prevCull = 0;
    bool prevHeld = false;
    bool prevActive = false;
    uint64_t traceSinceMs = 0;        // this dock's trace started
    uint64_t heldSinceMs = 0;         // the seal was taken
    uint32_t absentRun = 0;           // T4: informative held passes in a row without the own pad
    // The dumps
    uint32_t dumps = 0;               // dumps written this session
    bool dumpedBefore = false;
    uint64_t lastDumpMs = 0;          // the last dump's trigger time
    size_t postLeft = 0;              // post-trigger records still to write
    uint32_t dumpNumber = 0;          // the dump being written
    size_t dumpPre = 0;               // the records its header carried before the trigger

    // The i-th record, oldest first (i < count).
    const TickRecord& at(size_t i) const noexcept { return ring[(head + kTraceRing - count + i) % kTraceRing]; }

    // The 30 s summary reset the window totals: the next deltas count from zero.
    void windowReset() noexcept {
        prevRig = prevProducer = prevCull = 0;
    }

    static void emitRecord(const TickRecord& r, TraceEmit emit, void* ctx) noexcept {
        char line[320];
        formatTickLine(r, line, sizeof(line));
        emit(ctx, line);
    }

    // Called every tick the setting is on. The derived values and the window deltas are kept on every tick; the ring, the triggers and the dumps run
    // while the commander is docked (or a dump is writing).
    void step(const TickRecord& in, TraceEmit emit, void* ctx) noexcept {
        TickRecord r = in;
        const uint64_t nowMs = in.nowMs;
        const bool active = r.docked || postLeft > 0;
        if (active && !prevActive) traceSinceMs = nowMs;
        prevActive = active;
        r.sealedNow = r.held && !prevHeld;
        if (r.sealedNow) heldSinceMs = nowMs;
        prevHeld = r.held;
        r.ms = nowMs - (r.held ? heldSinceMs : traceSinceMs);

        r.shiftM = 0.0f;
        if (r.held && havePrevCentre) r.shiftM = distance(r.centre, prevCentre);
        havePrevCentre = r.held;
        if (r.held) std::memcpy(prevCentre, r.centre, sizeof(prevCentre));

        r.camJumpM = 0.0f;
        if (r.camValid && havePrevCam) r.camJumpM = distance(r.cam, prevCam);
        havePrevCam = r.camValid;
        if (r.camValid) std::memcpy(prevCam, r.cam, sizeof(prevCam));

        r.rigDelta = r.rigVetoes >= prevRig ? r.rigVetoes - prevRig : r.rigVetoes;
        r.producerDelta = r.producerVetoes >= prevProducer ? r.producerVetoes - prevProducer : r.producerVetoes;
        r.cullDelta = r.culled >= prevCull ? r.culled - prevCull : r.culled;
        prevRig = r.rigVetoes;
        prevProducer = r.producerVetoes;
        prevCull = r.culled;

        r.trigger = TraceTrigger::kNone;
        if (postLeft == 0 && r.held && active) {
            if (r.ownSet && r.verdict == TraceVerdict::kInformative) absentRun = r.ownInPass ? 0 : absentRun + 1;
            if (r.refresh == HeldRefresh::kKeptStale) r.trigger = TraceTrigger::kStaleCell;
            else if (r.shiftM > kTraceJumpM) r.trigger = TraceTrigger::kCellJump;
            else if (absentRun == kTraceAbsentPasses) r.trigger = TraceTrigger::kOwnAbsent;
        } else if (!r.held) {
            absentRun = 0;
        }

        if (active) {
            ring[head] = r;
            head = (head + 1) % kTraceRing;
            if (count < kTraceRing) ++count;
        }

        if (postLeft > 0) {   // the records after a trigger, as they come
            emitRecord(r, emit, ctx);
            if (--postLeft == 0) {
                char end[160];
                std::snprintf(end, sizeof(end), "optimized station trace end (dump %u of %u: %zu records before the trigger, %zu after)", dumpNumber,
                              kTraceDumpsMax, dumpPre, kTracePost);
                emit(ctx, end);
            }
            return;
        }

        if (r.trigger != TraceTrigger::kNone && dumps < kTraceDumpsMax && (!dumpedBefore || nowMs - lastDumpMs >= kTraceSpacingMs)) {
            ++dumps;
            dumpedBefore = true;
            lastDumpMs = nowMs;
            dumpNumber = dumps;
            dumpPre = count;
            char hdr[200];
            std::snprintf(hdr, sizeof(hdr), "optimized station trace (dump %u of %u, trigger %s at frame %u%s)", dumps, kTraceDumpsMax,
                          triggerName(r.trigger), static_cast<unsigned>(r.frame), dumps == kTraceDumpsMax ? "; the last dump this session" : "");
            emit(ctx, hdr);
            for (size_t i = 0; i < count; ++i) emitRecord(at(i), emit, ctx);
            postLeft = kTracePost;
        }
    }
};

}  // namespace edvr::sealed
