#include "celestial_motion.h"
#include "binding_shadow.h"
#include "depth_probe.h"
#include "../common/celestial_math.h"
#include "../common/guard.h"
#include "../common/log.h"
#include <wrl/client.h>
#include <cstdio>
#include <cstring>
#include <windows.h>

namespace edvr {

namespace detail {
bool g_celestialLive = false;
bool g_celestialAnyWatched = false;
}  // namespace detail

namespace {
using Microsoft::WRL::ComPtr;
namespace cel = edvr::celestial;

bool g_enabled = false, g_failed = false;
FaultBudget g_budget("celestial motion", 3);

// The supercruise gate (celestial_motion.h): what Status.json said at the last celestialMotionNoteStatus. Unset until the first
// one arrives, which counts as off. Render thread writes it; setLive (any thread, from Configure) only reads it.
enum Gate : uint8_t { kGateUnset = 0, kGateOn, kGateOffNormal, kGateOffUnknown };
Gate g_gate = kGateUnset;
constexpr uint32_t kMaxGateLines = 32;
uint32_t g_gateLines = 0;
bool g_resetPending = false;   // Configure (any thread) changed the module's own switch: the render thread drops the old frames' state
const char* gateName(Gate g) {
    switch (g) {
        case kGateOn: return "on";
        case kGateOffNormal: return "off (not supercruise)";
        case kGateOffUnknown: return "off (status unknown)";
        default: return "off (no status yet)";
    }
}
void setLive() { detail::g_celestialLive = g_enabled && !g_failed && g_gate == kGateOn; }

uint64_t qpc() {
    LARGE_INTEGER v{};
    QueryPerformanceCounter(&v);
    return static_cast<uint64_t>(v.QuadPart);
}
double usPerTick() {
    static const double f = [] {
        LARGE_INTEGER q{};
        QueryPerformanceFrequency(&q);
        return q.QuadPart > 0 ? 1e6 / static_cast<double>(q.QuadPart) : 1.0;
    }();
    return f;
}

// ---------------------------------------------------------------------------------------------------------------
// The watched buffers and their shadows
// ---------------------------------------------------------------------------------------------------------------
constexpr uint32_t kWatch = 128, kProbe = 8;
enum Role : uint8_t { kRoleNone = 0, kRoleB0 = 1, kRoleB2 = 2 };
struct Segment { uint32_t offset, length, minBytes; };
constexpr Segment kSegment[3] = {{0, 0, 0}, {cel::kB0Offset, cel::kB0Bytes, cel::kB0MinBytes}, {0, cel::kB2Bytes, cel::kB2MinBytes}};

// Identity only: no reference held (the game may destroy the buffer), compared and never dereferenced outside the
// draw's own verification. `data` is the segment of the role, from offset 0.
struct Watch {
    ID3D11Resource* res = nullptr;
    uint8_t role = kRoleNone;
    bool valid = false;
    uint32_t bytes = 0;       // ByteWidth, 0 = verify at the next draw
    uint32_t lastDraw = 0;    // the stamp of the last draw that used it: the eviction order
    void* mapped = nullptr;
    uint8_t data[cel::kB2Bytes];
};
Watch g_watch[kWatch];
uint32_t g_watched = 0;

uint32_t slotOf(const void* p) {
    return ((static_cast<uint32_t>(reinterpret_cast<uintptr_t>(p) >> 4) * 2654435761u) >> 7) & (kWatch - 1);
}
Watch* findWatch(const void* res) {
    uint32_t i = slotOf(res);
    for (uint32_t k = 0; k < kProbe; ++k) {
        Watch& w = g_watch[(i + k) & (kWatch - 1)];
        if (w.res == res) return &w;
        if (!w.res) return nullptr;   // slots are only ever replaced, never emptied: the run ends at the first hole
    }
    return nullptr;
}
Watch* addWatch(ID3D11Resource* res, uint8_t role, uint32_t stamp) {
    uint32_t i = slotOf(res);
    Watch* victim = nullptr;
    for (uint32_t k = 0; k < kProbe; ++k) {
        Watch& w = g_watch[(i + k) & (kWatch - 1)];
        if (!w.res) { victim = &w; break; }
        if (!victim || w.lastDraw < victim->lastDraw) victim = &w;
    }
    if (!victim->res) ++g_watched;
    victim->res = res;
    victim->role = role;
    victim->valid = false;
    victim->bytes = 0;
    victim->lastDraw = stamp;
    victim->mapped = nullptr;
    detail::g_celestialAnyWatched = true;
    return victim;
}

// ---------------------------------------------------------------------------------------------------------------
// Counters: one 5 s window, and the session's totals
// ---------------------------------------------------------------------------------------------------------------
enum Decline : uint32_t { kDecOffEye, kDecUnwatched, kDecNoB0, kDecNoB2, kDecSize, kDecConstants, kDecDuplicate, kDecCap, kDecCount };
const char* const kDeclineName[kDecCount] = {"off-eye", "unwatched", "no-b0", "no-b2", "size", "constants", "duplicate", "cap"};
struct Counters {
    uint64_t frames = 0, draws = 0, captured = 0;
    uint64_t declined[kDecCount] = {};
    uint64_t consumerCalls = 0, eyeFrames = 0, patches = 0, matched = 0, unmatched = 0, bodies = 0, behind = 0, offscreen = 0, records = 0, uploads = 0;
    uint64_t gatedFrames = 0;   // boundaries with the supercruise gate closed: no capture, no records
    uint64_t shellRecords = 0;  // records of a body whose boxes reach the eye plane: the shell holds their pixels
    uint64_t fallbacks[cel::kFbCount] = {};
    double maxDisplacement = 0.0;
    uint64_t pixels = 0;
    bool pixelsKnown = false;
    uint64_t teeMap = 0, teeUpdate = 0, teeInvalid = 0, teeCopies = 0, teeTicks = 0;
    uint64_t captureTicks = 0, buildTicks = 0;
};
Counters g_win;
bool g_noted[8] = {};   // 0 patch draws read, 1 first record; then the decline reasons are kept in g_declineNoted

// ---------------------------------------------------------------------------------------------------------------
// Per eye: the patch lists of this frame and the one before, the records built from them, the GPU buffer
// ---------------------------------------------------------------------------------------------------------------
constexpr uint32_t kNever = 0xFFFFFFFFu;
struct EyeState {
    cel::Patch patches[2][cel::kMaxPatches];
    uint32_t n[2] = {0, 0};
    uint32_t stamp[2] = {kNever, kNever};
    int cur = 0;
    uint32_t builtStamp = kNever;
    int builtW = 0, builtH = 0;
    float builtTan[4] = {};
    cel::BuildResult result;
    ComPtr<ID3D11Buffer> buffer;
    ComPtr<ID3D11ShaderResourceView> srv;
    float uploaded[cel::kMaxBodies * cel::kRecordFloats];
    bool uploadedValid = false;
};
EyeState g_eye[2];
cel::Scratch g_scratch;
uint32_t g_stamp = 0;               // the boundary count; a frame's draws and its consumer share one
uint64_t g_lastReport = 0;
bool g_declineNoted[kDecCount] = {};
bool g_fallbackNoted[cel::kFbCount] = {};
constexpr uint32_t kMaxBodyNames = 16;   // distinct radii (to a km) named in the log per session
long long g_bodyKm[kMaxBodyNames] = {};
uint32_t g_bodiesNamed = 0;

// What was captured does not cross a change of the gate (or of the module's own switch): both eyes' lists and stamps, so the next
// frame's first patch finds no previous frame. The record buffers stay; they are not bound while the gate is closed.
void dropCaptured() {
    for (EyeState& e : g_eye) {
        e.uploadedValid = false;
        e.n[0] = e.n[1] = 0;
        e.stamp[0] = e.stamp[1] = kNever;
        e.cur = 0;
        e.builtStamp = kNever;
    }
}
// The watched buffers and their shadows, emptied: the write tees' first test (celestialMotionAnyWatched) goes false and every
// Map/Unmap the game makes passes them by with one load.
void clearWatches() {
    for (Watch& w : g_watch) { w.res = nullptr; w.role = kRoleNone; w.valid = false; w.bytes = 0; w.mapped = nullptr; }
    g_watched = 0;
    detail::g_celestialAnyWatched = false;
}

void fail(const char* why) {
    if (!g_failed) Log::get().note("celestial motion: stood down (%s); planets keep the camera's motion.", why);
    g_failed = true;
    setLive();
}

// ---------------------------------------------------------------------------------------------------------------
// The draw: constants from the shadow into the eye's patch list
// ---------------------------------------------------------------------------------------------------------------
// The shadow of a draw's buffer for a role, or why not.
// The size is read here, off the buffer the draw really has bound (a constant buffer slot holds a buffer, so GetDesc is safe), and
// before the first write the tee will copy: the tee never calls GetDesc on a resource it was merely handed, and copies a segment
// only out of a mapping at least that long.
void verifySize(Watch* w, ID3D11Buffer* buffer) {
    D3D11_BUFFER_DESC bd{};
    buffer->GetDesc(&bd);
    w->bytes = bd.ByteWidth ? bd.ByteWidth : 1u;
}
Watch* shadowFor(ID3D11Buffer* buffer, uint8_t role, uint32_t stamp, Decline noShadow, Decline* why) {
    if (!buffer) { *why = noShadow; return nullptr; }
    Watch* w = findWatch(buffer);
    if (!w) {
        w = addWatch(buffer, role, stamp);   // the writes from here on are tee'd; this draw's cannot have been
        verifySize(w, buffer);
        *why = kDecUnwatched;
        return nullptr;
    }
    w->lastDraw = stamp;
    if (w->role != role) { *why = kDecSize; return nullptr; }
    if (w->bytes == 0) {
        verifySize(w, buffer);      // an address re-created since the last draw (the CreateBuffer hook cleared it)
        w->valid = false;
    }
    if (w->bytes < kSegment[role].minBytes) { *why = kDecSize; return nullptr; }
    if (!w->valid) { *why = noShadow; return nullptr; }
    return w;
}

void noteDeclineOnce(Decline why, const void* b0, const void* b2) {
    if (g_declineNoted[why]) return;
    g_declineNoted[why] = true;
    Log::get().note("celestial motion: a planet patch draw was not read (%s; VS b0 %p, VS b2 %p). The camera term stands for it; "
                    "the 5 s line counts these. Said once per reason.", kDeclineName[why], b0, b2);
}

void noteDrawImpl(ID3D11DeviceContext* ctx) {
    const uint64_t t0 = qpc();
    ++g_win.draws;
    auto* dsv = static_cast<ID3D11DepthStencilView*>(bindingGet(BindSlot::Dsv0));
    int eye = -1, target = -1;
    if (!dsv || !depthProbeCurrentSceneEyeOf(dsv, &eye, &target) || (eye != 0 && eye != 1)) {
        ++g_win.declined[kDecOffEye];
        g_win.captureTicks += qpc() - t0;
        return;
    }
    ID3D11Buffer* cb[3] = {};
    ctx->VSGetConstantBuffers(0, 3, cb);
    Decline why = kDecCount;
    Watch* w0 = shadowFor(cb[0], kRoleB0, g_stamp, kDecNoB0, &why);
    Decline why2 = kDecCount;
    Watch* w2 = shadowFor(cb[2], kRoleB2, g_stamp, kDecNoB2, &why2);
    if (!w0 || !w2) {
        const Decline d = !w0 ? why : why2;
        ++g_win.declined[d];
        noteDeclineOnce(d, cb[0], cb[2]);
    } else {
        cel::Patch p;
        const cel::Capture st = cel::patchFromBlocks(w2->data, w0->data, p);
        if (st != cel::kCaptureOk) {
            ++g_win.declined[kDecConstants];
            noteDeclineOnce(kDecConstants, cb[0], cb[2]);
        } else {
            EyeState& e = g_eye[eye];
            if (e.stamp[e.cur] != g_stamp) {
                // The eye's first patch this frame: what was captured becomes the previous frame's, whatever the boundary
                // did in between, so each eye pairs its own frames by the stamp.
                if (e.stamp[e.cur] != kNever) {
                    e.cur ^= 1;
                    e.n[e.cur] = 0;
                }
                e.stamp[e.cur] = g_stamp;
                e.n[e.cur] = 0;
            }
            cel::Patch* list = e.patches[e.cur];
            uint32_t& n = e.n[e.cur];
            bool duplicate = false;
            for (uint32_t i = 0; i < n && !duplicate; ++i)
                duplicate = list[i].hash == p.hash && std::memcmp(list[i].c, p.c, 12) == 0 && std::memcmp(list[i].q, p.q, 16) == 0 &&
                            std::memcmp(list[i].rows, p.rows, 64) == 0;
            if (duplicate) {
                ++g_win.declined[kDecDuplicate];
            } else if (n >= cel::kMaxPatches) {
                ++g_win.declined[kDecCap];
                noteDeclineOnce(kDecCap, cb[0], cb[2]);
            } else {
                list[n++] = p;
                ++g_win.captured;
                if (!g_noted[0]) {
                    g_noted[0] = true;
                    Log::get().note("celestial motion: planet patch draws are being read from the CPU shadow (VS b2 %u bytes, VS b0 rows 9-11): "
                                    "eye %d, frame %u, first patch at %.0f m, body radius %.0f m. A 'celestial motion 5s:' line follows each 5 s.",
                                    w2->bytes, eye, g_stamp, cel::distanceOf(p), static_cast<double>(p.body[3]));
                }
            }
        }
    }
    for (auto* b : cb) if (b) b->Release();
    g_win.captureTicks += qpc() - t0;
}

// ---------------------------------------------------------------------------------------------------------------
// The consumer: build the eye's records and upload them
// ---------------------------------------------------------------------------------------------------------------
bool ensureGpu(ID3D11DeviceContext* ctx, EyeState& e) {
    if (e.buffer && e.srv) return true;
    ComPtr<ID3D11Device> dev;
    ctx->GetDevice(&dev);
    if (!dev) return false;
    constexpr UINT stride = cel::kRecordFloats * 4;
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = stride * cel::kMaxBodies;
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    bd.StructureByteStride = stride;
    static const float zeros[cel::kMaxBodies * cel::kRecordFloats] = {};
    D3D11_SUBRESOURCE_DATA init{zeros, 0, 0};
    e.buffer.Reset();
    e.srv.Reset();
    if (FAILED(dev->CreateBuffer(&bd, &init, &e.buffer)) || FAILED(dev->CreateShaderResourceView(e.buffer.Get(), nullptr, &e.srv))) {
        e.buffer.Reset();
        e.srv.Reset();
        return false;
    }
    e.uploadedValid = false;
    return true;
}

void noteFallbacks(int eye, const EyeState& e) {
    for (uint32_t b = 0; b < e.result.bodies; ++b) {
        const cel::BodyResult& r = e.result.body[b];
        if (r.ok || r.skip != cel::kSkipNone || g_fallbackNoted[r.fallback]) continue;
        g_fallbackNoted[r.fallback] = true;
        Log::get().note("celestial motion: first body without a record for '%s' (eye %d, frame %u): radius %.0f m, %.0f m away, %u patch(es), "
                        "%u matched. The camera term stands for it; the 5 s line counts these.",
                        cel::fallbackName(r.fallback), eye, g_stamp, static_cast<double>(r.radius), r.nearest, r.patches, r.matched);
    }
}

// Each distinct body radius (to a km) the patch draws show, named once per session, 16 at most: how far its nearest patch is, whether its
// boxes reach the eye plane (a straddling body's pixels are held to its radial shell), and what the build did with it. Only the first body of
// a session used to be named; this answers which bodies -- landable, non-landable, gas giants -- the patch shader draws.
void noteBodies(int eye, const EyeState& e) {
    const cel::BuildResult& r = e.result;
    for (uint32_t b = 0; b < r.bodies && b < cel::kMaxGroups; ++b) {
        const cel::BodyResult& body = r.body[b];
        const long long km = std::llround(static_cast<double>(body.radius) / 1000.0);
        bool seen = false;
        for (uint32_t i = 0; i < g_bodiesNamed && !seen; ++i) seen = g_bodyKm[i] == km;
        if (seen) continue;
        if (g_bodiesNamed >= kMaxBodyNames) return;
        g_bodyKm[g_bodiesNamed++] = km;
        char outcome[96];
        if (body.ok) std::snprintf(outcome, sizeof outcome, "record bound, delta %.1f m", body.displacement);
        else if (body.skip == cel::kSkipBehind) std::snprintf(outcome, sizeof outcome, "wholly behind the eye, no record");
        else if (body.skip == cel::kSkipOffscreen) std::snprintf(outcome, sizeof outcome, "off the eye's pixels, no record");
        else std::snprintf(outcome, sizeof outcome, "no record (%s)", cel::fallbackName(body.fallback));
        Log::get().note("celestial motion: body %u of %u named: radius %lld km, nearest patch %.0f km away, %u patch(es), %s; %s (eye %d, frame %u). "
                        "Said once per radius, to a km, 16 at most.%s",
                        g_bodiesNamed, kMaxBodyNames, km, body.nearest / 1000.0, body.patches,
                        body.skip == cel::kSkipBehind ? "straddles the eye plane: n/a, wholly behind it"
                                                      : (body.straddle ? "straddles the eye plane: yes, its pixels are held to the radial shell"
                                                                       : "straddles the eye plane: no"),
                        outcome, eye, g_stamp, g_bodiesNamed == kMaxBodyNames ? " The 16th: no more radii are named." : "");
    }
}

bool recordsImpl(ID3D11DeviceContext* ctx, int eye, const float tanNow[4], int w, int h, CelestialEyeRecords* out) {
    ++g_win.consumerCalls;
    EyeState& e = g_eye[eye];
    const int cur = e.cur;
    if (e.stamp[cur] != g_stamp || e.n[cur] == 0) return false;   // nothing drawn for this eye this frame
    const uint64_t t0 = qpc();
    if (e.builtStamp != g_stamp || e.builtW != w || e.builtH != h || std::memcmp(e.builtTan, tanNow, 16) != 0) {
        const int prevIdx = cur ^ 1;
        const bool havePrev = e.n[prevIdx] > 0 && e.stamp[prevIdx] != kNever && e.stamp[prevIdx] + 1 == e.stamp[cur];
        cel::EyeInput in{};
        std::memcpy(in.tan, tanNow, 16);
        in.w = w;
        in.h = h;
        cel::build(e.patches[cur], e.n[cur], havePrev ? e.patches[prevIdx] : nullptr, havePrev ? e.n[prevIdx] : 0, in, e.result, g_scratch);
        e.builtStamp = g_stamp;
        e.builtW = w;
        e.builtH = h;
        std::memcpy(e.builtTan, tanNow, 16);
        const cel::BuildResult& r = e.result;
        ++g_win.eyeFrames;
        g_win.patches += r.patches;
        g_win.matched += r.matched;
        g_win.unmatched += r.unmatched;
        g_win.bodies += r.bodies;
        g_win.behind += r.behind;
        g_win.offscreen += r.offscreen;
        g_win.records += r.records;
        for (uint32_t k = 0; k < r.records; ++k) g_win.shellRecords += r.body[r.order[k]].shell ? 1u : 0u;
        for (uint32_t k = 0; k < cel::kFbCount; ++k) g_win.fallbacks[k] += r.fallbacks[k];
        if (r.maxDisplacement > g_win.maxDisplacement) g_win.maxDisplacement = r.maxDisplacement;
        noteFallbacks(eye, e);
        noteBodies(eye, e);
        if (r.records && !g_noted[1]) {
            g_noted[1] = true;
            const cel::BodyResult& nb = r.body[r.order[0]];
            Log::get().note("celestial motion: first body record (eye %d, frame %u): %u patch(es) in %u bod%s (%u behind the eye), nearest radius %.0f m at "
                            "%.0f m, delta %.1f m, turn %.5f deg, %u of %u patches agree. Its pixels take decision path 12 (tools\\eye_decisions.py: 'celestial').",
                            eye, g_stamp, r.patches, r.bodies, r.bodies == 1 ? "y" : "ies", r.behind, static_cast<double>(nb.radius), nb.nearest,
                            nb.displacement, nb.rotDeg, nb.agreeing, nb.matched);
        }
        if (r.records) {
            if (!ensureGpu(ctx, e)) {
                fail("the record buffer could not be created");
                g_win.buildTicks += qpc() - t0;
                return false;
            }
            if (!e.uploadedValid || std::memcmp(e.uploaded, r.gpu, sizeof(e.uploaded)) != 0) {
                ctx->UpdateSubresource(e.buffer.Get(), 0, nullptr, r.gpu, 0, 0);
                std::memcpy(e.uploaded, r.gpu, sizeof(e.uploaded));
                e.uploadedValid = true;
                ++g_win.uploads;
            }
        }
    }
    g_win.buildTicks += qpc() - t0;
    const cel::BuildResult& r = e.result;
    out->bodies = r.bodies;
    out->patches = r.patches;
    out->matched = r.matched;
    out->records = r.records;
    if (r.records) {
        const cel::BodyResult& nb = r.body[r.order[0]];
        for (int i = 0; i < 3; ++i) out->translation[i] = nb.t[i];
        out->rotationDeg = nb.rotDeg;
        out->distance = nb.nearest;
        out->srv = e.srv.Get();
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------------------------------------------
// The 5 s census
// ---------------------------------------------------------------------------------------------------------------
void report() {
    const Counters& c = g_win;
    const double frames = c.frames ? static_cast<double>(c.frames) : 1.0;
    const double eyeFrames = c.eyeFrames ? static_cast<double>(c.eyeFrames) : 1.0;
    char pixels[40];
    if (c.pixelsKnown) std::snprintf(pixels, sizeof pixels, "%llu", static_cast<unsigned long long>(c.pixels));
    else std::snprintf(pixels, sizeof pixels, "n/a (diagnostics off)");
    const double captureUs = c.draws ? static_cast<double>(c.captureTicks) * usPerTick() / static_cast<double>(c.draws) : 0.0;
    const double buildUs = c.eyeFrames ? static_cast<double>(c.buildTicks) * usPerTick() / eyeFrames : 0.0;
    const double teeUs = c.teeCopies ? static_cast<double>(c.teeTicks) * usPerTick() / static_cast<double>(c.teeCopies) : 0.0;
    const double totalMs = (static_cast<double>(c.captureTicks) + static_cast<double>(c.buildTicks) + static_cast<double>(c.teeTicks)) * usPerTick() / 1000.0 / frames;
    using ull = unsigned long long;
    // A window the gate held closed throughout has nothing else to say: one short line. A line that never appears means the
    // module never ran; this one says it ran and stood aside (and the transition lines say why).
    if (c.frames > 0 && c.gatedFrames >= c.frames) {
        Log::get().note("celestial motion 5s: frames=%llu gated-off=%llu (supercruise gate: %s); no planet patch capture, no records, no path-12 pixels this window",
                        static_cast<ull>(c.frames), static_cast<ull>(c.gatedFrames), gateName(g_gate));
        return;
    }
    // Three lines, each well under the 1000-character budget at 20-digit counters (the rig prints and pins the worst): the capture, then
    // the consumer's census, then the records, the tee, the cost and the gate. The first keeps the old key and now carries the gate's count.
    Log::get().note(
        "celestial motion 5s: frames=%llu gated-off=%llu draws=%llu captured=%llu declined[off-eye=%llu unwatched=%llu no-b0=%llu no-b2=%llu size=%llu "
        "constants=%llu duplicate=%llu cap=%llu]",
        static_cast<ull>(c.frames), static_cast<ull>(c.gatedFrames), static_cast<ull>(c.draws), static_cast<ull>(c.captured),
        static_cast<ull>(c.declined[kDecOffEye]), static_cast<ull>(c.declined[kDecUnwatched]),
        static_cast<ull>(c.declined[kDecNoB0]), static_cast<ull>(c.declined[kDecNoB2]),
        static_cast<ull>(c.declined[kDecSize]), static_cast<ull>(c.declined[kDecConstants]),
        static_cast<ull>(c.declined[kDecDuplicate]), static_cast<ull>(c.declined[kDecCap]));
    Log::get().note(
        "celestial motion 5s (2/3): consumer=%llu eye-frames=%llu patches/frame=%.1f bodies/frame=%.1f (behind=%llu off-screen=%llu) "
        "matched/frame=%.1f unmatched=%llu; fallback[no-previous-frame=%llu no-previous-body=%llu no-match=%llu implausible=%llu disagree=%llu]",
        static_cast<ull>(c.consumerCalls), static_cast<ull>(c.eyeFrames),
        static_cast<double>(c.patches) / eyeFrames, static_cast<double>(c.bodies) / eyeFrames,
        static_cast<ull>(c.behind), static_cast<ull>(c.offscreen),
        static_cast<double>(c.matched) / eyeFrames, static_cast<ull>(c.unmatched),
        static_cast<ull>(c.fallbacks[cel::kFbNoPrevFrame]), static_cast<ull>(c.fallbacks[cel::kFbNoPrevBody]),
        static_cast<ull>(c.fallbacks[cel::kFbNoMatch]), static_cast<ull>(c.fallbacks[cel::kFbImplausible]),
        static_cast<ull>(c.fallbacks[cel::kFbDisagree]));
    Log::get().note(
        "celestial motion 5s (3/3): records=%llu (%.2f/frame, %llu with a shell) uploads=%llu max|t|=%.1f m/frame pixels=%s; "
        "tee[map=%llu update=%llu invalidated=%llu watched=%u copy=%.2fus]; cpu[capture=%.2fus/draw build=%.1fus/eye-frame total=%.3f ms/frame]; "
        "gate=%s (%llu of %llu frames gated off)",
        static_cast<ull>(c.records), static_cast<double>(c.records) / eyeFrames, static_cast<ull>(c.shellRecords), static_cast<ull>(c.uploads),
        c.maxDisplacement, pixels,
        static_cast<ull>(c.teeMap), static_cast<ull>(c.teeUpdate), static_cast<ull>(c.teeInvalid),
        g_watched, teeUs, captureUs, buildUs, totalMs, gateName(g_gate), static_cast<ull>(c.gatedFrames), static_cast<ull>(c.frames));
}
}  // namespace

// ---------------------------------------------------------------------------------------------------------------
// The public surface
// ---------------------------------------------------------------------------------------------------------------
void celestialMotionConfigure(bool enabled) {
    // Turning the module on or off (the pass came or went) is a gap in the frames like any other: the gate decides afresh at the next
    // status (until then the module is not live), and the render thread drops what was captured before it does. Only flags move here,
    // as before. A repeat of the same setting changes nothing.
    if (g_enabled != enabled) {
        g_enabled = enabled;
        g_gate = kGateUnset;
        g_resetPending = true;
    }
    setLive();
}

void celestialMotionNoteStatus(bool known, bool supercruise) {
    if (!g_enabled) return;
    if (g_resetPending) {
        g_resetPending = false;   // (the 32-line cap on the gate's own lines is the session's: a pass that comes and goes does not reset it)
        dropCaptured();
        clearWatches();
    }
    const Gate want = !known ? kGateOffUnknown : (supercruise ? kGateOn : kGateOffNormal);
    if (want == g_gate) return;
    const Gate was = g_gate;
    g_gate = want;
    // Whatever the direction, what was captured does not cross the change; closing also unwatches every buffer, so the game's
    // thousand Maps a frame pass the tees by with their first test.
    dropCaptured();
    if (want != kGateOn) clearWatches();
    setLive();
    if (g_gateLines >= kMaxGateLines) return;
    ++g_gateLines;
    const char* why = want == kGateOn ? "Status.json Flags says supercruise"
                    : want == kGateOffNormal ? "Status.json Flags says not supercruise"
                                             : "supercruise status unknown: no Flags read from Status.json (menus, a missing file) or the journal watcher is off";
    Log::get().note("celestial motion: gate %s at frame %u, was %s (%s).%s%s",
                    want == kGateOn ? "ON" : "OFF", g_stamp, gateName(was), why,
                    want == kGateOn ? " Planet patch draws are read and bodies take their own motion."
                                    : " No planet patch capture, no records bound, no path-12 pixels; the camera term carries the world.",
                    g_gateLines == kMaxGateLines ? " (the last gate line this session; later changes are not logged)" : "");
}

void celestialMotionNoteDraw(ID3D11DeviceContext* ctx) {
    if (!detail::g_celestialLive || !ctx) return;
    if (!g_budget.shouldRun()) { fail("its fault budget is spent"); return; }
    guardedBudget(g_budget, [&] { noteDrawImpl(ctx); });
}

// Hot: about 1100 Maps a frame. Each tee starts with the table lookup and returns once nothing matches: no GetDesc,
// no logging, no allocation.
void celestialMotionConstantsMapped(ID3D11Resource* resource, void* data) {
    Watch* w = findWatch(resource);
    if (w) w->mapped = data;
}

void celestialMotionConstantsUnmapped(ID3D11Resource* resource) {
    Watch* w = findWatch(resource);
    if (!w) return;
    // D3D11_MAP_WRITE_DISCARD hands back a whole new allocation, but only this role's own segment is copied out of it:
    // the bytes the draw reads, never the rest of the buffer.
    if (w->mapped && w->role != kRoleNone && w->bytes >= kSegment[w->role].minBytes) {
        const Segment& s = kSegment[w->role];
        const uint64_t t0 = qpc();
        const bool ok = guardedBudget(g_budget, [&] { std::memcpy(w->data, static_cast<const uint8_t*>(w->mapped) + s.offset, s.length); });
        g_win.teeTicks += qpc() - t0;
        if (ok) {
            ++g_win.teeCopies;
            ++g_win.teeMap;
            w->valid = true;
        } else {
            w->valid = false;
        }
    } else if (w->role != kRoleNone) {
        w->valid = false;   // an Unmap with no Map seen (or an unverified size): nothing known about what was written
    }
    w->mapped = nullptr;
}

void celestialMotionConstantsWritten(ID3D11Resource* resource, const void* data, const D3D11_BOX* box) {
    if (!data) return;   // the runtime rejects the call; nothing was written
    Watch* w = findWatch(resource);
    if (!w || w->role == kRoleNone) return;
    const Segment& s = kSegment[w->role];
    const uint32_t bytes = w->bytes ? w->bytes : s.minBytes;
    const uint32_t left = box ? box->left : 0u;
    const uint32_t right = box ? box->right : bytes;
    if (right > bytes || left > right) { w->valid = false; return; }
    // Only the overlap of the write with this role's segment is copied. A partial write onto an invalid shadow leaves
    // it invalid; only a write covering the whole segment makes it valid again.
    const uint32_t s0 = s.offset, s1 = s.offset + s.length;
    const uint32_t lo = left > s0 ? left : s0, hi = right < s1 ? right : s1;
    if (lo < hi) {
        const uint64_t t0 = qpc();
        std::memcpy(w->data + (lo - s0), static_cast<const uint8_t*>(data) + (lo - left), hi - lo);
        g_win.teeTicks += qpc() - t0;
        ++g_win.teeCopies;
        ++g_win.teeUpdate;
        if (lo == s0 && hi == s1) w->valid = true;
    }
}

void celestialMotionConstantsUnknownWrite(ID3D11Resource* resource) {
    if (!resource) {
        for (Watch& w : g_watch) if (w.res) { w.valid = false; w.mapped = nullptr; ++g_win.teeInvalid; }
        return;
    }
    Watch* w = findWatch(resource);
    if (w) {
        w->valid = false;
        w->mapped = nullptr;
        w->bytes = 0;   // a re-created buffer may have another size: verify at the next draw
        ++g_win.teeInvalid;
    }
}

bool celestialMotionRecords(ID3D11DeviceContext* ctx, int eye, const float tanNow[4], int w, int h, CelestialEyeRecords* out) {
    if (out) *out = CelestialEyeRecords{};
    if (!detail::g_celestialLive || !ctx || !out || !tanNow || eye < 0 || eye > 1 || w <= 0 || h <= 0) return false;
    if (!g_budget.shouldRun()) { fail("its fault budget is spent"); return false; }
    bool got = false;
    guardedBudget(g_budget, [&] { got = recordsImpl(ctx, eye, tanNow, w, h, out); });
    return got;
}

void celestialMotionNotePixels(uint32_t pixels) {
    g_win.pixels += pixels;
    g_win.pixelsKnown = true;
}

void celestialMotionFrameBoundary() {
    // Armed (configured on, not stood down), live or not: a closed gate still counts its frames and keeps the 5 s line alive, so a
    // log can tell "the gate held" from "the module never ran".
    if (!g_enabled || g_failed) return;
    ++g_stamp;
    ++g_win.frames;
    if (g_gate != kGateOn) ++g_win.gatedFrames;
    const uint64_t now = qpc();
    LARGE_INTEGER q{};
    QueryPerformanceFrequency(&q);
    if (!g_lastReport) g_lastReport = now;
    if (q.QuadPart > 0 && now - g_lastReport >= static_cast<uint64_t>(q.QuadPart) * 5u) {
        g_lastReport = now;
        guardedBudget(g_budget, [&] { report(); });
        g_win = Counters{};
    }
}

void celestialMotionShutdown() {
    for (EyeState& e : g_eye) {
        e.buffer.Reset();
        e.srv.Reset();
    }
    dropCaptured();
    clearWatches();
    g_win = Counters{};
    std::memset(g_noted, 0, sizeof g_noted);
    std::memset(g_declineNoted, 0, sizeof g_declineNoted);
    std::memset(g_fallbackNoted, 0, sizeof g_fallbackNoted);
    g_lastReport = 0;
    g_stamp = 0;
    g_gate = kGateUnset;
    g_gateLines = 0;
    g_resetPending = false;
    g_bodiesNamed = 0;
    setLive();
}

}  // namespace edvr
