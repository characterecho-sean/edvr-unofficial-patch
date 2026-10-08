// The build's edge cases, on synthetic bodies made from the real moon's patches: every refusal has a case that
// reaches it, and every case names the reason it must give. Included by celestial_motion_test.cpp after its helpers.
#pragma once

#include <limits>

namespace edge {

// The moon's six faces of drawstate frame `frame`, eye 0, and the frame before.
struct Pair {
    std::vector<cel::Patch> cur, prev;
    cel::EyeInput eye;
};
Pair moonPair(const Fixture& fx, uint32_t frame, uint32_t eye = 0) {
    Pair p;
    for (const cel::Patch& q : fx.find(frame, eye)->patches) if (q.body[3] == kMoon) p.cur.push_back(q);
    for (const cel::Patch& q : fx.find(frame - 1, eye)->patches) if (q.body[3] == kMoon) p.prev.push_back(q);
    p.eye = eyeOf(fx, frame, eye);
    return p;
}

cel::BuildResult& run(const Pair& p, const std::vector<cel::Patch>& cur, const std::vector<cel::Patch>& prev) {
    static cel::BuildResult out;
    cel::build(cur.data(), static_cast<uint32_t>(cur.size()), prev.empty() ? nullptr : prev.data(), static_cast<uint32_t>(prev.size()), p.eye, out, scratch());
    return out;
}

// A copy of the patches as another body: its centre moved (the key of cb2[12]) and the whole body carried with it.
std::vector<cel::Patch> relocated(const std::vector<cel::Patch>& v, double dx, double dy, double dz) {
    std::vector<cel::Patch> out = v;
    for (cel::Patch& p : out) {
        p.body[0] += float(dx); p.body[1] += float(dy); p.body[2] += float(dz);
    }
    return out;
}

void all(const Fixture& fx) {
    unsigned cases = 0;
    const Pair base = moonPair(fx, 23654);   // a fast frame, 27 km, with all six faces matched
    {
        cel::BuildResult& r = run(base, base.cur, base.prev);
        check(r.records == 1 && r.body[0].ok, "the base case builds");
        check(r.body[0].matched == 6 && r.body[0].agreeing == 6, "the base case matches and agrees on all six faces");
        ++cases;
    }
    // no previous frame: every body in view has no previous frame, no record
    {
        cel::BuildResult& r = run(base, base.cur, {});
        check(r.records == 0 && r.fallbacks[cel::kFbNoPrevFrame] == 1 && !r.body[0].ok && r.body[0].fallback == cel::kFbNoPrevFrame,
              "no previous frame: a fallback by that name");
        ++cases;
    }
    // every face's static rows changed (a LOD change, all at once): no counterpart, no match
    {
        std::vector<cel::Patch> cur = base.cur;
        for (cel::Patch& p : cur) { p.rows[1] += 1.0f; p.hash = cel::hashRows(p.rows); }
        cel::BuildResult& r = run(base, cur, base.prev);
        check(r.records == 0 && r.fallbacks[cel::kFbNoMatch] == 1 && r.body[0].fallback == cel::kFbNoMatch, "all rows changed: no match");
        ++cases;
    }
    // one face a long way off its history: the others carry the consensus, and the delta is the unperturbed one
    {
        cel::BuildResult& clean = run(base, base.cur, base.prev);
        const double t[3] = {clean.body[0].t[0], clean.body[0].t[1], clean.body[0].t[2]};
        std::vector<cel::Patch> cur = base.cur;
        cur[2].c[0] += 50000.0f;   // 50 km: no rigid motion of the others explains it
        cel::BuildResult& r = run(base, cur, base.prev);
        check(r.records == 1 && r.body[0].ok, "one outlier face: the body still gets a delta");
        check(r.body[0].agreeing + 1 == r.body[0].matched, fmt("one outlier face: the others agree (%u of %u)", r.body[0].agreeing, r.body[0].matched));
        check(cel::dist3(r.body[0].t, t) < 2.0, "one outlier face: the delta is the unperturbed one");
        ++cases;
    }
    // the nearest face is the outlier: the vote still goes to the majority, not the nearest
    {
        cel::BuildResult& clean = run(base, base.cur, base.prev);
        const double t[3] = {clean.body[0].t[0], clean.body[0].t[1], clean.body[0].t[2]};
        std::vector<cel::Patch> cur = base.cur;
        size_t nearest = 0;
        for (size_t i = 1; i < cur.size(); ++i) if (cel::distanceOf(cur[i]) < cel::distanceOf(cur[nearest])) nearest = i;
        cur[nearest].c[1] -= 40000.0f;
        cel::BuildResult& r = run(base, cur, base.prev);
        check(r.records == 1 && cel::dist3(r.body[0].t, t) < 2.0, "the nearest face is the outlier: the majority's delta wins");
        ++cases;
    }
    // faces that each disagree with each other: no consensus
    {
        std::vector<cel::Patch> cur = base.cur;
        for (size_t i = 0; i < cur.size(); ++i) cur[i].c[0] += float(30000.0 * double(i) * double(i + 1));
        cel::BuildResult& r = run(base, cur, base.prev);
        check(r.records == 0 && r.fallbacks[cel::kFbDisagree] == 1, "every face moved its own way: disagree");
        ++cases;
    }
    // a teleport across the view: the whole body (every face the same way) jumped 4.7e6 m, more than it is far from the eye either side of
    // the jump (3.3e6 before, 3.4e6 after) -- the body would have passed through the camera. Rigid and consistent, and not a motion.
    {
        std::vector<cel::Patch> cur = base.cur;
        for (cel::Patch& p : cur) { p.c[0] -= 3.6e6f; p.c[2] -= 3.0e6f; }
        cel::BuildResult& r = run(base, cur, base.prev);
        check(r.records == 0 && r.fallbacks[cel::kFbImplausible] == 1, fmt("a body displaced by more than its distance is implausible (records %u, fallback %s)", r.records, cel::fallbackName(r.body[0].fallback)));
        // the same jump pointing away (the body's new place is farther than the jump): a long approach, which is allowed to be big
        std::vector<cel::Patch> away = base.cur;
        for (cel::Patch& p : away) p.c[2] += 9.0e6f;
        cel::BuildResult& r2 = run(base, away, base.prev);
        check(r2.records == 1 && r2.body[0].ok, "a displacement smaller than the body's distance on either side of it is allowed");
        ++cases;
    }
    // a spin about the body's own centre: one rigid motion, the faces turned and moved together. Ten degrees is the limit.
    {
        const double pi = 3.14159265358979323846;
        auto spun = [&](double degrees) {
            std::vector<cel::Patch> cur = base.cur;
            const double half = degrees * pi / 360.0, s = std::sin(half), c = std::cos(half);
            const double rc = std::cos(degrees * pi / 180.0), rs = std::sin(degrees * pi / 180.0);
            const double Rw[9] = {rc, 0, rs, 0, 1, 0, -rs, 0, rc};   // about world-aligned Y
            for (cel::Patch& p : cur) {
                // world-aligned: X' = B + Rw (X - B); back in head axes by A^T
                const double B[3] = {p.body[0], p.body[1], p.body[2]};
                double A[9];
                for (int i = 0; i < 9; ++i) A[i] = p.A[i];
                const double cc[3] = {p.c[0], p.c[1], p.c[2]};
                double X[3];
                cel::mulV(A, cc, X);
                const double d[3] = {X[0] - B[0], X[1] - B[1], X[2] - B[2]};
                double rd[3];
                cel::mulV(Rw, d, rd);
                const double Xn[3] = {B[0] + rd[0], B[1] + rd[1], B[2] + rd[2]};
                double h[3];
                cel::mulTV(A, Xn, h);
                p.c[0] = float(h[0]); p.c[1] = float(h[1]); p.c[2] = float(h[2]);
                // the orientation: qh = (A^T e_y sin(half), cos(half)) (x) q; A^T e_y is row 1 of A
                const double qx = A[3] * s, qy = A[4] * s, qz = A[5] * s, qw = c;
                const double x = p.q[0], y = p.q[1], z = p.q[2], w = p.q[3];
                p.q[0] = float(qw * x + qx * w + qy * z - qz * y);
                p.q[1] = float(qw * y - qx * z + qy * w + qz * x);
                p.q[2] = float(qw * z + qx * y - qy * x + qz * w);
                p.q[3] = float(qw * w - qx * x - qy * y - qz * z);
            }
            return cur;
        };
        {
            cel::BuildResult& r = run(base, spun(5.0), base.prev);
            check(r.records == 1 && r.body[0].ok && r.body[0].rotDeg > 4.0 && r.body[0].rotDeg < 6.0,
                  fmt("a 5 degree spin is a body's motion (rotDeg %.2f, fallback %s)", r.body[0].rotDeg, cel::fallbackName(r.body[0].fallback)));
        }
        {
            cel::BuildResult& r = run(base, spun(30.0), base.prev);
            check(r.records == 0 && r.fallbacks[cel::kFbImplausible] == 1, "a 30 degree spin in one frame is implausible");
        }
        ++cases;
    }
    // a previous body of another radius, or none near: no previous body
    {
        std::vector<cel::Patch> prev = base.prev;
        for (cel::Patch& p : prev) p.body[3] *= 1.5f;
        cel::BuildResult& r = run(base, base.cur, prev);
        check(r.records == 0 && r.fallbacks[cel::kFbNoPrevBody] == 1, "the previous frame's body has another radius: no previous body");
        std::vector<cel::Patch> distant = relocated(base.prev, 9.0e9, 0, 0);
        cel::BuildResult& r2 = run(base, base.cur, distant);
        check(r2.records == 0 && r2.fallbacks[cel::kFbNoPrevBody] == 1, "the previous frame's body is a long way off: no previous body");
        ++cases;
    }
    // two previous bodies of the same radius, one clearly the nearer: it wins; two equally near: neither
    {
        std::vector<cel::Patch> prev = base.prev;
        const std::vector<cel::Patch> twin = relocated(base.prev, 9.0e6, 1.0e6, 0);   // a same-radius body elsewhere (its rows equal the moon's)
        prev.insert(prev.end(), twin.begin(), twin.end());
        cel::BuildResult& r = run(base, base.cur, prev);
        check(r.records == 1 && r.body[0].ok, "a same-radius body far away does not confuse the previous-body match");
        std::vector<cel::Patch> both = base.prev;
        const std::vector<cel::Patch> t1 = relocated(base.prev, 5.0, 0, 0), t2 = relocated(base.prev, 0, 5.0, 0);
        both.insert(both.end(), t1.begin(), t1.end());
        both.insert(both.end(), t2.begin(), t2.end());
        cel::BuildResult& r2 = run(base, base.cur, both);
        check(r2.records == 0 && r2.fallbacks[cel::kFbNoPrevBody] == 1, "two previous bodies equally near: not trusted");
        ++cases;
    }
    // faces with identical rows inside the previous body: the tie breaks by where the centre's own move puts the patch
    {
        std::vector<cel::Patch> prev = base.prev;
        // make face 3's prev rows equal face 1's; face 1 and 3 are different places, the centre's shift tells them apart
        for (int i = 0; i < 16; ++i) prev[3].rows[i] = prev[1].rows[i];
        prev[3].hash = prev[1].hash;
        std::vector<cel::Patch> cur = base.cur;
        for (int i = 0; i < 16; ++i) cur[3].rows[i] = cur[1].rows[i];
        cur[3].hash = cur[1].hash;
        cel::BuildResult& r = run(base, cur, prev);
        check(r.records == 1 && r.body[0].ok && r.body[0].matched >= 4, fmt("identical rows on two faces are told apart by position (matched %u)", r.body[0].matched));
        ++cases;
    }
    // duplicates among the current patches of one body: their prev patch is claimed twice, neither trusted; the rest carry on
    {
        std::vector<cel::Patch> cur = base.cur;
        cur.push_back(cur[0]);
        cur.back().c[1] += 3.0f;   // same rows, another place: two current patches claim one previous
        cel::BuildResult& r = run(base, cur, base.prev);
        check(r.records == 1 && r.body[0].ok && r.body[0].matched == base.cur.size() - 1,
              fmt("a prev patch claimed twice is dropped, the others carry on (matched %u of %zu)", r.body[0].matched, base.cur.size()));
        ++cases;
    }
    // sixteen records at most, nearest first, the overflow counted: twenty copies of the moon, each its own body (its own cb2[12]) and
    // its own place (a patch's c moved with its body), spread across the view
    {
        std::vector<cel::Patch> cur, prev;
        for (int i = 0; i < 20; ++i) {
            const float dx = 60000.0f * float(i), dy = 30000.0f * float(i % 4);
            for (int pass = 0; pass < 2; ++pass) {
                std::vector<cel::Patch> v = pass == 0 ? base.cur : base.prev;
                for (cel::Patch& q : v) { q.body[0] += dx; q.body[1] += dy; q.c[0] += dx; q.c[1] += dy; }
                std::vector<cel::Patch>& dst = pass == 0 ? cur : prev;
                dst.insert(dst.end(), v.begin(), v.end());
            }
        }
        cel::BuildResult& r = run(base, cur, prev);
        check(r.bodies == 20, fmt("twenty bodies are told apart by cb2[12], got %u", r.bodies));
        check(r.records == cel::kMaxBodies && r.overflowBodies == 4, fmt("sixteen records, four left to the camera term; got %u and %u", r.records, r.overflowBodies));
        for (uint32_t k = 1; k < r.records; ++k) check(r.body[r.order[k - 1]].nearest <= r.body[r.order[k]].nearest, "overflow keeps the nearest, nearest first");
        double worstKept = 0.0, bestLeft = 1e300;
        for (uint32_t b = 0; b < r.bodies; ++b) {
            bool kept = false;
            for (uint32_t k = 0; k < r.records; ++k) kept = kept || r.order[k] == b;
            if (kept) worstKept = std::max(worstKept, r.body[b].nearest);
            else if (r.body[b].ok) bestLeft = std::min(bestLeft, r.body[b].nearest);
        }
        check(worstKept <= bestLeft, "every body left out is farther than every body kept");
        ++cases;
    }
    // a patch box that reaches the eye plane makes the volume the whole eye from the plane out
    {
        std::vector<cel::Patch> cur = base.cur, prev = base.prev;
        for (cel::Patch& p : cur) p.c[2] -= 3.3e6f;   // the face centres now about the eye's plane: boxes straddle it
        for (cel::Patch& p : prev) p.c[2] -= 3.3e6f;
        cel::BuildResult& r = run(base, cur, prev);
        bool any = false;
        for (uint32_t b = 0; b < r.bodies; ++b) any = any || r.body[b].straddle;
        check(any, "a body whose patches reach the eye plane has a whole-eye volume");
        if (r.records) {
            const float* g = r.gpu[0];
            check(g[12] < 0 && g[14] > base.eye.w && g[16] == 0.0f, "a straddling volume is the whole eye with zmin 0");
            // ...and, since 2026-10-08, it is held to the body's radial shell: rMax in span.w, the centre and rMin in ctr
            check(r.body[0].shell && g[19] > 0.0f && g[19] == float(r.body[0].rMax) && g[23] == float(r.body[0].rMin) && g[23] < g[19],
                  "a straddling record carries its shell (rMax in span.w, rMin in ctr.w)");
            check(g[20] == float(r.body[0].shellCentre[0]) && g[21] == float(r.body[0].shellCentre[1]) && g[22] == float(r.body[0].shellCentre[2]),
                  "and the body's centre in the shader's view space");
        }
        // a body wholly in front of the eye plane keeps its box and depth slab and carries no shell
        {
            cel::BuildResult& n = run(base, base.cur, base.prev);
            check(n.records == 1 && !n.body[0].straddle && !n.body[0].shell, "a body wholly in front of the eye plane does not straddle");
            const float* g = n.gpu[0];
            check(g[19] == 0.0f && g[20] == 0.0f && g[21] == 0.0f && g[22] == 0.0f && g[23] == 0.0f, "...and its record has no shell (span.w and ctr are 0)");
            check(n.body[0].rMax > 0.0 && n.body[0].rMin <= double(kMoon) && double(kMoon) <= n.body[0].rMax, "...though the shell is computed for it and holds the moon's radius");
        }
        ++cases;
    }
    // a constant block that is not a patch's: non-finite values, a translation in the rows, a quaternion that is not one
    {
        uint8_t b2[cel::kB2Bytes] = {}, b0[cel::kB0Bytes] = {};
        float f[4 * 24] = {};
        f[10 * 4 + 3] = 1.0f;   // q = identity
        f[12 * 4 + 3] = 1000.0f;
        std::memcpy(b2, f, sizeof f);
        float a[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
        std::memcpy(b0, a, sizeof a);
        cel::Patch p;
        check(cel::patchFromBlocks(b2, b0, p) == cel::kCaptureOk, "an identity patch is a patch");
        f[2 * 4] = std::numeric_limits<float>::quiet_NaN();
        std::memcpy(b2, f, sizeof f);
        check(cel::patchFromBlocks(b2, b0, p) == cel::kCaptureNonFinite, "a NaN centre is refused");
        f[2 * 4] = 0.0f;
        f[10 * 4 + 3] = 0.0f;
        std::memcpy(b2, f, sizeof f);
        check(cel::patchFromBlocks(b2, b0, p) == cel::kCaptureBadQuaternion, "a zero quaternion is refused");
        f[10 * 4 + 3] = 1.0f;
        std::memcpy(b2, f, sizeof f);
        a[3] = 5.0f;   // a translation in cb0's row 9
        std::memcpy(b0, a, sizeof a);
        check(cel::patchFromBlocks(b2, b0, p) == cel::kCaptureRowsMove, "rows with a translation are not the rows this math reads");
        ++cases;
    }
    // the shader's arithmetic outside a volume, and behind the eye
    {
        cel::BuildResult& r = run(base, base.cur, base.prev);
        const float* rec = r.gpu[0];
        double mx, my;
        check(!cel::shaderMotion(rec, base.eye.tan, base.eye.tan, kW, kH, rec[12] - 10, rec[13] + 5, 3.5e6, &mx, &my), "left of the volume");
        check(!cel::shaderMotion(rec, base.eye.tan, base.eye.tan, kW, kH, rec[12] + 5, rec[13] + 5, rec[17] + 100.0, &mx, &my), "behind the volume");
        check(!cel::shaderMotion(rec, base.eye.tan, base.eye.tan, kW, kH, rec[12] + 5, rec[13] + 5, rec[16] - 1.0, &mx, &my), "in front of the volume");
        ++cases;
    }
    std::printf("(e) %u edge cases of the build: each refusal reached, each by its own reason\n", cases);
}

}  // namespace edge
