// (f) The radial shell of a body whose patch boxes reach the eye plane (2026-10-08). Included by warp_tests.h inside namespace warp, after
// the judge and the controls it reuses.
//
// THE FAULT. cel::build() gave such a body ("straddle": a corner of a patch box at or behind the eye plane) the whole eye from depth 0
// as its volume, and never counted it off-screen. Every world-path pixel nearer than the body's far corners then took the body's rigid
// motion -- a hangar's walls 30 m away as much as the planet 12,000 km off -- and docked in a spinning station that is the station's
// spin on the walls (a Quest 3 log, v0.18.3: the station background a grainy mess, v0.18.2 fine). THE FIX. The record carries the
// body's centre in the eye's view space and a radial shell [rMin, rMax] read from the patches' own boxes; the shader takes a straddling
// body's pixel only where its view-space point lies in the shell.
//
// WHAT RUNS HERE, on synthetic bodies whose geometry is built from first principles (a sphere tiled by patch boxes that enclose it),
// through the production build, the production shader on WARP and the same judge as the real-moon scenes:
//   1. the station: an eye 12,213 km from the nearest patch of a 4,478 km body, the body ~80 degrees off the view axis so its boxes
//      straddle. A world-path pixel at 30 m takes the camera's motion; the body's surface takes path 12; so do not an object inside
//      the planet or one 2,000 km in front of it. Sweeps of depth, the legacy record (shell stripped), five shader mutants;
//   2. the landed ship: the eye 2 m above the surface. The ground still takes path 12;
//   3. the float32 the shader subtracts in (the centre is 1.7e7 m off): measured on the CPU and on WARP, metres;
//   4. the real constants of eye dump 180540: the shell about every real body contains the body's own radius.
#pragma once

namespace shell {
constexpr double kPi = 3.14159265358979323846;

void cross3(const double a[3], const double b[3], double o[3]) {
    o[0] = a[1] * b[2] - a[2] * b[1];
    o[1] = a[2] * b[0] - a[0] * b[2];
    o[2] = a[0] * b[1] - a[1] * b[0];
}
void unit3(double v[3]) {
    const double n = cel::len3(v);
    v[0] /= n; v[1] /= n; v[2] /= n;
}
// The unit quaternion (x y z w) of a rotation matrix, row-major, the convention cel::quatToMat reads.
void matToQuat(const double M[9], float q[4]) {
    double x, y, z, w;
    const double tr = M[0] + M[4] + M[8];
    if (tr > 0.0) {
        const double s = std::sqrt(tr + 1.0) * 2.0;
        w = 0.25 * s; x = (M[7] - M[5]) / s; y = (M[2] - M[6]) / s; z = (M[3] - M[1]) / s;
    } else if (M[0] > M[4] && M[0] > M[8]) {
        const double s = std::sqrt(1.0 + M[0] - M[4] - M[8]) * 2.0;
        w = (M[7] - M[5]) / s; x = 0.25 * s; y = (M[1] + M[3]) / s; z = (M[2] + M[6]) / s;
    } else if (M[4] > M[8]) {
        const double s = std::sqrt(1.0 + M[4] - M[0] - M[8]) * 2.0;
        w = (M[2] - M[6]) / s; x = (M[1] + M[3]) / s; y = 0.25 * s; z = (M[5] + M[7]) / s;
    } else {
        const double s = std::sqrt(1.0 + M[8] - M[0] - M[4]) * 2.0;
        w = (M[3] - M[1]) / s; x = (M[2] + M[6]) / s; y = (M[5] + M[7]) / s; z = 0.25 * s;
    }
    q[0] = float(x); q[1] = float(y); q[2] = float(z); q[3] = float(w);
}
// A = Ry(yaw) Rx(pitch): the camera rows' 3x3, so the world-aligned frame is not the head's.
void rotationOf(double yawDeg, double pitchDeg, double A[9]) {
    const double y = yawDeg * kPi / 180.0, p = pitchDeg * kPi / 180.0;
    const double Ry[9] = {std::cos(y), 0, std::sin(y), 0, 1, 0, -std::sin(y), 0, std::cos(y)};
    const double Rx[9] = {1, 0, 0, 0, std::cos(p), -std::sin(p), 0, std::sin(p), std::cos(p)};
    cel::mulM(Ry, Rx, A);
}

// A sphere of radius R at head-axes point C (the eye at the origin), as the patches that cover what the eye sees of it: the cap
// out to the horizon, cut at the polar angles `thetas` about the direction from the centre to the eye and into nPsi wedges each.
// Each patch is a box in the frame (tangent, radial, tangent) that encloses the surface of its cell to +-terrain metres, with the
// patch centre c on the surface at the cell's middle -- every number in the VS's own terms (cb2[2] c, [4..7] the boxes, [10] q,
// [12] the body, cb0's A). The box is measured from the float32 c the game would hold, so the corners the build reads are the
// surface to float rounding. `prev` is the same body carried by `shift` (head axes) -- one rigid motion for the build to find.
struct Cap {
    double R = 0, C[3] = {}, A[9] = {};
    std::vector<cel::Patch> cur, prev;
};
Cap makeCap(double R, const double C[3], const double A[9], const std::vector<double>& thetas, int nPsi, double terrain, const double shift[3]) {
    Cap cap;
    cap.R = R;
    for (int i = 0; i < 3; ++i) cap.C[i] = C[i];
    for (int i = 0; i < 9; ++i) cap.A[i] = A[i];
    const double D = cel::len3(C);
    const double e[3] = {-C[0] / D, -C[1] / D, -C[2] / D};
    double a[3] = {0, 1, 0};
    if (std::fabs(e[1]) > 0.9) { a[0] = 1; a[1] = 0; }
    double u[3], v[3];
    {
        const double d = a[0] * e[0] + a[1] * e[1] + a[2] * e[2];
        for (int i = 0; i < 3; ++i) u[i] = a[i] - d * e[i];
        unit3(u);
        cross3(e, u, v);
    }
    auto dirOf = [&](double th, double ps, double n[3]) {
        for (int i = 0; i < 3; ++i) n[i] = std::cos(th) * e[i] + std::sin(th) * (std::cos(ps) * u[i] + std::sin(ps) * v[i]);
    };
    auto dirTheta = [&](double th, double ps, double t[3]) {
        for (int i = 0; i < 3; ++i) t[i] = -std::sin(th) * e[i] + std::cos(th) * (std::cos(ps) * u[i] + std::sin(ps) * v[i]);
    };
    double B[3];
    cel::mulV(cap.A, C, B);
    double Bs[3], Cs[3] = {C[0] + shift[0], C[1] + shift[1], C[2] + shift[2]};
    cel::mulV(cap.A, Cs, Bs);
    for (size_t i = 0; i + 1 < thetas.size(); ++i) {
        for (int j = 0; j < nPsi; ++j) {
            const double th0 = thetas[i], th1 = thetas[i + 1];
            const double ps0 = 2.0 * kPi * j / nPsi, ps1 = 2.0 * kPi * (j + 1) / nPsi;
            double nc[3], t1[3], t2[3];
            dirOf(0.5 * (th0 + th1), 0.5 * (ps0 + ps1), nc);
            dirTheta(0.5 * (th0 + th1), 0.5 * (ps0 + ps1), t1);
            unit3(t1);
            cross3(t1, nc, t2);
            unit3(t2);
            const double Rm[9] = {t1[0], nc[0], t2[0], t1[1], nc[1], t2[1], t1[2], nc[2], t2[2]};   // columns: tangent, radial, tangent
            cel::Patch p{};
            matToQuat(Rm, p.q);
            double Rf[9];
            cel::quatToMat(p.q, Rf);
            for (int k = 0; k < 3; ++k) p.c[k] = float(C[k] + R * nc[k]);
            double lo[3] = {1e300, 1e300, 1e300}, hi[3] = {-1e300, -1e300, -1e300};
            for (int ia = 0; ia < 5; ++ia)
                for (int ib = 0; ib < 5; ++ib)
                    for (int ir = -1; ir <= 1; ++ir) {
                        double n[3];
                        dirOf(th0 + (th1 - th0) * ia / 4.0, ps0 + (ps1 - ps0) * ib / 4.0, n);
                        const double rad = R + ir * terrain;
                        const double s[3] = {C[0] + rad * n[0] - p.c[0], C[1] + rad * n[1] - p.c[1], C[2] + rad * n[2] - p.c[2]};
                        double l[3];
                        cel::mulTV(Rf, s, l);
                        for (int k = 0; k < 3; ++k) { lo[k] = std::min(lo[k], l[k]); hi[k] = std::max(hi[k], l[k]); }
                    }
            // a pad that differs cell to cell, so no two patches share their rows (the build matches patches across frames by them)
            const double pad = 1.0 + 0.013 * double(i * static_cast<size_t>(nPsi) + static_cast<size_t>(j));
            for (int k = 0; k < 3; ++k) { p.rows[k] = float(lo[k] - pad); p.rows[4 + k] = float(hi[k] + pad); }
            std::memcpy(&p.rows[8], &p.rows[0], 32);   // box two, the LOD morph's target, is the same box
            p.q2[3] = 1.0f;
            for (int k = 0; k < 9; ++k) p.A[k] = float(A[k]);
            for (int k = 0; k < 3; ++k) p.body[k] = float(B[k]);
            p.body[3] = float(R);
            p.hash = cel::hashRows(p.rows);
            cap.cur.push_back(p);
            cel::Patch q = p;
            for (int k = 0; k < 3; ++k) { q.c[k] = float(double(p.c[k]) + shift[k]); q.body[k] = float(Bs[k]); }
            cap.prev.push_back(q);
        }
    }
    return cap;
}

// ---- the two scenarios ----------------------------------------------------------------------------------------------------
struct Scenario {
    const char* name;
    Cap cap;
    float tan[4];
    float split;
};
// The station: 12,213 km from the nearest patch of a 4,478 km body, the body 80 degrees off the view axis (+Z) toward +X. The view is
// wide on that side (tan right 3) so a strip of the surface is on the eye's pixels.
Scenario station() {
    Scenario s{"station", {}, {-1.0f, 3.0f, -1.0f, 1.0f}, 20.0f};
    const double R = 4.478e6, D = R + 12.213e6, az = 80.0 * kPi / 180.0;
    const double C[3] = {D * std::sin(az), 0.0, D * std::cos(az)};
    double A[9];
    rotationOf(37.0, 11.0, A);
    const double horizon = std::acos(R / D);
    const double shift[3] = {-300.0, 40.0, -5000.0};
    s.cap = makeCap(R, C, A, {0.0, 0.1, 0.25, 0.4, 0.6, 0.8, 1.0, 1.15, horizon}, 16, 9000.0, shift);
    return s;
}
// The landed ship: the eye 2 m above the ground, the planet straight below. Patches from 9 m to the horizon at 4.2 km.
Scenario landed() {
    Scenario s{"landed", {}, {-1.0f, 1.0f, -1.0f, 1.0f}, 0.5f};
    const double R = 4.478e6, D = R + 2.0;
    const double C[3] = {0.0, -D, 0.0};
    double A[9];
    rotationOf(-20.0, 0.0, A);
    const double horizon = std::acos(R / D);
    const double shift[3] = {0.02, 0.0, 0.05};
    s.cap = makeCap(R, C, A, {0.0, 2.0e-6, 2.0e-5, 2.0e-4, horizon}, 8, 3.0, shift);
    return s;
}

// The depth along +Z at which a pixel's ray meets the sphere, or -1.
double sphereDepth(const float tan[4], int x, int y, const double C[3], double R) {
    double dx, dy;
    rayOf(tan, x, y, &dx, &dy);
    const double a = dx * dx + dy * dy + 1.0;
    const double b = -2.0 * (dx * C[0] + dy * C[1] + C[2]);
    const double c = C[0] * C[0] + C[1] * C[1] + C[2] * C[2] - R * R;
    const double disc = b * b - 4.0 * a * c;
    if (disc < 0.0) return -1.0;
    const double t = (-b - std::sqrt(disc)) / (2.0 * a);
    return t > 0.0 ? t : -1.0;
}

// A rectangle of pixels at a fixed depth (metres), or at an offset from the sphere's own depth under them.
struct Block {
    const char* name;
    int x0, y0, x1, y1;
    double metres;    // absolute depth when !relative
    double offset;    // added to the sphere's depth when relative (a pixel with no sphere under it is left alone)
    bool relative;
    int pixels = 0;
};

// The scene of a scenario as a Case: the production build of its patches, the sphere's depth with the blocks in front, the records at t15.
struct SynthCase {
    Case c;
    std::vector<Block> blocks;
    std::vector<int> label;   // per pixel: 0 sky, 1 sphere, 2.. block index + 2
    int spherePixels = 0;
};
void uploadRecords(Rig& R, Case& c) {
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = sizeof(c.build.gpu);
    bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    bd.StructureByteStride = cel::kRecordFloats * 4;
    D3D11_SUBRESOURCE_DATA init{c.build.gpu, 0, 0};
    c.crBuf.Reset();
    c.crSrv.Reset();
    hr(R.dev->CreateBuffer(&bd, &init, &c.crBuf), "record buffer (shell)");
    hr(R.dev->CreateShaderResourceView(c.crBuf.Get(), nullptr, &c.crSrv), "record SRV (shell)");
    c.withRecords[15] = c.crSrv.Get();
}
void makeSynth(Rig& R, const Scenario& s, std::vector<Block> blocks, SynthCase& out) {
    Case& c = out.c;
    c = Case{};
    c.frame = 1;
    std::memcpy(c.in.tan, s.tan, 16);
    c.in.w = kVW;
    c.in.h = kVH;
    c.inPrev = c.in;
    c.split = s.split;
    cel::build(s.cap.cur.data(), static_cast<uint32_t>(s.cap.cur.size()), s.cap.prev.data(), static_cast<uint32_t>(s.cap.prev.size()), c.in, c.build, scratch());
    Scene& sc = c.sc;
    sc.s.assign(static_cast<size_t>(kVW) * kVH * 4, 0.0f);
    sc.h.assign(sc.s.size(), 0.0f);
    sc.z.assign(static_cast<size_t>(kVW) * kVH, 0.0f);
    out.label.assign(static_cast<size_t>(kVW) * kVH, 0);
    Lcg g{777};
    for (int y = 0; y < kVH; ++y)
        for (int x = 0; x < kVW; ++x) {
            const size_t i = static_cast<size_t>(y) * kVW + x;
            for (int k = 0; k < 4; ++k) {
                sc.s[i * 4 + k] = k == 3 ? 1.0f : 0.15f + 0.7f * g.unit();
                sc.h[i * 4 + k] = k == 3 ? 1.0f : 0.15f + 0.7f * g.unit();
            }
            const double t = sphereDepth(s.tan, x, y, s.cap.C, s.cap.R);
            if (t > 0.0) { sc.z[i] = kDepthB / static_cast<float>(t); out.label[i] = 1; ++out.spherePixels; }
        }
    out.blocks = std::move(blocks);
    for (size_t b = 0; b < out.blocks.size(); ++b) {
        Block& k = out.blocks[b];
        for (int y = k.y0; y < k.y1; ++y)
            for (int x = k.x0; x < k.x1; ++x) {
                const size_t i = static_cast<size_t>(y) * kVW + x;
                float z;
                if (k.relative) {
                    if (out.label[i] != 1) continue;
                    z = static_cast<float>(kDepthB / sc.z[i] + k.offset);
                } else {
                    z = static_cast<float>(k.metres);
                }
                sc.z[i] = kDepthB / z;
                out.label[i] = static_cast<int>(b) + 2;
                ++k.pixels;
            }
    }
    sc.srvS = texSrv(R, DXGI_FORMAT_R32G32B32A32_FLOAT, 16, sc.s.data());
    sc.srvH = texSrv(R, DXGI_FORMAT_R32G32B32A32_FLOAT, 16, sc.h.data());
    sc.srvZ = texSrv(R, DXGI_FORMAT_R32_FLOAT, 4, sc.z.data());
    c.withRecords[0] = c.without[0] = sc.srvS.Get();
    c.withRecords[1] = c.without[1] = sc.srvH.Get();
    c.withRecords[2] = c.without[2] = sc.srvZ.Get();
    uploadRecords(R, c);
    c.off = baseParams(s.tan, s.tan);
    c.off.split[0] = s.split;
    c.on = c.off;
    c.on.probe[3] = 8192.0f;
}

uint32_t pathOf(const Outputs& C, size_t i) { return static_cast<uint32_t>(at(C.v[kSlotU7], i * 4 + 3)) & 15u; }

// Pixels of a label whose whole 5x5 neighbourhood carries the same label: away from every edge the nearest-depth 3x3 smears.
std::vector<size_t> interiorOf(const SynthCase& sc, int label) {
    std::vector<size_t> out;
    for (int y = 2; y < kVH - 2; ++y)
        for (int x = 2; x < kVW - 2; ++x) {
            bool same = true;
            for (int oy = -2; oy <= 2 && same; ++oy)
                for (int ox = -2; ox <= 2 && same; ++ox) same = sc.label[static_cast<size_t>(y + oy) * kVW + x + ox] == label;
            if (same) out.push_back(static_cast<size_t>(y) * kVW + x);
        }
    return out;
}

struct Counts { size_t claimed = 0, total = 0, changed = 0, lost = 0; };
// claimed: path 12. changed: the run differs from the reference (the shader without the path) at the pixel, in path or motion. lost: the body's
// motion was taken and then landed behind last frame's eye (dp.z >= 0: no valid projection, path 0), so the pixel keeps no history at all.
Counts claimedOf(const Outputs& A, const Outputs& C, const std::vector<size_t>& px) {
    Counts n;
    for (size_t i : px) {
        ++n.total;
        if (pathOf(C, i) == 12u) ++n.claimed;
        if (pathOf(C, i) != pathOf(A, i) || std::memcmp(&A.v[kSlotMV][i * 8], &C.v[kSlotMV][i * 8], 8) != 0) ++n.changed;
        if (pathOf(C, i) == 0u && pathOf(A, i) != 0u) ++n.lost;
    }
    return n;
}

// The shader's arithmetic in float32, for the precision figure: what celestialPixel computes for one pixel (the dispatch's nearest-depth
// z, the ray, P = d z, |P - centre|) in single precision, and the same from the same float inputs in double.
struct Radial { double f, exact; };
Radial radialOf(const float tan[4], int x, int y, float zraw, const float ctr[3]) {
    const float zr = zraw - 0.0f;                   // knobs.x = 0
    const float z = kDepthB / zr;                   // knobs.z / den
    float dx = tan[0] + (float(x) + 0.5f) / float(kVW) * (tan[1] - tan[0]);
    float dy = tan[3] - (float(y) + 0.5f) / float(kVH) * (tan[3] - tan[2]);
    const float Px = dx * z, Py = dy * z, Pz = -1.0f * z;
    const float ex = Px - ctr[0], ey = Py - ctr[1], ez = Pz - ctr[2];
    Radial r;
    r.f = std::sqrt(ex * ex + ey * ey + ez * ez);
    const double Pd[3] = {double(dx) * double(z), double(dy) * double(z), -double(z)};
    const double cd[3] = {ctr[0], ctr[1], ctr[2]};
    r.exact = cel::dist3(Pd, cd);
    return r;
}

// A record buffer for a case with the first record's shell replaced: rMax and rMin (rMax <= 0 = off).
void setShell(Rig& R, Case& c, float rMin, float rMax) {
    c.build.gpu[0][19] = rMax;
    c.build.gpu[0][23] = rMin;
    uploadRecords(R, c);
}

void realShells(const Fixture& fx) {
    unsigned bodies = 0, frames = 0;
    double thinnest = 1e300;
    for (uint32_t eye = 0; eye < 2; ++eye)
        for (uint32_t frame = 23651; frame <= 23668; ++frame) {
            const EyeFrame* cur = fx.find(frame, eye);
            const EyeFrame* prev = fx.find(frame - 1, eye);
            const cel::EyeInput in = eyeOf(fx, frame, eye);
            cel::BuildResult out;
            cel::build(cur->patches.data(), static_cast<uint32_t>(cur->patches.size()), prev->patches.data(), static_cast<uint32_t>(prev->patches.size()), in, out, scratch());
            check(out.bodies == 6, "six real bodies");
            for (uint32_t b = 0; b < out.bodies; ++b) {
                const cel::BodyResult& r = out.body[b];
                // the shell is read for every body with finite corners, in front of the eye or not
                check(r.rMax > 0.0 && r.rMin >= 0.0 && r.rMin < r.rMax, fmt("frame %u eye %u body %.0f: a shell [%.0f, %.0f]", frame, eye, r.radius, r.rMin, r.rMax));
                check(r.rMin <= r.radius && r.radius <= r.rMax,
                      fmt("frame %u eye %u body %.0f: the body's own radius lies inside its shell [%.0f, %.0f]", frame, eye, r.radius, r.rMin, r.rMax));
                // and so does every patch's own centre c, a point on the surface (the doc: |A c - centre| is the radius within 5 km)
                const cel::Patch* any = nullptr;
                for (const cel::Patch& p : cur->patches) {
                    if (p.body[3] != r.radius) continue;
                    any = &p;
                    const double Hc[3] = {r.shellCentre[0], r.shellCentre[1], -r.shellCentre[2]};   // the shell's centre is the shader's view; head axes flip z back
                    const double pc[3] = {p.c[0], p.c[1], p.c[2]};
                    const double d = cel::dist3(pc, Hc);
                    check(d >= r.rMin && d <= r.rMax, fmt("frame %u eye %u body %.0f: a patch centre is %.0f m from the body's centre, outside [%.0f, %.0f]",
                                                          frame, eye, r.radius, d, r.rMin, r.rMax));
                }
                check(any != nullptr, "the body has a patch");
                thinnest = std::min(thinnest, (r.rMax - r.rMin) / r.radius);
                ++bodies;
            }
            ++frames;
        }
    std::printf("(f) real constants: %u bodies in %u eye-frames, each shell holds the body's own radius and every patch centre; thinnest shell %.1f%% of the radius\n",
                bodies, frames, 100.0 * thinnest);
}

void all(Rig& R, std::vector<Variant>& v, const Fixture& fx) {
    realShells(fx);
    // ============================== 1. the station ==============================
    Scenario st = station();
    std::vector<Block> blocks = {
        {"hangar wall A, 30 m, clear of the planet", 40, 90, 140, 220, 30.0, 0, false},
        {"hangar wall B, 30 m, in front of the planet's pixels", 430, 200, 470, 260, 30.0, 0, false},
        {"hangar wall C, 300 m", 150, 300, 230, 380, 300.0, 0, false},
        {"a thing 700 km (depth) in front of the surface", 440, 300, 470, 330, 0.0, -7.0e5, true},
        // (the visible strip is the planet's near limb, seen at a graze: a ray there leaves the planet again within a few hundred km, so the
        // thing "inside" is put where the chord is long -- the strip's right edge, 8.6 degrees off the planet's centre -- and 2,400 km along it)
        {"a thing 770 km (depth) inside the planet", 470, 230, 498, 262, 0.0, +7.7e5, true},
    };
    SynthCase sc;
    makeSynth(R, st, blocks, sc);
    Case& c = sc.c;
    // the build, from first principles, is what the fault needs
    {
        const cel::BuildResult& b = c.build;
        check(b.bodies == 1 && b.records == 1 && b.body[0].ok, fmt("station: the synthetic body builds a record (bodies %u records %u fallback %s)", b.bodies, b.records, cel::fallbackName(b.body[0].fallback)));
        const cel::BodyResult& body = b.body[0];
        check(body.matched == st.cap.cur.size() && body.agreeing == body.matched, fmt("station: every patch matched and agrees (%u/%u of %zu)", body.agreeing, body.matched, st.cap.cur.size()));
        check(body.straddle && body.shell, "station: the body's boxes reach the eye plane -- the straddle the fault needs");
        check(b.gpu[0][12] < 0 && b.gpu[0][14] > kVW && b.gpu[0][16] == 0.0f && b.gpu[0][19] > 0.0f,
              "station: the volume is still the whole eye from depth 0 (the fault's), and the record now carries a shell");
        // the centre in the shader's view space: the head-axes centre with z flipped, to float32 (1 m at 1.7e7)
        const double Cs[3] = {st.cap.C[0], st.cap.C[1], -st.cap.C[2]};
        const double rc[3] = {b.gpu[0][20], b.gpu[0][21], b.gpu[0][22]};
        check(cel::dist3(Cs, rc) < 4.0, fmt("station: the shell's centre is the body's, in the shader's view space (off by %.2f m)", cel::dist3(Cs, rc)));
        check(b.gpu[0][23] <= st.cap.R - 9000.0 && b.gpu[0][19] >= st.cap.R + 9000.0,
              fmt("station: the shell [%.0f, %.0f] holds the surface +-9 km of terrain about R = %.0f", b.gpu[0][23], b.gpu[0][19], st.cap.R));
        check(b.gpu[0][19] < 1.2 * st.cap.R && b.gpu[0][23] > 0.5 * st.cap.R, "station: and is a shell, not the sky (it is read from the boxes, within 20% of the radius)");
    }
    // CPU twin: sweeps of depth over the whole eye, the world path (z above the split)
    {
        const cel::BuildResult& b = c.build;
        const float* rec = b.gpu[0];
        size_t claimedNear = 0, probes = 0, claimedSurface = 0, surfaces = 0;
        for (int y = 0; y < kVH; y += 3)
            for (int x = 0; x < kVW; x += 3) {
                for (double z : {25.0, 30.0, 100.0, 1000.0, 1.0e5, 1.0e6}) {
                    double mx, my;
                    const double sd = sphereDepth(st.tan, x, y, st.cap.C, st.cap.R);
                    if (sd > 0.0 && z >= 0.5 * sd) continue;   // only what is well in front of the surface (a grazing ray is near it far out)
                    ++probes;
                    if (cel::shaderMotion(rec, c.in.tan, c.inPrev.tan, kVW, kVH, x, y, z, &mx, &my)) ++claimedNear;
                }
                const double sd = sphereDepth(st.tan, x, y, st.cap.C, st.cap.R);
                if (sd > 0.0) {
                    double mx, my;
                    ++surfaces;
                    if (cel::shaderMotion(rec, c.in.tan, c.inPrev.tan, kVW, kVH, x, y, sd, &mx, &my)) ++claimedSurface;
                }
            }
        check(probes > 100000 && claimedNear == 0, fmt("station: no world pixel from 25 m to 1,000 km, or nearer than half the surface's depth, takes the body's motion (%zu of %zu did)", claimedNear, probes));
        check(surfaces > 1500 && claimedSurface * 100 >= surfaces * 99, fmt("station: the body's own surface takes it (%zu of %zu)", claimedSurface, surfaces));
        // the same pixel, the hangar wall and the planet's surface, by name
        const int px = 440, py = 230;
        const double sd = sphereDepth(st.tan, px, py, st.cap.C, st.cap.R);
        double mx, my;
        check(sd > 3.0e6 && sd < 8.0e6, fmt("station: pixel (%d,%d) sees the planet's surface at depth %.4g m", px, py, sd));
        check(!cel::shaderMotion(rec, c.in.tan, c.inPrev.tan, kVW, kVH, px, py, 30.0, &mx, &my), "station: the same pixel at 30 m does not take the body's motion");
        check(cel::shaderMotion(rec, c.in.tan, c.inPrev.tan, kVW, kVH, px, py, sd, &mx, &my), "station: and at the surface's depth it does");
    }
    // the production shader on WARP
    {
        const Outputs A = dispatch(R, v[0].cs.Get(), c.on, c.withRecords, true);
        const Outputs Bo = dispatch(R, v[1].cs.Get(), c.off, c.without, true);
        check(firstDiff(A, Bo).slot < 0, "station: with no records the shader is byte for byte the shader without the path");
        const Outputs Bb = dispatch(R, v[1].cs.Get(), c.off, c.withRecords, true);
        const Outputs C = dispatch(R, v[1].cs.Get(), c.on, c.withRecords, true);
        const Verdict vd = judge(c, A, Bb, C);
        check(vd.ok(), fmt("station: %s", vd.first.c_str()));
        check(vd.expected > 1000, fmt("station: the surface's pixels take path 12 (%zu)", vd.expected));
        // by the GPU's own paths, independent of the twin: each block's interior, the surface's interior
        size_t wallPixels = 0, heldBack = 0;
        for (size_t b = 0; b < sc.blocks.size(); ++b) {
            const std::vector<size_t> in = interiorOf(sc, static_cast<int>(b) + 2);
            const Counts n = claimedOf(A, C, in);
            check(!in.empty(), fmt("station: block '%s' has interior pixels", sc.blocks[b].name));
            check(n.claimed == 0 && n.changed == 0,
                  fmt("station: '%s': %zu of %zu interior pixels took path 12 and %zu differ from the camera term's answer; none may", sc.blocks[b].name, n.claimed, n.total, n.changed));
            for (size_t i : in) if (pathOf(A, i) == 2u) ++heldBack;
            if (b < 3) wallPixels += n.total;
        }
        const std::vector<size_t> surf = interiorOf(sc, 1);
        const Counts ns = claimedOf(A, C, surf);
        check(ns.total > 1000 && ns.claimed * 1000 >= ns.total * 999, fmt("station: the planet's interior pixels take path 12 (%zu of %zu)", ns.claimed, ns.total));
        check(wallPixels > 5000 && heldBack > 5000, fmt("station: the hangar blocks are world-path pixels the old record claimed (%zu wall pixels, %zu on path 2 in the reference)", wallPixels, heldBack));
        std::printf("(f) station: %zu planet pixels (interior %zu, %zu on path 12), %zu hangar-wall pixels at 30-300 m and the things in front of and inside the planet all keep the "
                    "camera term (path and motion identical to the shader without the path)\n", static_cast<size_t>(sc.spherePixels), ns.total, ns.claimed, wallPixels);
        // the diagnostic variant's own count agrees
        const Outputs D = dispatch(R, v[3].cs.Get(), c.on, c.withRecords, false);
        uint32_t counted;
        std::memcpy(&counted, &D.v[kSlotStats][39 * 4], 4);
        check(counted == vd.expected, fmt("station: Stats[39] counts the path-12 pixels (%u against %zu)", counted, vd.expected));
        // MUTATION CONTROL 1 -- the fault restored: the record as the old build wrote it (a whole-eye volume from depth 0 and no shell).
        // The same judge, with the shell's expectations, must refuse the run.
        {
            Case legacy = c;
            setShell(R, legacy, 0.0f, 0.0f);   // legacy's records only; the judge below keeps c's (the shell's) expectations
            const Outputs Lc = dispatch(R, v[1].cs.Get(), legacy.on, legacy.withRecords, true);
            const Verdict lv = judge(c, A, Bb, Lc);
            check(!lv.ok(), "station: control -- the record without its shell (the old whole-eye claim) is caught by the judge");
            const std::vector<size_t> wall = interiorOf(sc, 2);
            const Counts wn = claimedOf(A, Lc, wall);
            // The body's [R|t] on a point 30 m from the eye is the body's translation, thousands of metres: it lands behind last frame's eye, there
            // is no valid projection (path 0), and the pixel keeps no history at all -- the grain the Quest 3 log showed on the hangar wall.
            check(wn.changed == wn.total && wn.total > 1000 && wn.lost * 10 >= wn.total * 9,
                  fmt("station: control -- with the shell stripped the 30 m wall takes the body's motion (%zu of %zu changed, %zu of them with no valid history)", wn.changed, wn.total, wn.lost));
            std::printf("    control 'the old record, zmin 0 and no shell': caught -- %s; %zu of %zu wall pixels take the body's motion and %zu lose their history (the hangar fault)\n",
                        lv.first.c_str(), wn.changed, wn.total, wn.lost);
        }
        // MUTATION CONTROLS 2-6 -- one token of the shell test broken each, in the production text
        {
            const Mutant ms[] = {
                {"the shell test dropped", "if (r.span.w > 0.0) {", "if (r.span.w > 1.0e38) {"},
                {"the outer radius ignored", "radial >= r.ctr.w - slack && radial <= r.span.w + slack", "radial >= r.ctr.w - slack"},
                {"the inner radius ignored", "radial >= r.ctr.w - slack && radial <= r.span.w + slack", "radial <= r.span.w + slack"},
                {"the centre's sign flipped", "length(P - r.ctr.xyz)", "length(P + r.ctr.xyz)"},
                {"the centre dropped (the distance from the eye)", "length(P - r.ctr.xyz)", "length(P)"},
            };
            controls(R, {&c}, {&A}, ms, sizeof(ms) / sizeof(ms[0]));
        }
    }
    // ============================== 3. float32 ==============================
    {
        // On the CPU: the shader's float32 arithmetic against exact arithmetic on the same float inputs, over every pixel of the planet.
        const float ctr[3] = {c.build.gpu[0][20], c.build.gpu[0][21], c.build.gpu[0][22]};
        double worstF = 0.0, sumF = 0.0, worstCtr = 0.0;
        size_t n = 0;
        for (int y = 0; y < kVH; ++y)
            for (int x = 0; x < kVW; ++x) {
                const size_t i = static_cast<size_t>(y) * kVW + x;
                if (sc.label[i] != 1) continue;
                const Radial r = radialOf(c.in.tan, x, y, c.sc.z[i], ctr);
                const double e = std::fabs(r.f - r.exact);
                worstF = std::max(worstF, e);
                sumF += e;
                ++n;
            }
        {
            const double Cs[3] = {st.cap.C[0], st.cap.C[1], -st.cap.C[2]};
            const double cf[3] = {ctr[0], ctr[1], ctr[2]};
            worstCtr = cel::dist3(Cs, cf);
        }
        // and the 30 m wall's subtraction, which is where P is tiny and the centre huge
        double wallWorst = 0.0;
        for (int y = 90; y < 220; y += 7)
            for (int x = 40; x < 140; x += 7) {
                const Radial r = radialOf(c.in.tan, x, y, kDepthB / 30.0f, ctr);
                wallWorst = std::max(wallWorst, std::fabs(r.f - r.exact));
            }
        const double margin = st.cap.R * 0.01 + 4.0;
        check(n > 1500 && worstF < 8.0 && wallWorst < 8.0 && worstCtr < 4.0,
              fmt("float32: |P - centre| at 1.7e7 m is good to metres: planet pixels worst %.2f m, 30 m wall worst %.2f m, the centre's own rounding %.2f m",
                  worstF, wallWorst, worstCtr));
        check(worstF + worstCtr < 1.0e-3 * margin, fmt("float32: the error (%.1f m) is a tenth of a percent of the shell's margin (%.0f m)", worstF + worstCtr, margin));
        std::printf("(f) float32 at |centre| = %.4g m: |P - centre| off by %.2f m worst (mean %.2f m) over %zu planet pixels and %.2f m on a 30 m wall; the centre itself %.2f m; "
                    "ulp(1.7e7) = 1 m; the shell's margin is %.0f m (1%% of R + 4 m + 6 ulp)\n",
                    cel::len3(st.cap.C), worstF, sumF / double(n), n, wallWorst, worstCtr, margin);
        // On WARP: the production arithmetic with the slack taken out (a one-token build of the same text), a gradient of depths across the
        // planet's pixels, the shell's outer radius set at the median: the pixels the shader gets wrong are those within its error of the median.
        std::string text = edvr::kTemporalCsHlsl;
        const std::string anchor = "float slack = 3.0 * z * max((tanNow.y - tanNow.x) / float(size.x), (tanNow.w - tanNow.z) / float(size.y));";
        check(text.find(anchor) != std::string::npos && text.find(anchor, text.find(anchor) + 1) == std::string::npos, "float32: the slack's line is in the shader exactly once");
        text.replace(text.find(anchor), anchor.size(), "float slack = 0.0;");
        const D3D_SHADER_MACRO macros[] = {{"EDVR_TEMPORAL_DIAGNOSTICS", "1"}, {"EDVR_TEMPORAL_TRACE", "1"}, {"EDVR_CELESTIAL", "1"}, {nullptr, nullptr}};
        ComPtr<ID3DBlob> code, errors;
        const HRESULT hrc = D3DCompile(text.data(), text.size(), "noslack", macros, nullptr, "mv", "cs_5_0", 0, 0, code.GetAddressOf(), errors.GetAddressOf());
        check(SUCCEEDED(hrc), "float32: the slack-free build compiles");
        ComPtr<ID3D11ComputeShader> cs;
        hr(R.dev->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &cs), "CreateComputeShader (noslack)");
        // a gradient: the planet's pixels from x = 400, each pushed along its ray by (x - 450) * 0.8 + (y - 244) * 0.15 m
        SynthCase gr;
        makeSynth(R, st, {}, gr);   // the planet's depth, then the gradient on top, pixel by pixel
        for (int y = 20; y < 470; ++y)
            for (int x = 400; x < 500; ++x) {
                const size_t i = static_cast<size_t>(y) * kVW + x;
                if (gr.label[i] != 1) continue;
                const double z0 = kDepthB / gr.c.sc.z[i];
                gr.c.sc.z[i] = kDepthB / static_cast<float>(z0 + (x - 450) * 0.8 + (y - 244) * 0.15);
            }
        gr.c.sc.srvZ = texSrv(R, DXGI_FORMAT_R32_FLOAT, 4, gr.c.sc.z.data());
        gr.c.withRecords[2] = gr.c.without[2] = gr.c.sc.srvZ.Get();
        // the exact radial of each gradient pixel and the median
        std::vector<double> radials;
        for (int y = 20; y < 470; ++y)
            for (int x = 400; x < 500; ++x) {
                const size_t i = static_cast<size_t>(y) * kVW + x;
                if (gr.label[i] != 1) continue;
                // the shader's depth is the nearest of the 3x3
                float zr = 0.0f;
                for (int oy = -1; oy <= 1; ++oy)
                    for (int ox = -1; ox <= 1; ++ox) zr = std::max(zr, gr.c.sc.z[static_cast<size_t>(y + oy) * kVW + x + ox]);
                radials.push_back(radialOf(c.in.tan, x, y, zr, ctr).exact);
            }
        std::sort(radials.begin(), radials.end());
        check(radials.size() > 5000, "float32: the gradient has pixels");
        const double lo = radials.front(), hi = radials.back(), median = radials[radials.size() / 2];
        check(hi - lo > 40.0, fmt("float32: the gradient spans %.1f m of radial distance", hi - lo));
        setShell(R, gr.c, 0.0f, static_cast<float>(median));
        const Outputs G = dispatch(R, cs.Get(), gr.c.on, gr.c.withRecords, true);
        double wrongBand = 0.0;
        size_t wrong = 0, total = 0;
        for (int y = 20; y < 470; ++y)
            for (int x = 400; x < 500; ++x) {
                const size_t i = static_cast<size_t>(y) * kVW + x;
                if (gr.label[i] != 1) continue;
                float zr = 0.0f;
                for (int oy = -1; oy <= 1; ++oy)
                    for (int ox = -1; ox <= 1; ++ox) zr = std::max(zr, gr.c.sc.z[static_cast<size_t>(y + oy) * kVW + x + ox]);
                const Radial r = radialOf(c.in.tan, x, y, zr, ctr);
                const bool claimed = pathOf(G, i) == 12u;
                const bool shouldBe = r.exact <= static_cast<double>(static_cast<float>(median));
                ++total;
                if (claimed != shouldBe) { ++wrong; wrongBand = std::max(wrongBand, std::fabs(r.exact - median)); }
            }
        check(wrongBand < 8.0, fmt("float32 on WARP: a pixel more than 8 m on the wrong side of the shell's edge was misjudged (band %.2f m, %zu of %zu)", wrongBand, wrong, total));
        std::printf("(f) float32 on WARP, slack out, outer radius at the median of %zu pixels spanning %.0f m: the pixels judged on the wrong side are all within %.2f m of the edge "
                    "(%zu of %zu)\n", total, hi - lo, wrongBand, wrong, total);
    }
    // ============================== 2. the landed ship ==============================
    {
        Scenario ld = landed();
        SynthCase lc;
        std::vector<Block> lb = {{"a wall 40 m off, straight ahead, above the ground", 200, 20, 300, 120, 40.0, 0, false}};
        makeSynth(R, ld, lb, lc);
        Case& c2 = lc.c;
        const cel::BuildResult& b = c2.build;
        check(b.bodies == 1 && b.records == 1 && b.body[0].ok, fmt("landed: the body builds a record (bodies %u records %u fallback %s)", b.bodies, b.records, cel::fallbackName(b.body[0].fallback)));
        check(b.body[0].straddle && b.gpu[0][19] > 0.0f, "landed: the patches under the ship reach behind the eye plane: a straddling body, with a shell");
        check(b.gpu[0][23] <= ld.cap.R - 3.0 && b.gpu[0][19] >= ld.cap.R + 3.0, fmt("landed: the shell [%.0f, %.0f] holds the ground (R = %.0f)", b.gpu[0][23], b.gpu[0][19], ld.cap.R));
        const Outputs A = dispatch(R, v[0].cs.Get(), c2.on, c2.withRecords, true);
        const Outputs Bo = dispatch(R, v[1].cs.Get(), c2.off, c2.without, true);
        check(firstDiff(A, Bo).slot < 0, "landed: with no records the shader is byte for byte the shader without the path");
        const Outputs Bb = dispatch(R, v[1].cs.Get(), c2.off, c2.withRecords, true);
        const Outputs C = dispatch(R, v[1].cs.Get(), c2.on, c2.withRecords, true);
        const Verdict vd = judge(c2, A, Bb, C);
        check(vd.ok(), fmt("landed: %s", vd.first.c_str()));
        const std::vector<size_t> ground = interiorOf(lc, 1);
        const Counts n = claimedOf(A, C, ground);
        check(ground.size() > 5000 && n.claimed * 1000 >= n.total * 999, fmt("landed: the ground (%zu interior pixels from 2 m to the horizon) takes path 12: %zu", n.total, n.claimed));
        // the ground in the nearest 30 m, by itself, and the world wall at 40 m (a structure in front of the planet is NOT the ground's: 40 m
        // above the ground is inside the shell's percent, so it keeps the planet's motion as it did before the shell -- stated, not required)
        size_t nearGround = 0, nearClaimed = 0;
        for (size_t i : ground) {
            const double z = kDepthB / c2.sc.z[i];
            if (z > ld.split && z < 30.0) { ++nearGround; if (pathOf(C, i) == 12u) ++nearClaimed; }
        }
        check(nearGround > 1000 && nearClaimed == nearGround, fmt("landed: the ground within 30 m takes path 12 (%zu of %zu)", nearClaimed, nearGround));
        // the same body seen from the station's far side of the world would not have claimed this ground: the shell is the planet's
        double mx, my;
        check(!cel::shaderMotion(b.gpu[0], c2.in.tan, c2.inPrev.tan, kVW, kVH, 250, 300, 1.0e6, &mx, &my), "landed: a point 1,000 km out along a downward ray is inside the planet, not the ground");
        std::printf("(f) landed, the eye 2 m above the surface: %zu ground pixels from 2 m to the horizon, %zu on path 12 (%zu within 30 m, all); shell [%.0f, %.0f] m about R = %.0f m\n",
                    n.total, n.claimed, nearGround, static_cast<double>(b.gpu[0][23]), static_cast<double>(b.gpu[0][19]), ld.cap.R);
    }
}

}  // namespace shell
