#pragma once
// The camera-row event detector (I-1 of docs\sky-flicker-2026-10-13.md): CPU only, no key, run on the Pause dump's history.
//
// The 2026-10-13 review (reviews\skybox-flicker-2026-10-13.md) found the camera rows EDVR's temporal pass selects for DLSS jump for ONE frame (0.12 to
// 0.19 m, 1.4 to 3.0 deg) about once every 1.006 s, with a partner 11 frames later, and return the next frame. The dump already holds the selected rows
// of every eye call (temporal_history.h); this turns them into two things the log can show without a spreadsheet:
//
//   * per row: the rotation (deg) and translation (m) from the previous same-eye selected rows, appended to each TEMP line as "rot= mv=";
//   * per dump: the events (an excursion of 1 to 3 frames that is over a threshold and RETURNS), their frames, the leader-to-partner gaps and the period.
//
// An event is judged against the track, not against the last frame: the frames before and after the run are the bracket, the run's rows are compared
// with the straight line between the bracket's ends (so a steady turn or a steady flight is no event at any speed), and the run must return to the
// track the frames before it set (constant-velocity extrapolation) within the same thresholds (so a cut that stays is no event). Thresholds are the
// brief's: 0.5 deg or 0.05 m.
//
// Pure: no Windows, no globals. The rig is tools\sky_flicker_test.

#include "camera_pose_math.h"

#include <vector>

namespace edvr::camevents {

constexpr float kEventDeg = 0.5f;           // an excursion over this many degrees...
constexpr float kEventMeters = 0.05f;       // ...or this many metres is an event, if it returns
constexpr uint32_t kMaxRun = 3;             // the longest excursion called an event: frames
constexpr uint32_t kPartnerMaxGap = 30;     // an event this close after another is its partner; the first after a longer gap leads

// One eye call's selected rows, as the history retains them. valid: the chooser picked rows this frame.
struct Row {
    uint32_t frame = 0;
    uint32_t eye = 0;
    int64_t qpc = 0;
    bool valid = false;
    float rows[12] = {};
};

// The step from the previous VALID same-eye row. have: there is one. gap: the frames between them (1 for consecutive frames).
struct Step {
    bool have = false;
    uint32_t gap = 0;
    float rotDeg = 0.0f;
    float mvM = 0.0f;
};

// The steps for rows in history order. out has n entries.
inline void computeSteps(const Row* rows, size_t n, Step* out) {
    long last[2] = {-1, -1};
    for (size_t i = 0; i < n; ++i) {
        out[i] = Step{};
        const uint32_t eye = rows[i].eye > 1 ? 1u : rows[i].eye;
        if (!rows[i].valid) continue;
        if (last[eye] >= 0) {
            const Row& p = rows[last[eye]];
            out[i].have = true;
            out[i].gap = rows[i].frame >= p.frame ? rows[i].frame - p.frame : 0u;
            out[i].rotDeg = campose::rotationDeg(p.rows, rows[i].rows);
            out[i].mvM = campose::translationM(p.rows, rows[i].rows);
        }
        last[eye] = static_cast<long>(i);
    }
}

// " rot=1.23 mv=0.118" for a step, " rot=- mv=-" without one, and " gap=N" appended when the previous row is not the previous frame.
// Returns the length written, or 0 when the buffer is too small.
inline int formatStep(char* buf, size_t n, const Step& s) {
    int len;
    if (!s.have) {
        len = std::snprintf(buf, n, " rot=- mv=-");
    } else if (s.gap == 1) {
        len = std::snprintf(buf, n, " rot=%.2f mv=%.3f", static_cast<double>(s.rotDeg), static_cast<double>(s.mvM));
    } else {
        len = std::snprintf(buf, n, " rot=%.2f mv=%.3f gap=%u", static_cast<double>(s.rotDeg), static_cast<double>(s.mvM), s.gap);
    }
    return (len <= 0 || static_cast<size_t>(len) >= n) ? 0 : len;
}

struct Event {
    uint32_t frame = 0;       // the first frame of the excursion
    uint32_t run = 0;         // its length in frames (1 to kMaxRun)
    int64_t qpc = 0;          // the first frame's time
    float rotDeg = 0.0f;      // the largest excursion from the track over the run, degrees
    float mvM = 0.0f;         // ...metres
    uint32_t eyeMask = 0;     // bit 0 left, bit 1 right: the eyes whose rows excursed
};

namespace detail {

// The eye's valid rows in frame order, as indices into the history.
inline void eyeRows(const Row* rows, size_t n, uint32_t eye, std::vector<size_t>& out) {
    out.clear();
    for (size_t i = 0; i < n; ++i)
        if (rows[i].valid && (rows[i].eye > 1 ? 1u : rows[i].eye) == eye) out.push_back(i);
}

inline void lerpT(const float a[12], const float b[12], float f, float out[3]) {
    out[0] = a[3] + (b[3] - a[3]) * f;
    out[1] = a[7] + (b[7] - a[7]) * f;
    out[2] = a[11] + (b[11] - a[11]) * f;
}

inline float dist3(const float a[3], const float b[3]) {
    const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

}  // namespace detail

// The events in one eye's rows (history order, frames ascending). A run of L frames starting at index i is an event when every frame of it is over
// a threshold against the line from the frame before it to the frame after it, and the frame after it is back on the track the frames before it set.
inline void detectEye(const Row* rows, size_t n, uint32_t eye, std::vector<Event>& events) {
    std::vector<size_t> idx;
    detail::eyeRows(rows, n, eye, idx);
    const size_t m = idx.size();
    auto frameAt = [&](size_t k) { return rows[idx[k]].frame; };
    auto rowsAt = [&](size_t k) { return rows[idx[k]].rows; };
    size_t i = 1;
    while (i + 1 < m) {
        bool found = false;
        for (uint32_t L = kMaxRun; L >= 1 && !found; --L) {
            if (i + L >= m) continue;                       // the bracket's far end must exist
            bool consecutive = true;
            for (size_t k = i - 1; k < i + L; ++k)
                if (frameAt(k + 1) != frameAt(k) + 1) { consecutive = false; break; }
            if (!consecutive) continue;
            const float* B = rowsAt(i - 1);
            const float* R = rowsAt(i + L);
            const float rotBR = campose::rotationDeg(B, R);
            float maxT = 0.0f, maxRot = 0.0f;
            bool all = true;
            for (uint32_t j = 0; j < L; ++j) {
                const float f = static_cast<float>(j + 1) / static_cast<float>(L + 1);
                float line[3];
                detail::lerpT(B, R, f, line);
                const float p[3] = {rowsAt(i + j)[3], rowsAt(i + j)[7], rowsAt(i + j)[11]};
                const float dT = detail::dist3(p, line);
                const float dR = std::fabs(campose::rotationDeg(B, rowsAt(i + j)) - f * rotBR);
                if (!(dT > kEventMeters || dR > kEventDeg)) { all = false; break; }
                if (dT > maxT) maxT = dT;
                if (dR > maxRot) maxRot = dR;
            }
            if (!all) continue;
            // The return: R against the track the frames before the run set. With a second frame before B on consecutive frames the track has a
            // velocity (L+1 steps of B minus A), otherwise it stands still.
            float expectedT[3] = {B[3], B[7], B[11]};
            float expectedRot = 0.0f;
            if (i >= 2 && frameAt(i - 1) == frameAt(i - 2) + 1) {
                const float* A = rowsAt(i - 2);
                const float steps = static_cast<float>(L + 1);
                expectedT[0] += (B[3] - A[3]) * steps;
                expectedT[1] += (B[7] - A[7]) * steps;
                expectedT[2] += (B[11] - A[11]) * steps;
                expectedRot = campose::rotationDeg(A, B) * steps;
            }
            const float rT[3] = {R[3], R[7], R[11]};
            const float retT = detail::dist3(rT, expectedT);
            const float retRot = std::fabs(rotBR - expectedRot);
            if (retT > kEventMeters || retRot > kEventDeg) continue;
            Event e;
            e.frame = frameAt(i);
            e.run = L;
            e.qpc = rows[idx[i]].qpc;
            e.rotDeg = maxRot;
            e.mvM = maxT;
            e.eyeMask = 1u << eye;
            events.push_back(e);
            i += L + 1;                                      // the run and the frame that returned are spent
            found = true;
        }
        if (!found) ++i;
    }
}

// Both eyes, merged by frame (the same excursion in both eyes is one event with both bits), in frame order.
inline std::vector<Event> detectEvents(const Row* rows, size_t n) {
    std::vector<Event> all;
    detectEye(rows, n, 0, all);
    detectEye(rows, n, 1, all);
    std::sort(all.begin(), all.end(), [](const Event& a, const Event& b) { return a.frame < b.frame; });
    std::vector<Event> merged;
    for (const Event& e : all) {
        if (!merged.empty() && merged.back().frame == e.frame) {
            Event& m = merged.back();
            m.eyeMask |= e.eyeMask;
            if (e.run > m.run) m.run = e.run;
            if (e.rotDeg > m.rotDeg) m.rotDeg = e.rotDeg;
            if (e.mvM > m.mvM) m.mvM = e.mvM;
        } else {
            merged.push_back(e);
        }
    }
    return merged;
}

// The run an event covers frame f with, or 0 when none does.
inline uint32_t eventCovering(const std::vector<Event>& events, uint32_t frame) {
    for (const Event& e : events)
        if (frame >= e.frame && frame < e.frame + e.run) return e.run;
    return 0;
}

struct Summary {
    uint32_t events = 0;            // merged events
    uint32_t frames = 0;            // distinct frames the history covered (valid rows)
    uint32_t eventFrames = 0;       // frames inside events
    uint32_t byRun[kMaxRun + 1] = {};
    uint32_t both = 0, leftOnly = 0, rightOnly = 0;
    uint32_t leaders = 0, partners = 0;
    bool havePeriod = false;
    double periodFrames = 0.0;      // median frames from one leader to the next
    double periodSec = 0.0;         // ...seconds, when the clock is known
    bool havePartnerGap = false;
    double partnerGapFrames = 0.0;  // median frames from a leader to its partner
    float maxRotDeg = 0.0f, maxMvM = 0.0f;
};

inline double median(std::vector<double> v) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    const size_t k = v.size() / 2;
    return (v.size() & 1u) ? v[k] : 0.5 * (v[k - 1] + v[k]);
}

// frames: the distinct frames with valid rows. qpcPerSec: the performance counter's frequency, or 0.
inline Summary summarize(const std::vector<Event>& events, uint32_t frames, double qpcPerSec) {
    Summary s;
    s.frames = frames;
    s.events = static_cast<uint32_t>(events.size());
    std::vector<double> partnerGaps, leaderFrames, leaderSecs;
    bool havePrev = false;
    uint32_t prevFrame = 0;
    int64_t lastLeaderQpc = 0;
    bool haveLeader = false;
    uint32_t lastLeaderFrame = 0;
    for (const Event& e : events) {
        s.eventFrames += e.run;
        if (e.run >= 1 && e.run <= kMaxRun) ++s.byRun[e.run];
        if (e.eyeMask == 3u) ++s.both; else if (e.eyeMask == 1u) ++s.leftOnly; else ++s.rightOnly;
        if (e.rotDeg > s.maxRotDeg) s.maxRotDeg = e.rotDeg;
        if (e.mvM > s.maxMvM) s.maxMvM = e.mvM;
        const bool partner = havePrev && e.frame - prevFrame < kPartnerMaxGap;
        if (partner) {
            ++s.partners;
            partnerGaps.push_back(static_cast<double>(e.frame - prevFrame));
        } else {
            ++s.leaders;
            if (haveLeader) {
                leaderFrames.push_back(static_cast<double>(e.frame - lastLeaderFrame));
                if (qpcPerSec > 0.0) leaderSecs.push_back(static_cast<double>(e.qpc - lastLeaderQpc) / qpcPerSec);
            }
            haveLeader = true;
            lastLeaderFrame = e.frame;
            lastLeaderQpc = e.qpc;
        }
        havePrev = true;
        prevFrame = e.frame;
    }
    if (!leaderFrames.empty()) {
        s.havePeriod = true;
        s.periodFrames = median(leaderFrames);
        s.periodSec = leaderSecs.empty() ? 0.0 : median(leaderSecs);
    }
    if (!partnerGaps.empty()) {
        s.havePartnerGap = true;
        s.partnerGapFrames = median(partnerGaps);
    }
    return s;
}

// The dump's event-summary line. No events reads "camera-row events: 0 of N frames", distinct from a dump that never ran this code. The frames and the
// gaps between events are listed (up to kListMax of each) so the leader-partner pairing and the cycle are on the line.
constexpr size_t kListMax = 24;
inline int formatSummary(char* buf, size_t n, const Summary& s, const std::vector<Event>& events) {
    int len = std::snprintf(buf, n, "camera-row events: %u of %u frames", s.events, s.frames);
    if (len < 0 || static_cast<size_t>(len) >= n) return 0;
    if (s.events == 0) return len;
    auto add = [&](const char* fmt, auto... args) {
        if (len <= 0 || static_cast<size_t>(len) >= n) return;
        const int w = std::snprintf(buf + len, n - static_cast<size_t>(len), fmt, args...);
        if (w < 0 || static_cast<size_t>(len) + static_cast<size_t>(w) >= n) len = 0; else len += w;
    };
    add(" (%u frames inside; run1=%u run2=%u run3=%u; both eyes=%u left only=%u right only=%u)",
        s.eventFrames, s.byRun[1], s.byRun[2], s.byRun[3], s.both, s.leftOnly, s.rightOnly);
    add("; peak rot=%.2f deg mv=%.3f m", static_cast<double>(s.maxRotDeg), static_cast<double>(s.maxMvM));
    add("; frames:");
    for (size_t i = 0; i < events.size() && i < kListMax; ++i) add(" %u", events[i].frame);
    if (events.size() > kListMax) add(" ...");
    add("; gaps:");
    for (size_t i = 1; i < events.size() && i <= kListMax; ++i) add(" %u", events[i].frame - events[i - 1].frame);
    if (events.size() > kListMax + 1) add(" ...");
    add("; leaders=%u partners=%u", s.leaders, s.partners);
    if (s.havePartnerGap) add(" partner_gap=%.1f frames", s.partnerGapFrames);
    else add(" partner_gap=n/a");
    if (s.havePeriod) {
        if (s.periodSec > 0.0) add(" period=%.1f frames (%.3f s)", s.periodFrames, s.periodSec);
        else add(" period=%.1f frames", s.periodFrames);
    } else {
        add(" period=n/a");
    }
    return len;
}

}  // namespace edvr::camevents
