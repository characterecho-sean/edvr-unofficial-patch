// The splash's recogniser, run for real (src/d3d11/intro_curve.cpp; src/d3d11/intro_curve_math.h; docs/intro-video.md, job 3).
//
// With fix.panel_curvature above 0 the splash is drawn as a bent strip in place of the game's flat quad. The splash is the SAME composite as the
// intro movie (VS EF103A7CB4A8369A, six indices) with the game's own world-space placement in VS b2, so the module has to tell, from the shape of a
// draw and a one-shot copy of the 80 bytes the game bound, whether THIS draw is a world-space panel -- and if so what half-width, which depth
// direction and which way the picture's u must run to hand the strip. This rig compiles the REAL intro_curve.cpp with the real Config, Log and fault guard, puts a WARP device behind it
// with the game's state (a constant buffer at VS slot 2 holding real captures, a stand-in for the surface at PS slot 0 in the binding shadow), and
// drives it the way vscreen.cpp will: ask, recognise, hand the numbers to the strip, disarm, and once a frame tick.
//
// The strip itself (panel_curve.cpp, panelCurveSurfaceWanted / Draw / Info) is linked for real in build.bat's rig; the doubles of the three, and of
// the variables panelCurveInfo() reads, are for building without it (the double of Draw records the gain, direction and u direction it is handed).
// Building with INTRO_CURVE_RIG_REAL_STRIP (and panel_curve.cpp in the link) leaves the doubles out.
//
//   C1  THE CAPTURES      the game's two real splash captures, both eyes of each (docs: edvr_gfx_20260828_182818.log, the panel in front and behind):
//                         the first draw of a pair is the game's own and issues the copy; the pair stays unknown through the settle frames and is
//                         read on the fourth tick, not before; then every draw of it is armed with gain 4.44444, direction +1 and the strip's u running
//                         against x, and the staging buffer is gone; the copy, not the buffer's later content, is what is read; one line says so.
//   C2  FLAT VERDICTS     the movie's stock constants and several constants that cannot be a world-space panel (NaN, a short column, a w of zero, a
//                         half-width of 100) are read and left as the game drew them, once, with the reason and the 20 floats in the log; a constant
//                         buffer under 80 bytes is flat at first sight without a copy.
//   C3  A NEW PAIR        a new surface or a new buffer is learned afresh, and a learned world pair is not read again (a flat one is: C13); the
//                         verdict is per pair.
//   C4  CURVATURE 0       at curvature 0 (or with the strip stood down) nothing is wanted, nothing is created, copied or logged whatever draws come, and
//                         a live change to above 0 starts learning at once; a pair learned before the curvature dropped is still armed when it returns.
//   C5  THE SHAPE         only kind X, six indices, one instance, with the intro composite's VS hash and a constant buffer at slot 2 are recognised; a
//                         learned pair does not escape the VS test; no other draw makes an entry or a copy.
//   C6  THE SETTLE        each copy is read on its own due frame; a pair drawn once is still read; a tick with no context reads nothing and does no harm.
//   C7  THE TABLE         16 pairs; a 17th pushes out the one drawn least recently (the earlier arrival of two equals), whose staging buffer goes with it;
//                         it is said once, and counted.
//   C8  EXPIRY            a pair not drawn for 180 frames is forgotten, a pair drawn within them is not, and what it held goes with it.
//   C9  RETIREMENT        the first rendered scene retires the module for good (even at curvature 0), releases what it holds and says what it did, once;
//                         shutdown releases the table.
//   C10 FAULTS            a fault in the draw path, and a fault in the readback, stand THIS module down (its own line; the strip's wanted flag and every
//                         other budget are not touched), and what it held is let go.
//   C11 THE LOG           the world line and the flat line word for word; one line per learned pair; a cap on the lines about FLAT pairs that a
//                         turning-over table writes, said once -- and none on the world lines, the one learned after the cap included.
//   C12 THE WIRING        what the strip is handed: the snapshot's half-width (not a constant) and its direction, the opposite one for a matrix whose
//                         w terms share a sign, and the u direction, both eyes on their own numbers; the numbers hold until the draw is disarmed and
//                         are zero after (the u direction too); a draw that is not armed leaves nothing armed.
//   C13 THE CUT           a flat pair is copied again 60 frames after its last copy: the movie's own buffer and surface reused for the splash (the
//                         same objects, stock bytes and then the world capture) draw as the game's until the re-read has settled, on a frame asserted
//                         exactly for six write times, then armed with 4.44444 and +1, one world line, no flicker before it, a flat pair that stays
//                         flat while the copy is pending; the counters and the retirement clause say what happened.
//   C14 STAYING FLAT      a flat pair drawn for 600 frames stays flat with no extra line: 1 + floor(600 / 60) copies, one staging buffer at a time,
//                         every reference given back; a re-read that gives another reason says so once; the flat cap holds for those lines and not
//                         for a world line; a buffer too small to copy is read again in silence; a re-read left unread for several periods (no
//                         context on the ticks) is never copied over.
//   C15 FAILED RE-READS   a re-read whose map fails leaves the pair flat, gives the buffer back, says so once and is tried again a period later; one
//                         whose readback faults, or whose draw faults, stands this down (the game's draws) and lets go.
//   C16 A WORLD IS FINAL  a world pair is never copied again, whatever its buffer comes to hold (the known limit intro_curve.h says); expiry is how
//                         its verdict ends, and its u direction stays with the verdict.
//   C17 THE DIRECTION     which way u runs, per pair: every real placement (+x to the viewer's left) is armed with reverseU true, the same placements
//                         with the x column negated with it false, eight pairs drawn in turn, the strip in hand and the log following; a pair read
//                         edge-on is flat with the reason and arms, with its own direction, when a re-read finds a good placement in the buffer.
//
// tools\intro_curve_module_test\mutants.py compiles this rig against a copy of the module with ONE rule flipped and requires the rig to fail on the
// case that belongs to the rule: every check below carries a label "C<case>.<what>", and that prefix is what the tool looks for.
//
// Usage: --self-test [--only C1,C4] [--keep]   |   --dry-run (nothing run, nothing written)
#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "../../src/common/config.h"
#include "../../src/common/guard.h"
#include "../../src/common/log.h"
#include "../../src/common/runtime_profile.h"
#include "../../src/common/system_d3d11.h"
#include "binding_shadow.h"
#include "intro_curve.h"
#include "panel_curve.h"
#ifdef INTRO_CURVE_RIG_REAL_STRIP
#include "screen_motion.h"
#endif

using Microsoft::WRL::ComPtr;

namespace {

// ---------------------------------------------------------------------------------------------------------------------------------
// The harness: every failing check is printed once under its label, and the run goes on (one run shows every rule that broke).
// ---------------------------------------------------------------------------------------------------------------------------------
unsigned g_checks = 0;
unsigned g_failed = 0;
std::map<std::string, unsigned> g_failCount;
std::string g_note;   // what the loop is on, appended to a failure's line

std::string fmt(const char* f, ...) {
    char buf[900];
    va_list ap;
    va_start(ap, f);
    _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, f, ap);
    va_end(ap);
    return std::string(buf);
}

void check(bool ok, const std::string& label, const std::string& detail = std::string()) {
    ++g_checks;
    if (ok) return;
    ++g_failed;
    if (++g_failCount[label] == 1) {
        std::string line = "FAIL: " + label;
        if (!detail.empty()) line += " -- " + detail;
        if (!g_note.empty()) line += " [" + g_note + "]";
        std::printf("%s\n", line.c_str());
        std::fflush(stdout);
    }
}

bool has(const std::string& text, const std::string& needle) { return text.find(needle) != std::string::npos; }
size_t count(const std::string& s, const std::string& needle) {
    size_t n = 0;
    for (size_t at = s.find(needle); at != std::string::npos; at = s.find(needle, at + needle.size())) ++n;
    return n;
}
std::string narrow(const std::wstring& w) {
    std::string out;   // the scratch paths are ASCII
    for (wchar_t c : w) out.push_back(static_cast<char>(c));
    return out;
}

// What the module is held to. Each is the module's own constant, spelled again here on purpose: a module that moved one has changed behaviour.
constexpr float kHalfWidth = 4.44444f;   // the splash's measured half-width, metres
constexpr uint32_t kSettle = 4;          // frames a copy waits before it is read
constexpr uint32_t kExpire = 180;        // frames a pair may go undrawn
constexpr uint32_t kRecheck = 60;        // frames from a flat pair's last copy to the next
constexpr uint32_t kTable = 16;          // pairs the table holds
constexpr uint32_t kFlatLines = 12;      // lines about flat pairs per session (a world line is never capped)
constexpr uint64_t kVsHash = 0xEF103A7CB4A8369Aull;   // the intro composite's vertex shader: the census line `vh EF103A7CB4A8369A`

// The game's constants, cb2[0..4] = 20 floats (the same two captures and the movie's stock constants intro_curve_math_test pins).
const float kCap2[20] = {4.44444f, 2.5f, 0, 0,  -0.780684f, -0.0197438f, 9.48621e-08f, -0.000948464f,  -0.0047429f, 0.788105f, -7.60756e-06f, 0.076063f,
                         -0.194148f, 0.0601386f, 9.97268e-05f, -0.997103f,  -0.0725725f, -0.247872f, 0.0996338f, 3.76108f};
const float kCap1[20] = {4.44444f, 2.5f, 0, 0,  0.795297f, 0.0141897f, -9.32133e-06f, 0.0931979f,  -0.0047877f, 0.788078f, -7.64753e-06f, 0.0764627f,
                         0.121095f, -0.0620333f, -9.92872e-05f, 0.992707f,  -0.908756f, 18.4664f, 0.100169f, -1.5911f};
// The other eye of each capture (the same log, the second `DCW read` line of each pair): cb2[1].x and cb2[4].x differ, everything else is alike.
const float kCap2b[20] = {4.44444f, 2.5f, 0, 0,  -0.780317f, -0.0197438f, 9.48621e-08f, -0.000948464f,  -0.0342501f, 0.788105f, -7.60756e-06f, 0.076063f,
                          0.192659f, 0.0601386f, 9.97268e-05f, -0.997103f,  -1.57885f, -0.247872f, 0.0996338f, 3.76108f};
const float kCap1b[20] = {4.44444f, 2.5f, 0, 0,  0.759143f, 0.0141897f, -9.32133e-06f, 0.0931979f,  -0.0344499f, 0.788078f, -7.64753e-06f, 0.0764627f,
                          -0.264007f, -0.0620333f, -9.92872e-05f, 0.992707f,  -0.338754f, 18.4664f, 0.100169f, -1.5911f};
const float kStock[20] = {512, 288, 0, 0,  -0.000368732f, 0, 0, 0,  0, 0.000373413f, 0, 0,  0, 0, 0, 0,  0.193907f, 0, 0, 1};

struct Cb {
    float f[20];
};
Cb cbOf(const float* base) {
    Cb c;
    std::memcpy(c.f, base, sizeof(c.f));
    return c;
}
// The same placement with the x column run the other way (cb2[1] = -cb2[1]): +x then runs to the viewer's RIGHT, as the on-foot screen's does.
Cb cbRunningRight(const float* base) {
    Cb c = cbOf(base);
    for (int i = 4; i < 8; ++i) c.f[i] = -c.f[i];
    return c;
}
// A placement whose x column and centre cancel (cb2[4].x * cb2[1].w = cb2[1].x * cb2[4].w): the panel seen edge-on, +x running neither way.
Cb cbEdgeOn(const float* base) {
    Cb c = cbOf(base);
    c.f[16] = static_cast<float>((static_cast<double>(c.f[4]) * static_cast<double>(c.f[19])) / static_cast<double>(c.f[7]));
    return c;
}

}  // namespace

// ---------------------------------------------------------------------------------------------------------------------------------
// What the module calls that this rig does not link: the strip, and the variables panelCurveInfo() reads.
// ---------------------------------------------------------------------------------------------------------------------------------
#ifndef INTRO_CURVE_RIG_REAL_STRIP
namespace {
struct StripCall {
    ID3D11DeviceContext* ctx;
    float gain;
    int toward;
    bool reverseU;
};
std::vector<StripCall> g_strip;      // every panelCurveSurfaceDraw the rig's wiring made
bool g_stripStoodDown = false;       // the strip's own stand-down, which the module must never cause
}  // namespace

namespace edvr {
namespace detail {
bool g_panelCurveStoodDown = false;
float g_panelCurveCurvature = 0.0f;
int g_panelCurveSegments = kDefaultSegments;
float g_panelCurveGain = 0.0f;
bool g_panelCurveReady = false;
uint64_t g_panelCurveReissues = 0;
}  // namespace detail

bool panelCurveSurfaceWanted() { return detail::g_panelCurveCurvature > 0.0f && !g_stripStoodDown; }

bool panelCurveSurfaceDraw(ID3D11DeviceContext* ctx, float gain, int toward, bool reverseU, PanelCurveDrawFn) {
    g_strip.push_back(StripCall{ctx, gain, toward, reverseU});
    return true;
}

PanelCurveSurfaceInfo panelCurveSurfaceInfo() {
    PanelCurveSurfaceInfo i;
    i.drawn = g_strip.size();
    i.standDown = g_stripStoodDown;
    i.reversed = !g_strip.empty() && g_strip.back().reverseU;   // the strip in hand, as the real one reports it
    return i;
}
}  // namespace edvr
#else
// With panel_curve.cpp linked for real: what IT calls that this rig does not link -- the binding shadow's resolver and the screen's motion
// pass (tools\panel_curve_test stubs the same two, the same way).
namespace edvr {
bool bindingResolveResource(void* resource, ResourceInfo* out) {
    if (!resource || !out) return false;
    ID3D11Resource* r = static_cast<ID3D11Resource*>(resource);
    D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
    r->GetType(&dim);
    if (dim != D3D11_RESOURCE_DIMENSION_BUFFER) return false;
    D3D11_BUFFER_DESC d{};
    static_cast<ID3D11Buffer*>(resource)->GetDesc(&d);
    *out = ResourceInfo{};
    out->isBuffer = true;
    out->a = d.ByteWidth;
    out->b = d.StructureByteStride;
    out->resource = resource;
    return true;
}

void screenMotionDraw(ID3D11DeviceContext*, PanelCurveDrawFn, unsigned, unsigned, unsigned, int, unsigned, const float*) {}
}  // namespace edvr
#endif

// The binding shadow's storage: the module reads the bound VS hash and the PS slot 0 view from it, and this rig is the game that bound them.
namespace edvr {
namespace detail {
BindingSlot g_bindingSlots[static_cast<size_t>(BindSlot::Count)];
}  // namespace detail
}  // namespace edvr

namespace {

using namespace edvr;

// The draw function the strip is handed (the thunk's real draw, in vscreen.cpp): recorded. The doubles ignore it; the real strip issues
// (indices, 1, 0, 0, 0) through it.
struct DrawCall {
    UINT count, instances, start;
    INT base;
    UINT startInstance;
};
std::vector<DrawCall> g_draws;
void __stdcall recordingDraw(ID3D11DeviceContext*, UINT count, UINT instances, UINT start, INT base, UINT startInstance) {
    g_draws.push_back(DrawCall{count, instances, start, base, startInstance});
}

// ---------------------------------------------------------------------------------------------------------------------------------
// The scratch directory, the log the module writes, and the files read back.
// ---------------------------------------------------------------------------------------------------------------------------------
std::wstring g_dir;
std::string slurp(const std::wstring& path) {
    std::ifstream in(path.c_str(), std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}
std::wstring newestLog(const wchar_t* tag) {
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((g_dir + L"\\edvr_" + tag + L"_*.log").c_str(), &fd);
    std::wstring best;
    if (h != INVALID_HANDLE_VALUE) {
        do {
            const std::wstring n = g_dir + L"\\" + fd.cFileName;
            if (n > best) best = n;
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    return best;
}
bool beginLog(const wchar_t* tag) { return Log::get().open(g_dir, tag); }
std::string endLog(const wchar_t* tag) {
    Log::get().close();   // the flusher is joined: the file is complete
    return slurp(newestLog(tag));
}

// ---------------------------------------------------------------------------------------------------------------------------------
// The GPU: a WARP device, and the game's state around the composite draw.
// ---------------------------------------------------------------------------------------------------------------------------------
struct Gpu {
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<ID3D11Buffer> decoy;   // what slots 0, 1 and 3 hold: the movie's stock constants, so a read of the wrong slot is seen
    bool ok = false;
};

ComPtr<ID3D11Buffer> makeCb(Gpu& g, const float* f, UINT bytes = 80) {
    std::vector<float> data(bytes / 4, 0.0f);
    for (size_t i = 0; i < data.size() && i < 20; ++i) data[i] = f[i];
    D3D11_BUFFER_DESC d{};
    d.ByteWidth = bytes;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA init{data.data(), 0, 0};
    ComPtr<ID3D11Buffer> out;
    g.dev->CreateBuffer(&d, &init, &out);
    return out;
}

Gpu makeGpu() {
    Gpu g;
    const PFN_D3D11_CREATE_DEVICE create = systemD3D11CreateDevice();
    if (!create) return g;
    D3D_FEATURE_LEVEL fl{};
    if (FAILED(create(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &g.dev, &fl, &g.ctx)) || !g.dev || !g.ctx) return g;
    g.decoy = makeCb(g, kStock);
    g.ok = g.decoy != nullptr;
    return g;
}

void setCb(Gpu& g, ID3D11Buffer* cb, const float* f) { g.ctx->UpdateSubresource(cb, 0, nullptr, f, 0, 0); }

// The game's constant buffers: the composite's at slot 2 (null for "nothing bound there"), decoys around it.
void bindCb(Gpu& g, ID3D11Buffer* cb) {
    ID3D11Buffer* decoy = g.decoy.Get();
    ID3D11Buffer* slots[4] = {decoy, decoy, cb, decoy};
    g.ctx->VSSetConstantBuffers(0, 4, slots);
}

// The shader resource views the composite samples: the module only ever compares them, so these are stand-in addresses.
char g_views[40];
void* view(int i) { return &g_views[i]; }

// The binding shadow, as the game's draw leaves it: the bound VS and its hash, the view at PS slot 0 (decoys in the slots after it).
void setShadow(uint64_t vsHash, void* surface) {
    detail::g_bindingSlots[static_cast<size_t>(BindSlot::Vs)].hash = vsHash;
    detail::g_bindingSlots[static_cast<size_t>(BindSlot::PsSrv0)].ptr = surface;
    detail::g_bindingSlots[static_cast<size_t>(BindSlot::PsSrv1)].ptr = view(39);
    detail::g_bindingSlots[static_cast<size_t>(BindSlot::PsSrv2)].ptr = view(38);
}

ULONG refs(IUnknown* p) {
    p->AddRef();
    return p->Release();
}

void setCurvature(const char* v) {
#ifdef INTRO_CURVE_RIG_REAL_STRIP
    Config::get().set("fix.panel_curvature", v);
    panelCurveConfigure(Config::get());
#else
    detail::g_panelCurveCurvature = std::strtof(v, nullptr);
#endif
}

// The strip's own buffers are children of the device and each holds a reference on it, so a count of the device's references is taken with the
// strip let go (the doubles hold nothing).
void releaseStrip() {
#ifdef INTRO_CURVE_RIG_REAL_STRIP
    panelCurveShutdown();
#endif
}

void setColumns(int n) {
#ifdef INTRO_CURVE_RIG_REAL_STRIP
    Config::get().set("advanced.panel_curvature_segments", std::to_string(n).c_str());
    panelCurveConfigure(Config::get());
#else
    detail::g_panelCurveSegments = n;
#endif
}

// Every case starts from a fresh process's module state: a retirement and a stand-down are both for the session, so they cannot be undone any
// other way. The curvature is 0.3, 64 columns, and the composite's VS is bound with no surface.
void fresh() {
    introCurveResetForTest();
#ifndef INTRO_CURVE_RIG_REAL_STRIP
    detail::g_panelCurveStoodDown = false;
    detail::g_panelCurveSegments = 64;
    g_stripStoodDown = false;
    g_strip.clear();
#else
    panelCurveShutdown();   // the strip in hand and the key it was built for go; its counters and stand-down are the session's
    Config::get().set("advanced.panel_curvature_segments", "64");
#endif
    g_draws.clear();
    setCurvature("0.3");
    setShadow(kVsHash, nullptr);
    g_note.clear();
}

// One eye draw the way vscreen.cpp's wiring makes it: ask, recognise, hand the numbers to the strip, disarm. The result carries what the strip
// was handed and what the accessors say once the draw is disarmed.
struct Wired {
    bool asked = false;         // introCurveWants()
    bool armed = false;         // introCurveOnComposite()
    float gain = 0.0f;          // introCurveGain() while armed
    int toward = 0;             // introCurveToward() while armed
    bool reverseU = false;      // introCurveReverseU() while armed: the strip's u runs against its x
    float gainAfter = 1.0f;     // ... after introCurveEndDraw()
    int towardAfter = 1;
    bool reverseUAfter = true;
};

bool rawDraw(Gpu& g, ID3D11Buffer* cb, void* surface, char kind = 'X', uint32_t count = 6, uint32_t instances = 1,
             uint64_t vs = kVsHash) {
    bindCb(g, cb);
    setShadow(vs, surface);
    return introCurveOnComposite(g.ctx.Get(), kind, count, instances);
}

Wired wired(Gpu& g, ID3D11Buffer* cb, void* surface) {
    Wired w;
    bindCb(g, cb);
    setShadow(kVsHash, surface);
    w.asked = introCurveWants();
    if (!w.asked) return w;
    w.armed = introCurveOnComposite(g.ctx.Get(), 'X', 6, 1);
    if (w.armed) {
        w.gain = introCurveGain();
        w.toward = introCurveToward();
        w.reverseU = introCurveReverseU();
        panelCurveSurfaceDraw(g.ctx.Get(), w.gain, w.toward, w.reverseU, recordingDraw);
        introCurveEndDraw();
        w.gainAfter = introCurveGain();
        w.towardAfter = introCurveToward();
        w.reverseUAfter = introCurveReverseU();
    }
    return w;
}

void tick(Gpu& g, uint32_t n = 1, bool scene = false) {
    for (uint32_t i = 0; i < n; ++i) introCurveTick(g.ctx.Get(), scene);
}

// A pair seen once and read: the draw, then the frames the copy waits.
void learn(Gpu& g, ID3D11Buffer* cb, void* surface) {
    wired(g, cb, surface);
    tick(g, kSettle);
}

// A context that raises an access violation on the first call through it, the way one gone bad does.
ID3D11DeviceContext* badCtx() { return reinterpret_cast<ID3D11DeviceContext*>(static_cast<uintptr_t>(0x10)); }

// One pair's staging buffer watched from outside, so that the module's own counters are not what counts its copies: a buffer that appears where
// there was none is a copy issued, and the rig keeps a reference of its own on the one in flight, so that the module's letting it go is the count
// falling to the rig's alone. `stuck` counts buffers the module gave up while something still held them (a leaked reference); `swapped` counts the
// times one buffer was seen replaced by another with no gap between (a copy issued over one still in flight). Look after every draw and tick.
struct StageWatch {
    void* buffer;
    void* surface;
    ID3D11Buffer* held = nullptr;
    unsigned copies = 0;
    unsigned stuck = 0;
    unsigned swapped = 0;
    StageWatch(void* b, void* s) : buffer(b), surface(s) {}
    StageWatch(const StageWatch&) = delete;
    StageWatch& operator=(const StageWatch&) = delete;
    ~StageWatch() { done(); }
    void look() {
        ID3D11Buffer* now = introCurveStageForTest(buffer, surface);
        if (now == held) return;
        if (held) {
            if (held->Release() != 0) ++stuck;
            held = nullptr;
            if (now) ++swapped;
        }
        if (now) {
            now->AddRef();
            held = now;
            ++copies;
        }
    }
    void done() {   // the rig's own reference given back, before a count of the device's references is taken
        if (held) held->Release();
        held = nullptr;
    }
};

// One pair drawn once a frame, the way the intro draws it, its staging buffer watched, and the module's frame counted from its start.
struct Flyer {
    Gpu& g;
    ID3D11Buffer* cb;
    void* surface;
    StageWatch watch;
    uint32_t frame = 0;
    Flyer(Gpu& gpu, ID3D11Buffer* buffer, void* surf) : g(gpu), cb(buffer), surface(surf), watch(buffer, surf) {}
    Wired draw() {
        const Wired w = wired(g, cb, surface);
        watch.look();
        return w;
    }
    void next(ID3D11DeviceContext* ctx = nullptr) {   // the frame boundary, through `ctx` when the rig wants the read to go through another
        introCurveTick(ctx ? ctx : g.ctx.Get(), false);
        ++frame;
        watch.look();
    }
    void nextWithoutContext() {   // a frame boundary that has no context to read with
        introCurveTick(nullptr, false);
        ++frame;
        watch.look();
    }
};

// ---------------------------------------------------------------------------------------------------------------------------------
// C1  THE CAPTURES
// ---------------------------------------------------------------------------------------------------------------------------------
void case1(Gpu& g) {
    struct Cap {
        const char* name;
        const float* f;
        const wchar_t* tag;
        const char* w3;   // cb2[3].w as the log prints it
        const char* w4;   // cb2[4].w
    };
    const Cap caps[] = {{"capture 2, the panel in front", kCap2, L"ic_c1a", "-0.997", "3.761"},
                        {"capture 1, the panel behind", kCap1, L"ic_c1b", "0.993", "-1.591"},
                        {"capture 2, the other eye", kCap2b, L"ic_c1c", "-0.997", "3.761"},
                        {"capture 1, the other eye", kCap1b, L"ic_c1d", "0.993", "-1.591"}};
    for (const Cap& cap : caps) {
        g_note = cap.name;
        fresh();
        check(beginLog(cap.tag), "C1.log: the scratch log opens");
        ComPtr<ID3D11Buffer> cb = makeCb(g, cap.f);
        const ULONG refsBefore = refs(cb.Get());
        const ULONG devBefore = refs(g.dev.Get());

        Wired w = wired(g, cb.Get(), view(0));
        IntroCurveInfo i = introCurveInfo();
        check(w.asked, "C1.asked: the strip is asked for at curvature 0.3");
        check(!w.armed, "C1.first-sight-is-the-games: the first draw of a pair is not armed");
        check(i.entries == 1 && i.copying == 1 && i.staging == 1 && i.worlds == 0 && i.flats == 0,
              "C1.first-sight-copies: one entry, one staging buffer, copying", fmt("%u entries %u copying %u staging", i.entries, i.copying, i.staging));
        check(introCurveStageForTest(cb.Get(), view(0)) != nullptr, "C1.copy-issued-at-first-sight: the staging buffer exists at once");

        for (uint32_t t = 1; t < kSettle; ++t) {
            tick(g);
            w = wired(g, cb.Get(), view(0));
            i = introCurveInfo();
            check(!w.armed && i.copying == 1 && i.worlds == 0 && i.staging == 1, "C1.unknown-until-settled: not read before the fourth tick",
                  fmt("tick %u: armed %d, %u copying", t, w.armed ? 1 : 0, i.copying));
        }
        ID3D11Buffer* held = introCurveStageForTest(cb.Get(), view(0));
        if (held) held->AddRef();   // the rig's own reference: the module's going is the count falling to ours alone
        tick(g);   // the fourth tick: the readback
        i = introCurveInfo();
        check(i.worlds == 1 && i.copying == 0 && i.flats == 0 && i.learned == 1, "C1.read-on-the-fourth-tick: the pair is a world-space panel",
              fmt("%u worlds %u copying %u flats", i.worlds, i.copying, i.flats));
        check(i.staging == 0 && introCurveStageForTest(cb.Get(), view(0)) == nullptr, "C1.staging-let-go-after-the-read");
        if (held) {
            const ULONG left = held->Release();
            check(left == 0, "C1.staging-buffer-released-after-the-read: the module's reference is gone", fmt("%lu left", left));
        }

        w = wired(g, cb.Get(), view(0));
        check(w.armed, "C1.armed-after-the-read: the next draw is handed to the strip");
        check(w.gain == kHalfWidth, "C1.gain-is-the-half-width: cb2[0].x", fmt("%.7f", static_cast<double>(w.gain)));
        check(w.toward == 1, "C1.toward-is-plus-one: cb2[3].w and cb2[4].w have opposite signs in this capture", fmt("%d", w.toward));
        check(w.reverseU, "C1.reverse-u: every real capture's +x runs to the viewer's left, so the strip's u runs against x");
        check(w.gainAfter == 0.0f && w.towardAfter == 0 && !w.reverseUAfter, "C1.end-draw-disarms: the gain, the direction and the u direction");
        for (int n = 0; n < 10; ++n) {
            tick(g);
            w = wired(g, cb.Get(), view(0));
            check(w.armed && w.gain == kHalfWidth && w.toward == 1 && w.reverseU, "C1.stays-armed: every later draw of the pair, with the same numbers");
        }
        i = introCurveInfo();
        check(i.armed == 11, "C1.armed-count: the draws handed to the strip", fmt("%llu", static_cast<unsigned long long>(i.armed)));
        check(refs(cb.Get()) == refsBefore, "C1.no-reference-left-on-the-games-buffer", fmt("%lu then %lu", refsBefore, refs(cb.Get())));
        releaseStrip();
        check(refs(g.dev.Get()) == devBefore, "C1.no-reference-left-on-the-device", fmt("%lu then %lu", devBefore, refs(g.dev.Get())));

        const std::string log = endLog(cap.tag);
        check(count(log, "splash curve:") == 1, "C1.one-line: one line for the learned pair", fmt("%zu lines", count(log, "splash curve:")));
        check(has(log, fmt("splash curve: the game-placed composite (VS EF103A7C) reads as a world-space panel: half-width 4.444 m, depth toward the "
                           "viewer +1 (cb2[3].w %s, cb2[4].w %s); +x runs to the viewer's left, so u runs against x; drawn as a 64-column strip at "
                           "curvature 0.300.", cap.w3, cap.w4)),
              "C1.world-line: the line that says what was read");
    }

    // The copy is what is read, not whatever the buffer holds when the frames have passed.
    g_note = "the buffer changes after the copy";
    fresh();
    {
        ComPtr<ID3D11Buffer> a = makeCb(g, kCap2), b = makeCb(g, kStock);
        wired(g, a.Get(), view(0));
        wired(g, b.Get(), view(1));
        setCb(g, a.Get(), kStock);   // after the copies were issued: must not be seen
        setCb(g, b.Get(), kCap2);
        tick(g, kSettle);
        check(wired(g, a.Get(), view(0)).armed, "C1.the-copy-is-read: a buffer that read world at its copy stays world however it changes after");
        check(!wired(g, b.Get(), view(1)).armed, "C1.the-copy-is-read: a buffer that read stock at its copy stays flat however it changes after");
    }
    g_note.clear();
}

// ---------------------------------------------------------------------------------------------------------------------------------
// C2  FLAT VERDICTS
// ---------------------------------------------------------------------------------------------------------------------------------
void case2(Gpu& g) {
    struct Flat {
        const char* name;
        Cb cb;
        const char* word;   // what the reason says
    };
    std::vector<Flat> table;
    table.push_back({"the movie's stock constants", cbOf(kStock), "screen-space placement"});
    {
        Cb c = cbOf(kCap2);
        c.f[4] = std::nanf("");
        table.push_back({"a NaN in cb2[1].x", c, "not a finite number"});
    }
    {
        Cb c = cbOf(kCap2);
        c.f[12] = c.f[13] = c.f[14] = c.f[15] = 0.01f;
        table.push_back({"a short cb2[3]", c, "unit-scale column"});
    }
    {
        Cb c = cbOf(kCap2);
        c.f[14] = 0.0f;
        c.f[15] = 0.0f;
        c.f[12] = 0.6f;
        c.f[13] = 0.6f;
        table.push_back({"a cb2[3].w of zero", c, "cb2[3].w is too close to zero"});
    }
    {
        Cb c = cbOf(kCap2);
        c.f[0] = 100.0f;
        c.f[1] = 56.25f;
        table.push_back({"a half-width of 100", c, "half-width"});
    }
    for (size_t k = 0; k < table.size(); ++k) {
        const Flat& row = table[k];
        g_note = row.name;
        fresh();
        const std::wstring tag = L"ic_c2_" + std::to_wstring(k);
        check(beginLog(tag.c_str()), "C2.log: the scratch log opens");
        ComPtr<ID3D11Buffer> cb = makeCb(g, row.cb.f);
        learn(g, cb.Get(), view(0));
        IntroCurveInfo i = introCurveInfo();
        check(i.flats == 1 && i.worlds == 0 && i.copying == 0 && i.staging == 0, "C2.flat-after-the-read: left as the game drew it",
              fmt("%u flats %u worlds %u copying %u staging", i.flats, i.worlds, i.copying, i.staging));
        for (int n = 0; n < 30; ++n) {
            tick(g);
            check(!wired(g, cb.Get(), view(0)).armed, "C2.never-armed: no later draw of a flat pair is handed to the strip");
        }
        i = introCurveInfo();
        check(i.armed == 0 && i.learned == 1, "C2.counts: nothing armed, one pair learned", fmt("%llu armed %llu learned", static_cast<unsigned long long>(i.armed),
                                                                                           static_cast<unsigned long long>(i.learned)));
        const std::string log = endLog(tag.c_str());
        check(count(log, "splash curve:") == 1, "C2.one-line: said once however many draws follow", fmt("%zu lines", count(log, "splash curve:")));
        check(has(log, "this composite stays as the game drew it, because ") && has(log, row.word), "C2.reason-in-the-log: the line says why",
              row.word);
        check(has(log, "Its cb2: "), "C2.floats-in-the-log: the 20 floats follow, for the next flight to read");
    }

    g_note = "a constant buffer under 80 bytes";
    fresh();
    {
        check(beginLog(L"ic_c2_small"), "C2.log: the scratch log opens");
        ComPtr<ID3D11Buffer> tiny = makeCb(g, kCap2, 64);
        const bool armed = wired(g, tiny.Get(), view(0)).armed;
        IntroCurveInfo i = introCurveInfo();
        check(!armed && i.flats == 1 && i.copying == 0 && i.staging == 0, "C2.small-buffer-flat-at-first-sight: no copy is made of 64 bytes",
              fmt("%u flats %u copying %u staging", i.flats, i.copying, i.staging));
        check(introCurveStageForTest(tiny.Get(), view(0)) == nullptr, "C2.small-buffer-no-staging-buffer");
        wired(g, tiny.Get(), view(0));
        tick(g, kSettle + 2);
        const std::string log = endLog(L"ic_c2_small");
        check(count(log, "splash curve:") == 1 && has(log, "its constant buffer is 64 bytes, under the 80 the shader declares"),
              "C2.small-buffer-line: said once, with the size", fmt("%zu lines", count(log, "splash curve:")));
    }

    // A copy that will not map: a deferred context cannot Map(READ), so the read fails the way a lost device's would -- an error code, not a
    // fault. The pair is flat, the buffer is let go, and the module is not stood down.
    g_note = "a copy that cannot be read back";
    fresh();
    {
        check(beginLog(L"ic_c2_nomap"), "C2.log: the scratch log opens");
        ComPtr<ID3D11DeviceContext> deferred;
        g.dev->CreateDeferredContext(0, &deferred);
        check(deferred != nullptr, "C2.deferred-context: the rig can make a context whose Map(READ) fails");
        ComPtr<ID3D11Buffer> cb = makeCb(g, kCap2);
        wired(g, cb.Get(), view(0));
        ID3D11Buffer* held = introCurveStageForTest(cb.Get(), view(0));
        if (held) held->AddRef();
        if (deferred) {
            for (uint32_t n = 0; n < kSettle; ++n) introCurveTick(deferred.Get(), false);
        }
        const IntroCurveInfo i = introCurveInfo();
        check(i.flats == 1 && i.worlds == 0 && i.copying == 0 && i.staging == 0 && !i.standDown,
              "C2.unreadable-copy-is-flat: not armed, not a fault, nothing held", fmt("%u flats %u worlds %u staging", i.flats, i.worlds, i.staging));
        if (held) {
            const ULONG left = held->Release();
            check(left == 0, "C2.unreadable-copy-staging-buffer-released", fmt("%lu left", left));
        }
        tick(g, 3);
        check(!wired(g, cb.Get(), view(0)).armed, "C2.unreadable-copy-stays-flat: it is not read again");
        const std::string log = endLog(L"ic_c2_nomap");
        check(count(log, "splash curve:") == 1 && has(log, "this composite stays as the game drew it, because the copy of its constants could not be read back."),
              "C2.unreadable-copy-line: said once", fmt("%zu lines", count(log, "splash curve:")));
    }
    g_note.clear();
}

// ---------------------------------------------------------------------------------------------------------------------------------
// C3  A NEW PAIR
// ---------------------------------------------------------------------------------------------------------------------------------
void case3(Gpu& g) {
    fresh();
    ComPtr<ID3D11Buffer> a = makeCb(g, kCap2), b = makeCb(g, kCap1);
    learn(g, a.Get(), view(0));
    check(wired(g, a.Get(), view(0)).armed, "C3.first-pair-armed");

    // A new surface over the same buffer.
    g_note = "same buffer, new surface";
    Wired w = wired(g, a.Get(), view(1));
    IntroCurveInfo i = introCurveInfo();
    check(!w.armed && i.entries == 2 && i.copying == 1, "C3.new-surface-is-a-new-pair: the game's own until it is read",
          fmt("armed %d, %u entries %u copying", w.armed ? 1 : 0, i.entries, i.copying));
    check(wired(g, a.Get(), view(0)).armed, "C3.old-pair-stays-armed: the first pair is not unlearned by the second");
    tick(g, kSettle);
    check(wired(g, a.Get(), view(1)).armed, "C3.new-surface-armed-once-read");

    // A new buffer over the same surface.
    g_note = "new buffer, same surface";
    w = wired(g, b.Get(), view(0));
    i = introCurveInfo();
    check(!w.armed && i.entries == 3 && i.copying == 1, "C3.new-buffer-is-a-new-pair", fmt("armed %d, %u entries %u copying", w.armed ? 1 : 0, i.entries, i.copying));
    tick(g, kSettle);
    w = wired(g, b.Get(), view(0));
    check(w.armed && w.gain == kHalfWidth && w.toward == 1, "C3.new-buffer-armed-once-read");

    // Both new.
    g_note = "new buffer and new surface";
    ComPtr<ID3D11Buffer> c = makeCb(g, kStock);
    check(!wired(g, c.Get(), view(2)).armed, "C3.both-new-is-a-new-pair");
    tick(g, kSettle);
    i = introCurveInfo();
    check(!wired(g, c.Get(), view(2)).armed && i.flats == 1 && i.worlds == 3, "C3.each-pair-has-its-own-verdict: the stock pair is flat beside three worlds",
          fmt("%u flats %u worlds", i.flats, i.worlds));

    // A learned world pair is not read again: the game's buffer changes under it and the verdict stands; a new surface over it reads the new content.
    g_note = "a learned pair is not read again";
    setCb(g, a.Get(), kStock);
    tick(g, kSettle + 2);
    check(wired(g, a.Get(), view(0)).armed && wired(g, a.Get(), view(1)).armed, "C3.learned-pair-not-reread: the verdict stands when the buffer changes");
    check(!wired(g, a.Get(), view(3)).armed, "C3.changed-buffer-new-surface-is-new");
    tick(g, kSettle);
    check(!wired(g, a.Get(), view(3)).armed && introCurveInfo().flats == 2, "C3.changed-buffer-new-surface-reads-the-new-content: it is flat now");

    // The copy is made once per pair however often it is drawn while it settles.
    g_note = "one copy per pair";
    fresh();
    {
        ComPtr<ID3D11Buffer> d = makeCb(g, kCap2);
        wired(g, d.Get(), view(0));
        ID3D11Buffer* first = introCurveStageForTest(d.Get(), view(0));
        wired(g, d.Get(), view(0));
        tick(g);
        wired(g, d.Get(), view(0));
        ID3D11Buffer* later = introCurveStageForTest(d.Get(), view(0));
        check(first != nullptr && first == later && introCurveInfo().staging == 1, "C3.one-copy-per-pair: drawing a settling pair again makes no second buffer");
    }
    g_note.clear();
}

// ---------------------------------------------------------------------------------------------------------------------------------
// C4  CURVATURE 0
// ---------------------------------------------------------------------------------------------------------------------------------
void case4(Gpu& g) {
    fresh();
    setCurvature("0");
    check(beginLog(L"ic_c4a"), "C4.log: the scratch log opens");
    ComPtr<ID3D11Buffer> cb = makeCb(g, kCap2), st = makeCb(g, kStock);
    const ULONG refsBefore = refs(cb.Get());
    bool anyArmed = false, anyAsked = false;
    for (int n = 0; n < 40; ++n) {
        const Wired w1 = wired(g, cb.Get(), view(0));
        const Wired w2 = wired(g, st.Get(), view(1));
        anyAsked = anyAsked || w1.asked || w2.asked;
        anyArmed = anyArmed || w1.armed || w2.armed;
        anyArmed = anyArmed || rawDraw(g, cb.Get(), view(0));   // asked directly, past the gate
        tick(g);
    }
    IntroCurveInfo i = introCurveInfo();
    check(!anyAsked && !i.wanted, "C4.not-wanted-at-curvature-0");
    check(!anyArmed, "C4.nothing-armed-at-curvature-0: even a draw asked past the gate");
    check(i.entries == 0 && i.staging == 0 && i.learned == 0 && i.armed == 0, "C4.nothing-created-at-curvature-0: no entry, no copy, no read",
          fmt("%u entries %u staging %llu learned", i.entries, i.staging, static_cast<unsigned long long>(i.learned)));
    check(introCurveStageForTest(cb.Get(), view(0)) == nullptr, "C4.no-staging-buffer-at-curvature-0");
    check(refs(cb.Get()) == refsBefore, "C4.no-reference-taken-at-curvature-0", fmt("%lu then %lu", refsBefore, refs(cb.Get())));
    const std::string quiet = endLog(L"ic_c4a");
    check(!has(quiet, "splash curve"), "C4.no-line-at-curvature-0: nothing is written");

    // A live change to above 0 starts at once, and a learned pair outlives a drop back to 0.
    g_note = "curvature 0 then 0.3 then 0 then 0.3";
    setCurvature("0.3");
    learn(g, cb.Get(), view(0));
    check(wired(g, cb.Get(), view(0)).armed, "C4.live-change-learns: curvature raised while running");
    setCurvature("0");
    const Wired off = wired(g, cb.Get(), view(0));
    check(!off.asked && !off.armed && !rawDraw(g, cb.Get(), view(0)), "C4.dropped-to-0-not-wanted: the learned pair is not armed at curvature 0");
    check(introCurveInfo().entries == 1 && introCurveInfo().worlds == 1, "C4.dropped-to-0-keeps-the-pair: nothing is forgotten by the drop");
    setCurvature("0.3");
    check(wired(g, cb.Get(), view(0)).armed, "C4.raised-again-armed-at-once: the pair is still learned");

    // The strip stood down: not wanted whatever the curvature.
    g_note = "the strip stood down";
#ifndef INTRO_CURVE_RIG_REAL_STRIP
    g_stripStoodDown = true;
    const Wired down = wired(g, cb.Get(), view(0));
    check(!down.asked && !down.armed && !rawDraw(g, cb.Get(), view(0)), "C4.strip-stood-down-not-wanted: the module follows the strip's own flag");
    g_stripStoodDown = false;
    check(wired(g, cb.Get(), view(0)).armed, "C4.strip-back-armed");
#endif
    g_note.clear();
}

// ---------------------------------------------------------------------------------------------------------------------------------
// C5  THE SHAPE
// ---------------------------------------------------------------------------------------------------------------------------------
void case5(Gpu& g) {
    check(kIntroCompositeVsHash == kVsHash, "C5.the-vs-hash-is-the-intro-composites: EF103A7CB4A8369A, as the census and the disassembly name it");
    fresh();
    check(beginLog(L"ic_c5"), "C5.log: the scratch log opens");
    ComPtr<ID3D11Buffer> cb = makeCb(g, kCap2);
    const ULONG refsBefore = refs(cb.Get());
    struct Shape {
        const char* name;
        char kind;
        uint32_t count;
        uint32_t instances;
        uint64_t vs;
    };
    const Shape shapes[] = {
        {"kind N", 'N', 6, 1, kVsHash},
        {"kind lower-case x", 'x', 6, 1, kVsHash},
        {"kind NUL", '\0', 6, 1, kVsHash},
        {"5 indices", 'X', 5, 1, kVsHash},
        {"7 indices", 'X', 7, 1, kVsHash},
        {"0 indices", 'X', 0, 1, kVsHash},
        {"2 instances", 'X', 6, 2, kVsHash},
        {"0 instances", 'X', 6, 0, kVsHash},
        {"no VS hash", 'X', 6, 1, 0},
        {"another VS", 'X', 6, 1, 0xDED8796049C7BB4Aull},
        {"the VS hash one bit off", 'X', 6, 1, kVsHash ^ 1ull},
        {"the VS hash's high half only", 'X', 6, 1, kVsHash & 0xFFFFFFFF00000000ull},
        {"the VS hash's low half only", 'X', 6, 1, kVsHash & 0x00000000FFFFFFFFull},
    };
    for (const Shape& s : shapes) {
        g_note = s.name;
        const bool armed = rawDraw(g, cb.Get(), view(0), s.kind, s.count, s.instances, s.vs);
        const IntroCurveInfo i = introCurveInfo();
        check(!armed && i.entries == 0 && i.staging == 0, "C5.not-the-composite: no entry and no copy for a draw of another shape",
              fmt("armed %d, %u entries %u staging", armed ? 1 : 0, i.entries, i.staging));
    }
    g_note = "no buffer at slot 2";
    {
        bindCb(g, nullptr);
        setShadow(kVsHash, view(0));
        const bool armed = introCurveOnComposite(g.ctx.Get(), 'X', 6, 1);
        check(!armed && introCurveInfo().entries == 0, "C5.no-buffer-no-entry: slot 2 empty makes no entry");
    }
    g_note = "a null context";
    {
        bindCb(g, cb.Get());
        setShadow(kVsHash, view(0));
        check(!introCurveOnComposite(nullptr, 'X', 6, 1) && introCurveInfo().entries == 0, "C5.null-context-is-safe: false, no entry, no fault");
        check(!introCurveInfo().standDown, "C5.null-context-no-stand-down");
    }
    g_note = "the VS test guards a learned pair";
    {
        learn(g, cb.Get(), view(0));
        check(wired(g, cb.Get(), view(0)).armed, "C5.learned-armed");
        check(!rawDraw(g, cb.Get(), view(0), 'X', 6, 1, 0xDED8796049C7BB4Aull), "C5.learned-pair-does-not-escape-the-vs-test");
        check(!rawDraw(g, cb.Get(), view(0), 'X', 6, 2), "C5.learned-pair-does-not-escape-the-instance-test");
        check(!rawDraw(g, cb.Get(), view(0), 'X', 12, 1), "C5.learned-pair-does-not-escape-the-index-test");
        check(!rawDraw(g, cb.Get(), view(0), 'N', 6, 1), "C5.learned-pair-does-not-escape-the-kind-test");
        check(introCurveInfo().entries == 1, "C5.still-one-entry");
    }
    g_note.clear();
    check(refs(cb.Get()) == refsBefore, "C5.no-reference-left-on-the-games-buffer", fmt("%lu then %lu", refsBefore, refs(cb.Get())));
    const std::string log = endLog(L"ic_c5");
    check(count(log, "splash curve:") == 1, "C5.only-the-real-composite-is-logged", fmt("%zu lines", count(log, "splash curve:")));
}

// ---------------------------------------------------------------------------------------------------------------------------------
// C6  THE SETTLE
// ---------------------------------------------------------------------------------------------------------------------------------
void case6(Gpu& g) {
    fresh();
    ComPtr<ID3D11Buffer> a = makeCb(g, kCap2), b = makeCb(g, kCap1);
    wired(g, a.Get(), view(0));   // frame 0: due on the fourth tick
    tick(g, 2);
    wired(g, b.Get(), view(1));   // frame 2: due on the sixth
    tick(g, 1);
    IntroCurveInfo i = introCurveInfo();
    check(i.worlds == 0 && i.copying == 2, "C6.nothing-read-early: two pairs still settling after three ticks", fmt("%u worlds %u copying", i.worlds, i.copying));
    tick(g, 1);   // frame 4
    i = introCurveInfo();
    check(i.worlds == 1 && i.copying == 1 && i.staging == 1, "C6.first-pair-read-on-its-own-frame: the one drawn at frame 0 is read at frame 4",
          fmt("%u worlds %u copying %u staging", i.worlds, i.copying, i.staging));
    tick(g, 1);   // frame 5
    check(introCurveInfo().copying == 1, "C6.second-pair-not-read-early: still settling at frame 5");
    tick(g, 1);   // frame 6
    i = introCurveInfo();
    check(i.worlds == 2 && i.copying == 0 && i.staging == 0, "C6.second-pair-read-on-its-own-frame: the one drawn at frame 2 is read at frame 6",
          fmt("%u worlds %u copying %u staging", i.worlds, i.copying, i.staging));
    check(wired(g, a.Get(), view(0)).armed && wired(g, b.Get(), view(1)).armed, "C6.both-pairs-armed-without-being-drawn-in-between: a pair drawn once is still read");

    // A tick with no context reads nothing and does no harm; the next one with a context does the read.
    g_note = "a tick with no context";
    fresh();
    {
        ComPtr<ID3D11Buffer> c = makeCb(g, kCap2);
        wired(g, c.Get(), view(0));
        for (uint32_t n = 0; n < kSettle + 3; ++n) introCurveTick(nullptr, false);
        i = introCurveInfo();
        check(i.copying == 1 && i.staging == 1 && i.worlds == 0 && !i.standDown, "C6.no-context-reads-nothing: the copy waits, nothing faults",
              fmt("%u copying %u staging %u worlds", i.copying, i.staging, i.worlds));
        tick(g);
        i = introCurveInfo();
        check(i.worlds == 1 && i.copying == 0, "C6.read-when-a-context-arrives: due long ago, read at the next tick with one");
    }
    g_note.clear();
}

// ---------------------------------------------------------------------------------------------------------------------------------
// C7  THE TABLE
// ---------------------------------------------------------------------------------------------------------------------------------
void case7(Gpu& g) {
    // 17 pairs seen on frame 0 and 1: the table holds 16, and the one drawn least recently goes -- by the frame it was last drawn, then by arrival.
    fresh();
    check(beginLog(L"ic_c7a"), "C7.log: the scratch log opens");
    std::vector<ComPtr<ID3D11Buffer>> bufs;
    for (uint32_t k = 0; k < kTable + 4; ++k) bufs.push_back(makeCb(g, kCap2));
    for (uint32_t k = 0; k < kTable; ++k) wired(g, bufs[k].Get(), view(0));
    IntroCurveInfo i = introCurveInfo();
    check(i.entries == kTable && i.staging == kTable && i.evicted == 0, "C7.full-table: sixteen pairs, sixteen copies, nothing pushed out",
          fmt("%u entries %u staging %llu evicted", i.entries, i.staging, static_cast<unsigned long long>(i.evicted)));
    tick(g, 1);                          // frame 1
    wired(g, bufs[0].Get(), view(0));    // the first pair drawn again: no longer the least recent
    ID3D11Buffer* firstStage = introCurveStageForTest(bufs[0].Get(), view(0));
    ID3D11Buffer* secondStage = introCurveStageForTest(bufs[1].Get(), view(0));
    ID3D11Buffer* lastStage = introCurveStageForTest(bufs[kTable - 1].Get(), view(0));
    check(firstStage && secondStage && lastStage, "C7.stages-exist-before-the-eviction");
    if (secondStage) secondStage->AddRef();   // the rig's own reference: the module's going is the count falling to ours alone
    wired(g, bufs[kTable].Get(), view(0));   // the seventeenth
    i = introCurveInfo();
    check(i.entries == kTable && i.evicted == 1, "C7.seventeenth-pushes-one-out: still sixteen, one evicted", fmt("%u entries %llu evicted", i.entries,
                                                                                                                static_cast<unsigned long long>(i.evicted)));
    check(introCurveStageForTest(bufs[1].Get(), view(0)) == nullptr, "C7.the-least-recent-is-the-one-pushed-out: the second pair, not the first");
    check(introCurveStageForTest(bufs[0].Get(), view(0)) == firstStage, "C7.the-pair-drawn-again-stays: LRU, not first in first out");
    check(introCurveStageForTest(bufs[kTable - 1].Get(), view(0)) == lastStage, "C7.the-newest-old-pair-stays: not the newest evicted");
    check(introCurveStageForTest(bufs[kTable].Get(), view(0)) != nullptr, "C7.the-new-pair-is-in-and-copying");
    if (secondStage) {
        const ULONG left = secondStage->Release();
        check(left == 0, "C7.evicted-staging-buffer-released: the module's reference went with the pair", fmt("%lu left", left));
    }
    // The pushed-out pair is a stranger when it returns: its draw is the game's, and it pushes another out.
    check(!wired(g, bufs[1].Get(), view(0)).armed && introCurveInfo().evicted == 2, "C7.an-evicted-pair-is-learned-afresh: and takes another's place");
    const std::string log1 = endLog(L"ic_c7a");
    check(count(log1, "more than 16 composites in use at once") == 1, "C7.eviction-said-once", fmt("%zu", count(log1, "more than 16 composites in use at once")));

    // Equal last-drawn frames: the earlier arrival goes.
    g_note = "ties";
    fresh();
    for (uint32_t k = 0; k < kTable; ++k) wired(g, bufs[k].Get(), view(1));
    ID3D11Buffer* s0 = introCurveStageForTest(bufs[0].Get(), view(1));
    ID3D11Buffer* s1 = introCurveStageForTest(bufs[1].Get(), view(1));
    check(s0 && s1, "C7.tie-stages-exist");
    wired(g, bufs[kTable].Get(), view(1));
    check(introCurveStageForTest(bufs[0].Get(), view(1)) == nullptr && introCurveStageForTest(bufs[1].Get(), view(1)) == s1,
          "C7.the-earlier-arrival-goes-on-a-tie: the first pair is pushed out, the second stays");
    // The eighteenth, the same frame: the newcomer sits in the first pair's old slot, and it is ARRIVAL order that counts, not slot order.
    wired(g, bufs[kTable + 1].Get(), view(1));
    check(introCurveStageForTest(bufs[1].Get(), view(1)) == nullptr && introCurveStageForTest(bufs[kTable].Get(), view(1)) != nullptr,
          "C7.arrival-order-not-slot-order: the second pair goes, the seventeenth (in the first one's slot) stays");

    // A learned pair is pushed out the same way, and the table keeps working for the rest.
    g_note = "learned pairs";
    fresh();
    const ULONG devBefore = refs(g.dev.Get());
    for (uint32_t k = 0; k < kTable; ++k) {
        wired(g, bufs[k].Get(), view(2));
        tick(g, kSettle);
    }
    check(introCurveInfo().worlds == kTable, "C7.sixteen-worlds");
    wired(g, bufs[kTable].Get(), view(2));   // the first pair (drawn long ago) goes
    tick(g, kSettle);
    check(wired(g, bufs[kTable].Get(), view(2)).armed, "C7.the-newcomer-is-learned: armed once read");
    check(introCurveInfo().worlds == kTable && introCurveInfo().evicted == 1, "C7.still-sixteen-worlds", fmt("%u worlds", introCurveInfo().worlds));
    releaseStrip();
    check(refs(g.dev.Get()) == devBefore, "C7.no-reference-left-on-the-device: seventeen copies later", fmt("%lu then %lu", devBefore, refs(g.dev.Get())));
    g_note.clear();
}

// ---------------------------------------------------------------------------------------------------------------------------------
// C8  EXPIRY
// ---------------------------------------------------------------------------------------------------------------------------------
void case8(Gpu& g) {
    fresh();
    ComPtr<ID3D11Buffer> a = makeCb(g, kCap2), b = makeCb(g, kCap1);
    wired(g, a.Get(), view(0));       // frame 0
    tick(g, kSettle);
    check(introCurveInfo().worlds == 1, "C8.learned");
    tick(g, kExpire - kSettle);       // frame 180: 180 frames since it was drawn
    check(introCurveInfo().entries == 1 && introCurveInfo().expired == 0, "C8.still-there-after-180-frames: the limit is more than 180");
    tick(g, 1);                       // frame 181
    IntroCurveInfo i = introCurveInfo();
    check(i.entries == 0 && i.expired == 1, "C8.forgotten-after-181-frames", fmt("%u entries %llu expired", i.entries, static_cast<unsigned long long>(i.expired)));
    check(!wired(g, a.Get(), view(0)).armed, "C8.a-forgotten-pair-is-learned-afresh: its next draw is the game's");
    tick(g, kSettle);
    check(wired(g, a.Get(), view(0)).armed, "C8.and-armed-again-once-read");

    // A pair drawn within the window is kept and its clock starts again.
    g_note = "drawn within the window";
    fresh();
    wired(g, b.Get(), view(0));       // frame 0
    tick(g, 150);
    check(wired(g, b.Get(), view(0)).armed, "C8.armed-at-150");
    tick(g, 180);                     // frame 330: 180 since the last draw
    check(introCurveInfo().entries == 1, "C8.drawing-it-restarts-the-clock: alive 180 frames after the last draw");
    tick(g, 1);                       // frame 331
    check(introCurveInfo().entries == 0 && introCurveInfo().expired == 1, "C8.and-gone-181-frames-after-the-last-draw");

    // What a pair holds goes with it: a copy that never got read (no context to read it with).
    g_note = "a copy that was never read";
    fresh();
    {
        wired(g, a.Get(), view(3));
        ID3D11Buffer* st = introCurveStageForTest(a.Get(), view(3));
        check(st != nullptr, "C8.copy-in-hand");
        if (st) st->AddRef();
        for (uint32_t n = 0; n < kExpire; ++n) introCurveTick(nullptr, false);
        check(introCurveInfo().entries == 1 && introCurveInfo().staging == 1, "C8.not-expired-yet-with-no-context");
        introCurveTick(nullptr, false);
        i = introCurveInfo();
        check(i.entries == 0 && i.staging == 0 && i.expired == 1, "C8.expired-without-a-context: forgotten all the same",
              fmt("%u entries %u staging", i.entries, i.staging));
        if (st) {
            const ULONG left = st->Release();
            check(left == 0, "C8.expired-staging-buffer-released", fmt("%lu left", left));
        }
    }
    g_note.clear();
}

// ---------------------------------------------------------------------------------------------------------------------------------
// C9  RETIREMENT
// ---------------------------------------------------------------------------------------------------------------------------------
void case9(Gpu& g) {
    // A learned pair armed twice, and a pair still being copied: the first rendered scene.
    fresh();
    check(beginLog(L"ic_c9a"), "C9.log: the scratch log opens");
    ComPtr<ID3D11Buffer> a = makeCb(g, kCap2), b = makeCb(g, kStock), c = makeCb(g, kCap1);
    learn(g, a.Get(), view(0));
    learn(g, b.Get(), view(1));
    wired(g, a.Get(), view(0));
    wired(g, a.Get(), view(0));
    wired(g, c.Get(), view(2));   // copying when the scene arrives
    ID3D11Buffer* st = introCurveStageForTest(c.Get(), view(2));
    check(st != nullptr, "C9.copy-in-hand-at-the-scene");
    if (st) st->AddRef();
    const ULONG aRefs = refs(a.Get());
    tick(g, 1, true);
    IntroCurveInfo i = introCurveInfo();
    check(i.retired && !i.wanted, "C9.retired-and-not-wanted");
    check(i.entries == 0 && i.staging == 0, "C9.table-emptied-at-retirement", fmt("%u entries %u staging", i.entries, i.staging));
    if (st) {
        const ULONG left = st->Release();
        check(left == 0, "C9.staging-buffer-released-at-retirement", fmt("%lu left", left));
    }
    check(!wired(g, a.Get(), view(0)).asked && !rawDraw(g, a.Get(), view(0)) && introCurveInfo().entries == 0,
          "C9.nothing-after-retirement: not asked, not armed, no entry, even for a pair that was armed");
    tick(g, 5);
    tick(g, 1, true);
    check(introCurveInfo().retired && !introCurveInfo().wanted, "C9.retired-for-good: later ticks do not bring it back");
    check(refs(a.Get()) == aRefs, "C9.no-reference-left-on-the-games-buffer", fmt("%lu then %lu", aRefs, refs(a.Get())));
    const std::string log = endLog(L"ic_c9a");
    check(count(log, "a rendered scene arrived") == 1, "C9.said-once", fmt("%zu", count(log, "a rendered scene arrived")));
    check(has(log, "splash curve: a rendered scene arrived -- the intro is over and this stands down for the session. It learned 2 composite(s) "
                   "(1 world-space, 1 flat) and armed 2 strip draw(s)."),
          "C9.what-it-did: the counts (the pair still being copied is not counted)");

    // At curvature 0 it retires without a word; with the strip asked for and nothing ever seen, it says so.
    g_note = "curvature 0";
    fresh();
    setCurvature("0");
    check(beginLog(L"ic_c9b"), "C9.log: the scratch log opens");
    tick(g, 20);
    tick(g, 1, true);
    check(introCurveInfo().retired, "C9.retired-at-curvature-0-too");
    setCurvature("0.3");
    check(!introCurveInfo().wanted && !wired(g, a.Get(), view(0)).asked, "C9.not-wanted-after-retirement-at-0.3-either: it is for good");
    check(!has(endLog(L"ic_c9b"), "splash curve"), "C9.silent-at-curvature-0: no line when it was never wanted");

    g_note = "wanted, nothing seen";
    fresh();
    check(beginLog(L"ic_c9c"), "C9.log: the scratch log opens");
    tick(g, 10);
    tick(g, 1, true);
    const std::string never = endLog(L"ic_c9c");
    check(has(never, "splash curve: a rendered scene arrived and no game-placed composite was ever seen"), "C9.never-seen-line: the strip was asked for and no composite came",
          never.substr(0, 200));
    check(count(never, "splash curve:") == 1, "C9.never-seen-said-once");

    // Shutdown releases the table, and is not a retirement.
    g_note = "shutdown";
    fresh();
    {
        ComPtr<ID3D11Buffer> d = makeCb(g, kCap2);
        learn(g, d.Get(), view(0));
        wired(g, d.Get(), view(1));
        ID3D11Buffer* held = introCurveStageForTest(d.Get(), view(1));
        check(held != nullptr, "C9.copy-in-hand-at-shutdown");
        if (held) held->AddRef();
        introCurveShutdown();
        i = introCurveInfo();
        check(i.entries == 0 && i.staging == 0 && !i.retired, "C9.shutdown-empties-the-table: and is not a retirement", fmt("%u entries %u staging", i.entries, i.staging));
        if (held) {
            const ULONG left = held->Release();
            check(left == 0, "C9.staging-buffer-released-at-shutdown", fmt("%lu left", left));
        }
        introCurveShutdown();   // twice is fine
        check(!wired(g, d.Get(), view(0)).armed && introCurveInfo().entries == 1, "C9.after-shutdown-it-learns-afresh");
    }
    g_note.clear();
}

// ---------------------------------------------------------------------------------------------------------------------------------
// C10  FAULTS
// ---------------------------------------------------------------------------------------------------------------------------------
void case10(Gpu& g) {
    // A fault in the draw path.
    fresh();
    check(beginLog(L"ic_c10a"), "C10.log: the scratch log opens");
    ComPtr<ID3D11Buffer> a = makeCb(g, kCap2);
    learn(g, a.Get(), view(0));
    check(wired(g, a.Get(), view(0)).armed, "C10.armed-before-the-fault");
    setShadow(kVsHash, view(1));
    const bool armed = introCurveOnComposite(badCtx(), 'X', 6, 1);
    IntroCurveInfo i = introCurveInfo();
    check(!armed, "C10.draw-fault-is-the-games-draw: false, nothing armed");
    check(i.standDown && !i.wanted, "C10.draw-fault-stands-this-down");
    check(introCurveGain() == 0.0f && introCurveToward() == 0, "C10.draw-fault-leaves-nothing-armed");
    check(panelCurveSurfaceWanted() && !panelCurveSurfaceInfo().standDown, "C10.the-strip-is-not-stood-down: its own flag is untouched");
    check(!wired(g, a.Get(), view(0)).armed && !rawDraw(g, a.Get(), view(0)), "C10.stood-down-nothing-armed: even for a pair learned before");
    ComPtr<ID3D11Buffer> fresh1 = makeCb(g, kCap1);
    rawDraw(g, fresh1.Get(), view(5));
    check(introCurveInfo().entries == 1, "C10.stood-down-learns-nothing: no entry is made", fmt("%u entries", introCurveInfo().entries));
    tick(g, 3);
    i = introCurveInfo();
    check(i.staging == 0 && i.entries == 0, "C10.stood-down-lets-go: what it held is released at the next tick", fmt("%u staging %u entries", i.staging, i.entries));
    tick(g, 1, true);
    const std::string logA = endLog(L"ic_c10a");
    check(count(logA, "FEATURE-DISABLED introCurve exhausted its fault budget") == 1, "C10.budget-line-once: the guard's line, for this module", fmt("%zu", count(logA, "FEATURE-DISABLED introCurve")));
    check(count(logA, "exhausted its fault budget") == 1, "C10.no-other-budget-spent: only this module's line", fmt("%zu", count(logA, "exhausted its fault budget")));
    check(count(logA, "splash curve: a fault stood this down for the session") == 1, "C10.own-line-once: this module's own line");
    check(has(logA, "a fault had already stood it down"), "C10.retirement-line-says-so: after a fault");

    // A fault in the readback: the copy was issued with a good context, the read is attempted through a bad one.
    g_note = "the readback faults";
    fresh();
    check(beginLog(L"ic_c10b"), "C10.log: the scratch log opens");
    {
        ComPtr<ID3D11Buffer> b = makeCb(g, kCap2), c = makeCb(g, kCap1);
        wired(g, b.Get(), view(0));
        wired(g, c.Get(), view(1));
        ID3D11Buffer* held = introCurveStageForTest(b.Get(), view(0));
        check(held != nullptr, "C10.copy-in-hand");
        if (held) held->AddRef();
        tick(g, kSettle - 1);
        introCurveTick(badCtx(), false);   // due now: the read goes through the bad context
        i = introCurveInfo();
        check(i.standDown && !i.wanted, "C10.readback-fault-stands-this-down");
        check(i.worlds == 0, "C10.readback-fault-arms-nothing", fmt("%u worlds", i.worlds));
        check(!wired(g, b.Get(), view(0)).armed, "C10.nothing-armed-after-the-readback-fault");
        tick(g, 1);
        i = introCurveInfo();
        check(i.staging == 0 && i.entries == 0, "C10.readback-fault-lets-go: both copies released at the next tick", fmt("%u staging %u entries", i.staging, i.entries));
        if (held) {
            const ULONG left = held->Release();
            check(left == 0, "C10.readback-fault-staging-buffer-released", fmt("%lu left", left));
        }
        check(panelCurveSurfaceWanted(), "C10.the-strip-still-wanted-after-the-readback-fault");
    }
    const std::string logB = endLog(L"ic_c10b");
    check(count(logB, "FEATURE-DISABLED introCurve exhausted its fault budget") == 1 && count(logB, "exhausted its fault budget") == 1,
          "C10.readback-budget-line-once-and-only-this-one");
    check(count(logB, "splash curve: a fault stood this down for the session") == 1, "C10.readback-own-line-once");
    g_note.clear();
}

// ---------------------------------------------------------------------------------------------------------------------------------
// C11  THE LOG
// ---------------------------------------------------------------------------------------------------------------------------------
void case11(Gpu& g) {
    // The two lines word for word, the flat one with the movie's stock constants.
    fresh();
    check(beginLog(L"ic_c11a"), "C11.log: the scratch log opens");
    {
        ComPtr<ID3D11Buffer> a = makeCb(g, kStock);
        learn(g, a.Get(), view(0));
        wired(g, a.Get(), view(0));
        Cb same = cbOf(kCap2);
        same.f[15] = -same.f[15];   // cb2[3].w and cb2[4].w now share a sign
        same.f[0] = 3.0f;
        ComPtr<ID3D11Buffer> b = makeCb(g, same.f);
        learn(g, b.Get(), view(1));
    }
    const std::string logA = endLog(L"ic_c11a");
    check(has(logA, "splash curve: this composite stays as the game drew it, because the constants read as a screen-space placement (cb2[3] is zero and "
                    "w is a constant 1), not a world-space panel. Its cb2: 512 288 0 0 | -0.000368732 0 0 0 | 0 0.000373413 0 0 | 0 0 0 0 | 0.193907 0 0 1."),
          "C11.flat-line: the reason and the 20 floats, word for word");
    check(has(logA, "splash curve: the game-placed composite (VS EF103A7C) reads as a world-space panel: half-width 3.000 m, depth toward the viewer -1 "
                    "(cb2[3].w 0.997, cb2[4].w 3.761); +x runs to the viewer's left, so u runs against x; drawn as a 64-column strip at curvature 0.300."),
          "C11.world-line-with-minus-one: half-width and direction from the buffer, not constants");
    check(count(logA, "splash curve:") == 2, "C11.one-line-per-pair", fmt("%zu lines", count(logA, "splash curve:")));

    // The columns and the curvature in the world line are the strip's live settings.
    g_note = "columns and curvature";
    fresh();
    check(beginLog(L"ic_c11b"), "C11.log: the scratch log opens");
    {
        setColumns(128);
        setCurvature("0.7");
        ComPtr<ID3D11Buffer> a = makeCb(g, kCap1);
        learn(g, a.Get(), view(0));
    }
    const std::string logB = endLog(L"ic_c11b");
    check(has(logB, "drawn as a 128-column strip at curvature 0.700."), "C11.columns-and-curvature-are-live: the strip's own settings, as read");

    // A table that turns over -- every third pair a world, the rest flat -- writes twelve lines about the FLAT pairs and then says so; a world line
    // is never capped, the one learned after the cap included (the last pair, the 22nd, is a world); the counts stay exact (8 worlds and 14 flats, so
    // a count of one in the other's place is seen).
    g_note = "a table that turns over";
    fresh();
    check(beginLog(L"ic_c11c"), "C11.log: the scratch log opens");
    {
        std::vector<ComPtr<ID3D11Buffer>> bufs;
        for (uint32_t k = 0; k < kFlatLines + 10; ++k) {
            bufs.push_back(makeCb(g, k % 3 == 0 ? kCap2 : kStock));
            learn(g, bufs.back().Get(), view(0));
        }
    }
    const IntroCurveInfo i = introCurveInfo();
    tick(g, 1, true);
    const std::string logC = endLog(L"ic_c11c");
    check(count(logC, "this composite stays as the game drew it") == kFlatLines && count(logC, "reads as a world-space panel") == 8,
          "C11.twelve-flat-lines-then-no-more: the cap is on the flat lines; every world line is written",
          fmt("%zu flat lines, %zu world lines", count(logC, "this composite stays as the game drew it"), count(logC, "reads as a world-space panel")));
    check(count(logC, "12 lines about composites left as the game drew them are written; the rest are not (a world-space one is always written, and the "
                      "counts in the retirement line stay exact).") == 1,
          "C11.cap-said-once: and says a world-space line is always written", fmt("%zu", count(logC, "12 lines about composites left as the game drew them")));
    check(logC.rfind("reads as a world-space panel") != std::string::npos && logC.find("the rest are not") != std::string::npos &&
              logC.rfind("reads as a world-space panel") > logC.find("the rest are not"),
          "C11.a-world-line-after-the-cap: the pair learned after the cap has its line");
    check(i.learned == kFlatLines + 10 && i.evicted == 6, "C11.counts-stay-exact: every learn counted, every eviction counted",
          fmt("%llu learned %llu evicted", static_cast<unsigned long long>(i.learned), static_cast<unsigned long long>(i.evicted)));
    check(has(logC, "It learned 22 composite(s) (8 world-space, 14 flat) and armed 0 strip draw(s)."), "C11.retirement-counts-are-exact");
    g_note.clear();
}

// ---------------------------------------------------------------------------------------------------------------------------------
// C12  THE WIRING
// ---------------------------------------------------------------------------------------------------------------------------------
void case12(Gpu& g) {
    fresh();
    ComPtr<ID3D11Buffer> left = makeCb(g, kCap2);
    Cb narrow3 = cbOf(kCap1);
    narrow3.f[0] = 3.0f;
    ComPtr<ID3D11Buffer> right = makeCb(g, narrow3.f);
    Cb same = cbOf(kCap2);
    same.f[15] = -same.f[15];
    ComPtr<ID3D11Buffer> odd = makeCb(g, same.f);
    ComPtr<ID3D11Buffer> twin = makeCb(g, kCap1);   // the other eye of the splash: the same half-width and direction as the first
    learn(g, left.Get(), view(0));
    learn(g, right.Get(), view(0));
    learn(g, odd.Get(), view(0));
    learn(g, twin.Get(), view(0));

#ifndef INTRO_CURVE_RIG_REAL_STRIP
    g_strip.clear();
#endif
    g_draws.clear();
    const PanelCurveSurfaceInfo before = panelCurveSurfaceInfo();
    const Wired l = wired(g, left.Get(), view(0));
    const Wired r = wired(g, right.Get(), view(0));
    const Wired o = wired(g, odd.Get(), view(0));
    check(l.armed && l.gain == kHalfWidth && l.toward == 1, "C12.left-eye-numbers: 4.44444 m, toward the viewer");
    check(r.armed && r.gain == 3.0f && r.toward == 1, "C12.right-eye-its-own-half-width: 3 m, not a constant", fmt("%.5f", static_cast<double>(r.gain)));
    check(o.armed && o.gain == kHalfWidth && o.toward == -1, "C12.same-sign-w-the-other-direction: -1", fmt("%d", o.toward));
    check(l.reverseU && r.reverseU && o.reverseU && !l.reverseUAfter && !r.reverseUAfter && !o.reverseUAfter,
          "C12.the-u-direction-is-handed-with-them: against x for each, and cleared by the end of the draw");
    const PanelCurveSurfaceInfo after = panelCurveSurfaceInfo();
    check(after.drawn - before.drawn == 3, "C12.each-armed-draw-is-one-strip-draw", fmt("%llu", static_cast<unsigned long long>(after.drawn - before.drawn)));
    check(after.reversed, "C12.the-strip-in-hand-runs-u-against-x");
#ifndef INTRO_CURVE_RIG_REAL_STRIP
    check(g_strip.size() == 3 && g_strip[0].gain == kHalfWidth && g_strip[0].toward == 1 && g_strip[1].gain == 3.0f && g_strip[1].toward == 1 &&
              g_strip[2].gain == kHalfWidth && g_strip[2].toward == -1,
          "C12.the-strip-is-handed-the-numbers: in the order the draws were made", fmt("%zu calls", g_strip.size()));
    check(g_strip[0].reverseU && g_strip[1].reverseU && g_strip[2].reverseU, "C12.the-strip-is-handed-the-u-direction: all three, against x");
#else
    // The real strip: three different numbers, three builds; each draw is one (indices, 1, 0, 0, 0) through the draw function it was handed;
    // and the two eyes of the splash -- the same numbers -- share ONE strip, as the movie's do (nothing rebuilds at the cut from the movie).
    check(after.built - before.built == 3, "C12.different-numbers-are-different-strips: three builds", fmt("%llu", static_cast<unsigned long long>(after.built - before.built)));
    check(g_draws.size() == 3 && g_draws[0].count > 0 && g_draws[0].instances == 1 && g_draws[0].start == 0 && g_draws[0].base == 0 && g_draws[0].startInstance == 0,
          "C12.the-strip-is-drawn-through-the-draw-function: (indices, 1, 0, 0, 0)", fmt("%zu draws", g_draws.size()));
    wired(g, left.Get(), view(0));
    const PanelCurveSurfaceInfo pivot = panelCurveSurfaceInfo();
    for (int n = 0; n < 4; ++n) {
        wired(g, left.Get(), view(0));
        wired(g, twin.Get(), view(0));
    }
    const PanelCurveSurfaceInfo shared = panelCurveSurfaceInfo();
    check(shared.built == pivot.built && shared.drawn - pivot.drawn == 8, "C12.both-eyes-share-one-strip: the same numbers rebuild nothing",
          fmt("%llu builds %llu draws", static_cast<unsigned long long>(shared.built - pivot.built), static_cast<unsigned long long>(shared.drawn - pivot.drawn)));
#endif

    // The numbers hold for the whole armed draw -- the splash dim's re-issue asks again inside it -- and are zero after.
    g_note = "inside the armed draw";
    bindCb(g, left.Get());
    setShadow(kVsHash, view(0));
    check(introCurveOnComposite(g.ctx.Get(), 'X', 6, 1), "C12.armed");
    check(introCurveGain() == kHalfWidth && introCurveToward() == 1 && introCurveReverseU() && introCurveGain() == kHalfWidth && introCurveToward() == 1 &&
              introCurveReverseU(),
          "C12.numbers-hold-inside-the-draw: asked twice, the same, the u direction with them");
    introCurveEndDraw();
    check(introCurveGain() == 0.0f && introCurveToward() == 0 && !introCurveReverseU(), "C12.zero-after-end-draw");
    introCurveEndDraw();   // twice is fine
    check(introCurveGain() == 0.0f && introCurveToward() == 0 && !introCurveReverseU(), "C12.end-draw-twice-is-fine");

    // A draw that is not armed leaves nothing armed, even after one that was left armed.
    g_note = "a draw after an armed one that was not disarmed";
    check(introCurveOnComposite(g.ctx.Get(), 'X', 6, 1) && introCurveReverseU(), "C12.armed-again");
    check(!rawDraw(g, left.Get(), view(0), 'N', 6, 1), "C12.not-the-composite");
    check(introCurveGain() == 0.0f && introCurveToward() == 0 && !introCurveReverseU(), "C12.a-draw-that-is-not-armed-clears-the-numbers");
    ComPtr<ID3D11Buffer> st = makeCb(g, kStock);
    learn(g, st.Get(), view(4));
    check(rawDraw(g, left.Get(), view(0)) && introCurveReverseU(), "C12.armed-once-more");
    check(!rawDraw(g, st.Get(), view(4)), "C12.a-flat-pair-is-not-armed");
    check(introCurveGain() == 0.0f && introCurveToward() == 0 && !introCurveReverseU(), "C12.a-flat-pair-clears-the-numbers");
    check(rawDraw(g, left.Get(), view(0)) && introCurveReverseU(), "C12.armed-yet-again");
    check(!rawDraw(g, left.Get(), view(9)), "C12.a-new-pair-is-not-armed");
    check(introCurveGain() == 0.0f && introCurveToward() == 0 && !introCurveReverseU(), "C12.a-new-pair-clears-the-numbers");
    g_note.clear();
    introCurveEndDraw();
}

// ---------------------------------------------------------------------------------------------------------------------------------
// C13  THE CUT: a flat pair is copied again
// ---------------------------------------------------------------------------------------------------------------------------------
void case13(Gpu& g) {
    // Nothing says the cut from the movie to the splash gives the composite new objects: the pair may be the SAME buffer and the SAME surface, holding the
    // movie's stock bytes and then, from frame `s`, the splash's world capture (written before that frame's draw). A flat pair is copied again 60 frames
    // after its last copy, at its next draw, so the copies are made on frames 0, 60, 120 ... and read four frames on; the first copy at or after `s` is
    // the one that sees the capture, and the draws are the game's until it has settled -- and flat, not copying, while it is pending.
    struct Cut {
        uint32_t s;
        const char* name;
    };
    const Cut cuts[] = {{1, "the capture written a frame after the first copy"},
                        {59, "written a frame before the second copy"},
                        {60, "written on the frame of the second copy"},
                        {61, "written a frame after the second copy"},
                        {90, "written between the second and the third"},
                        {121, "written a frame after the third copy"}};
    for (const Cut& cut : cuts) {
        g_note = cut.name;
        fresh();
        const std::wstring tag = L"ic_c13_" + std::to_wstring(cut.s);
        check(beginLog(tag.c_str()), "C13.log: the scratch log opens");
        ComPtr<ID3D11Buffer> cb = makeCb(g, kStock);
        const ULONG refsBefore = refs(cb.Get());
        const ULONG devBefore = refs(g.dev.Get());
        Flyer fl(g, cb.Get(), view(0));

        const uint32_t due = ((cut.s + kRecheck - 1) / kRecheck) * kRecheck;   // the copy that sees the capture: the first at or after s
        const uint32_t armedFrom = due + kSettle;                              // ... and the first draw after it has settled
        const uint32_t last = armedFrom + 20;
        uint32_t wrongArm = UINT32_MAX, wrongState = UINT32_MAX, flips = 0, armedDraws = 0;
        std::string stateDetail;
        bool prev = false;
        float gain = 0.0f;
        int toward = 0;
        unsigned notReversed = 0;
        for (uint32_t f = 0; f < last; ++f) {
            if (f == cut.s) setCb(g, cb.Get(), kCap2);   // the game writes the splash's capture into the movie's own buffer
            const Wired w = fl.draw();
            const IntroCurveInfo i = introCurveInfo();
            if (w.armed != (f >= armedFrom) && wrongArm == UINT32_MAX) wrongArm = f;
            if (w.armed != prev) ++flips;
            prev = w.armed;
            if (w.armed) {
                ++armedDraws;
                gain = w.gain;
                toward = w.toward;
                if (!w.reverseU) ++notReversed;
            }
            const bool landed = f >= armedFrom;
            const uint32_t k = f / kRecheck;
            const bool pending = !landed && f - k * kRecheck < kSettle && k * kRecheck <= due;   // a copy in flight: the first, or a re-read
            const unsigned wantCopying = f < kSettle ? 1u : 0u;
            const unsigned wantFlats = (landed || f < kSettle) ? 0u : 1u;
            const unsigned wantWorlds = landed ? 1u : 0u;
            const unsigned wantStaging = pending ? 1u : 0u;
            if ((i.copying != wantCopying || i.flats != wantFlats || i.worlds != wantWorlds || i.staging != wantStaging || i.entries != 1) &&
                wrongState == UINT32_MAX) {
                wrongState = f;
                stateDetail = fmt("frame %u: %u copying %u flats %u worlds %u staging %u entries; wanted %u %u %u %u", f, i.copying, i.flats, i.worlds, i.staging,
                                  i.entries, wantCopying, wantFlats, wantWorlds, wantStaging);
            }
            fl.next();
        }
        const IntroCurveInfo fin = introCurveInfo();
        check(wrongArm == UINT32_MAX, "C13.armed-on-the-frame-the-reread-has-settled: not before, not after",
              fmt("first wrong draw at frame %u; wanted armed from frame %u", wrongArm, armedFrom));
        check(flips == 1 && armedDraws == last - armedFrom, "C13.no-flicker: the draws change once, from the game's to the strip's, and stay",
              fmt("%u changes, %u armed draws", flips, armedDraws));
        check(wrongState == UINT32_MAX, "C13.flat-while-the-copy-is-pending: no change of state until a read says otherwise", stateDetail);
        check(gain == kHalfWidth && toward == 1, "C13.numbers-of-the-capture: 4.44444 m, toward the viewer", fmt("%.7f %d", static_cast<double>(gain), toward));
        check(notReversed == 0, "C13.the-reread-carries-the-u-direction: against x, as a first learn of the same capture does", fmt("%u armed draws with u with x", notReversed));
        check(fin.worlds == 1 && fin.flats == 0 && fin.learned == 2 && fin.staging == 0, "C13.learned-twice-and-nothing-held: a flat reading, then a world one",
              fmt("%u worlds %u flats %llu learned %u staging", fin.worlds, fin.flats, static_cast<unsigned long long>(fin.learned), fin.staging));
        check(fin.rereads == due / kRecheck && fin.flatToWorld == 1, "C13.counters: the re-reads, and the one verdict that changed",
              fmt("%llu re-reads (wanted %u), %llu flat-to-world", static_cast<unsigned long long>(fin.rereads), due / kRecheck,
                  static_cast<unsigned long long>(fin.flatToWorld)));
        check(fl.watch.copies == 1 + due / kRecheck, "C13.copies-issued: the first, and one every 60 frames until the capture was read, none after",
              fmt("%u copies, wanted %u", fl.watch.copies, 1 + due / kRecheck));
        check(fl.watch.stuck == 0 && fl.watch.swapped == 0, "C13.every-staging-buffer-given-back: one at a time, each released when read",
              fmt("%u stuck, %u swapped", fl.watch.stuck, fl.watch.swapped));

        tick(g, 1, true);   // the first rendered scene: the retirement line says what happened
        fl.watch.look();
        fl.watch.done();
        releaseStrip();
        check(refs(cb.Get()) == refsBefore, "C13.no-reference-left-on-the-games-buffer", fmt("%lu then %lu", refsBefore, refs(cb.Get())));
        check(refs(g.dev.Get()) == devBefore, "C13.no-reference-left-on-the-device", fmt("%lu then %lu", devBefore, refs(g.dev.Get())));
        const std::string log = endLog(tag.c_str());
        check(count(log, "splash curve:") == 3 && count(log, "read again") == 0, "C13.three-lines: the flat reading, the world one, the retirement; the flat re-reads say nothing",
              fmt("%zu lines", count(log, "splash curve:")));
        check(count(log, "this composite stays as the game drew it, because the constants read as a screen-space placement") == 1, "C13.the-flat-line-once");
        check(count(log, "splash curve: the game-placed composite (VS EF103A7C) reads as a world-space panel: half-width 4.444 m, depth toward the viewer +1 "
                         "(cb2[3].w -0.997, cb2[4].w 3.761); +x runs to the viewer's left, so u runs against x; drawn as a 64-column strip at "
                         "curvature 0.300.") == 1,
              "C13.the-world-line-once: the same line a first learn writes");
        check(has(log, fmt("It learned 2 composite(s) (1 world-space, 1 flat) and armed 20 strip draw(s), %u re-read(s), 1 flat-to-world change(s).", due / kRecheck)),
              "C13.retirement-clause: the re-reads and the change, said when there were re-reads");
    }
    g_note.clear();
}

// ---------------------------------------------------------------------------------------------------------------------------------
// C14  STAYING FLAT
// ---------------------------------------------------------------------------------------------------------------------------------
void case14(Gpu& g) {
    // (a) The movie's stock bytes for 600 frames: read again every 60, read the same, and said once.
    g_note = "stock bytes for 600 frames";
    fresh();
    check(beginLog(L"ic_c14a"), "C14.log: the scratch log opens");
    {
        ComPtr<ID3D11Buffer> cb = makeCb(g, kStock);
        const ULONG refsBefore = refs(cb.Get());
        const ULONG devBefore = refs(g.dev.Get());
        Flyer fl(g, cb.Get(), view(0));
        unsigned armed = 0, notFlat = 0, maxStaging = 0;
        for (uint32_t f = 0; f <= 600; ++f) {
            const Wired w = fl.draw();
            const IntroCurveInfo i = introCurveInfo();
            if (w.armed) ++armed;
            if (f >= kSettle && !(i.flats == 1 && i.worlds == 0 && i.copying == 0 && i.entries == 1)) ++notFlat;
            if (i.staging > maxStaging) maxStaging = i.staging;
            if (f < 600) fl.next();
        }
        for (uint32_t n = 0; n < kSettle; ++n) fl.next();   // the copy of frame 600 settles
        const IntroCurveInfo i = introCurveInfo();
        check(armed == 0 && notFlat == 0, "C14.stays-flat: never armed, one flat pair at every frame, whatever the re-reads",
              fmt("%u armed draws, %u frames not flat", armed, notFlat));
        check(fl.watch.copies == 1 + 600 / kRecheck, "C14.copies-issued: the first and one every 60 frames, 1 + floor(600 / 60)",
              fmt("%u copies, wanted %u", fl.watch.copies, 1 + 600 / kRecheck));
        check(i.rereads == 600 / kRecheck && i.flatToWorld == 0 && i.learned == 1, "C14.counters: ten re-reads, no change, one pair learned",
              fmt("%llu re-reads %llu changes %llu learned", static_cast<unsigned long long>(i.rereads), static_cast<unsigned long long>(i.flatToWorld),
                  static_cast<unsigned long long>(i.learned)));
        check(maxStaging <= 1 && i.staging == 0, "C14.one-staging-buffer-at-a-time: and none held once they have settled", fmt("%u at most, %u at the end", maxStaging, i.staging));
        check(fl.watch.stuck == 0 && fl.watch.swapped == 0, "C14.every-staging-buffer-given-back", fmt("%u stuck, %u swapped", fl.watch.stuck, fl.watch.swapped));
        tick(g, 1, true);
        fl.watch.look();
        fl.watch.done();
        releaseStrip();
        check(refs(cb.Get()) == refsBefore, "C14.no-reference-left-on-the-games-buffer", fmt("%lu then %lu", refsBefore, refs(cb.Get())));
        check(refs(g.dev.Get()) == devBefore, "C14.no-reference-left-on-the-device", fmt("%lu then %lu", devBefore, refs(g.dev.Get())));
    }
    const std::string logA = endLog(L"ic_c14a");
    check(count(logA, "this composite stays as the game drew it, because the constants read as a screen-space placement") == 1 && count(logA, "read again") == 0,
          "C14.no-extra-line: ten re-reads that read the same say nothing", fmt("%zu lines", count(logA, "splash curve:")));
    check(has(logA, "It learned 1 composite(s) (0 world-space, 1 flat) and armed 0 strip draw(s), 10 re-read(s), 0 flat-to-world change(s)."),
          "C14.retirement-clause: ten re-reads and no change, in that order");

    // (b) A re-read that gives another reason says so once, and says nothing more while it keeps giving that one.
    g_note = "re-reads that give other reasons";
    fresh();
    check(beginLog(L"ic_c14b"), "C14.log: the scratch log opens");
    {
        Cb shortCb = cbOf(kCap2);
        shortCb.f[12] = shortCb.f[13] = shortCb.f[14] = shortCb.f[15] = 0.01f;
        Cb nanCb = cbOf(kCap2);
        nanCb.f[4] = std::nanf("");
        ComPtr<ID3D11Buffer> cb = makeCb(g, kStock);
        Flyer fl(g, cb.Get(), view(0));
        unsigned armed = 0;
        for (uint32_t f = 0; f <= 480; ++f) {
            if (f == 30) setCb(g, cb.Get(), shortCb.f);   // read at 60 and at every period after it: a short column
            if (f == 250) setCb(g, cb.Get(), kStock);     // read at 300: the movie's stock again
            if (f == 370) setCb(g, cb.Get(), nanCb.f);    // read at 420: a NaN
            if (fl.draw().armed) ++armed;
            if (f < 480) fl.next();
        }
        for (uint32_t n = 0; n < kSettle; ++n) fl.next();
        const IntroCurveInfo i = introCurveInfo();
        check(armed == 0 && i.flats == 1 && i.worlds == 0 && i.rereads == 8 && i.flatToWorld == 0, "C14.other-reasons-stay-flat: eight re-reads, none armed",
              fmt("%u armed %u flats %llu re-reads", armed, i.flats, static_cast<unsigned long long>(i.rereads)));
        fl.watch.done();
    }
    const std::string logB = endLog(L"ic_c14b");
    const std::string again = "this composite, read again, stays as the game drew it, because ";
    const size_t p1 = logB.find(again + "cb2[3] is not a unit-scale column (its length is outside 0.5 to 2). Its cb2: ");
    const size_t p2 = logB.find(again + "the constants read as a screen-space placement (cb2[3] is zero and w is a constant 1), not a world-space panel. Its cb2: ");
    const size_t p3 = logB.find(again + "a constant is not a finite number. Its cb2: ");
    check(count(logB, "read again") == 3 && count(logB, "stays as the game drew it, because ") == 4, "C14.a-new-reason-is-said-once-each: three lines in eight re-reads",
          fmt("%zu re-read lines, %zu flat lines", count(logB, "read again"), count(logB, "stays as the game drew it, because ")));
    check(p1 != std::string::npos && p2 != std::string::npos && p3 != std::string::npos && p1 < p2 && p2 < p3,
          "C14.the-reasons-and-the-floats-in-order: a short column, the stock again, a NaN, word for word");

    // (c) The cap on the flat lines holds for a re-read's line, and not for a world line: twelve flat pairs fill it, one of them then re-reads as
    // another reason (the 13th flat line: said not to be written) and another as the splash's capture (a world line: written all the same).
    g_note = "the flat cap and a re-read";
    fresh();
    check(beginLog(L"ic_c14c"), "C14.log: the scratch log opens");
    {
        std::vector<ComPtr<ID3D11Buffer>> pairs;
        for (uint32_t k = 0; k < kFlatLines; ++k) pairs.push_back(makeCb(g, kStock));
        Cb nanCb = cbOf(kCap2);
        nanCb.f[4] = std::nanf("");
        unsigned worldArmed = 0, strayArmed = 0;
        for (uint32_t f = 0; f < 2 * kRecheck; ++f) {
            if (f == 30) {
                setCb(g, pairs[0].Get(), nanCb.f);   // read at 60: another class of reason
                setCb(g, pairs[1].Get(), kCap2);     // read at 60: the splash's capture
            }
            for (uint32_t k = 0; k < kFlatLines; ++k) {
                const bool armed = wired(g, pairs[k].Get(), view(0)).armed;
                if (k == 1 && armed) ++worldArmed;
                if (k != 1 && armed) ++strayArmed;
            }
            tick(g);
        }
        const IntroCurveInfo i = introCurveInfo();
        check(i.rereads == kFlatLines && i.flatToWorld == 1 && i.worlds == 1 && i.flats == kFlatLines - 1 && i.learned == kFlatLines + 1 && i.staging == 0,
              "C14.counters-with-twelve-pairs: every pair re-read once, one changed", fmt("%llu re-reads %llu changes %u worlds %u flats", static_cast<unsigned long long>(i.rereads),
                                                                                         static_cast<unsigned long long>(i.flatToWorld), i.worlds, i.flats));
        check(worldArmed == 2 * kRecheck - (kRecheck + kSettle) && strayArmed == 0, "C14.only-the-changed-pair-is-armed: from frame 64", fmt("%u, %u stray", worldArmed, strayArmed));
    }
    const std::string logC = endLog(L"ic_c14c");
    check(count(logC, "this composite stays as the game drew it") == kFlatLines && count(logC, "read again") == 0,
          "C14.a-re-read-line-is-held-to-the-cap: the thirteenth flat line is not written", fmt("%zu first lines, %zu re-read lines", count(logC, "this composite stays as the game drew it"),
                                                                                                count(logC, "read again")));
    check(count(logC, "12 lines about composites left as the game drew them are written; the rest are not") == 1, "C14.cap-said-once");
    check(count(logC, "reads as a world-space panel") == 1 && logC.find("reads as a world-space panel") > logC.find("the rest are not"),
          "C14.a-world-line-is-written-past-the-cap: the pair that became a world", fmt("%zu world lines", count(logC, "reads as a world-space panel")));

    // (d) A buffer too small to copy cannot be copied again either: no copy is made at any frame, and the reason is not said at every period.
    g_note = "a buffer too small to copy";
    fresh();
    check(beginLog(L"ic_c14d"), "C14.log: the scratch log opens");
    {
        ComPtr<ID3D11Buffer> tiny = makeCb(g, kCap2, 64);
        Flyer fl(g, tiny.Get(), view(0));
        unsigned armed = 0;
        for (uint32_t f = 0; f < 200; ++f) {
            if (fl.draw().armed) ++armed;
            fl.next();
        }
        const IntroCurveInfo i = introCurveInfo();
        check(armed == 0 && fl.watch.copies == 0 && i.staging == 0 && i.rereads == 0 && i.flats == 1 && i.entries == 1,
              "C14.small-buffer-no-copy-at-any-frame: flat, nothing held, nothing read", fmt("%u armed %u copies %u staging %llu re-reads", armed, fl.watch.copies, i.staging,
                                                                                            static_cast<unsigned long long>(i.rereads)));
    }
    const std::string logD = endLog(L"ic_c14d");
    check(count(logD, "splash curve:") == 1 && has(logD, "its constant buffer is 64 bytes, under the 80 the shader declares"),
          "C14.small-buffer-said-once: not at every period", fmt("%zu lines", count(logD, "splash curve:")));

    // (e) A re-read that cannot be read for several periods -- the ticks carry no context -- is not copied over: the pair holds ONE buffer, and the
    // next copy waits for it (the period alone would not say so: a copy is due again 60 frames after the last, whether or not that was ever read).
    g_note = "a re-read pending while the ticks have no context";
    fresh();
    check(beginLog(L"ic_c14e"), "C14.log: the scratch log opens");
    {
        ComPtr<ID3D11Buffer> cb = makeCb(g, kStock);
        const ULONG devBefore = refs(g.dev.Get());
        Flyer fl(g, cb.Get(), view(0));
        for (uint32_t f = 0; f < kRecheck; ++f) {
            fl.draw();
            fl.next();
        }
        fl.draw();   // frame 60: the copy again
        unsigned wrong = 0;
        for (uint32_t n = 0; n < 3 * kRecheck; ++n) {   // frames 61 to 240: still drawn every frame, the ticks have nothing to read with
            fl.nextWithoutContext();
            fl.draw();
            const IntroCurveInfo i = introCurveInfo();
            if (fl.watch.copies != 2 || i.staging != 1 || i.rereads != 0 || i.flats != 1) ++wrong;
        }
        check(wrong == 0 && fl.watch.swapped == 0 && fl.watch.stuck == 0, "C14.no-second-copy-over-one-in-flight: one buffer, three periods on",
              fmt("%u frames wrong, %u copies, %u swapped, %u stuck", wrong, fl.watch.copies, fl.watch.swapped, fl.watch.stuck));
        fl.next();   // a context at last: the copy of frame 60 is read
        IntroCurveInfo i = introCurveInfo();
        check(i.rereads == 1 && i.staging == 0 && i.flats == 1 && fl.watch.stuck == 0, "C14.read-when-a-context-arrives: the buffer given back",
              fmt("%llu re-reads %u staging %u stuck", static_cast<unsigned long long>(i.rereads), i.staging, fl.watch.stuck));
        fl.draw();   // a period has long passed since that copy: the next waits for nothing else
        i = introCurveInfo();
        check(fl.watch.copies == 3 && i.staging == 1, "C14.the-next-copy-waited-only-for-that-one", fmt("%u copies %u staging", fl.watch.copies, i.staging));
        fl.next();
        fl.next();
        fl.next();
        fl.next();
        check(introCurveInfo().staging == 0 && fl.watch.stuck == 0 && fl.watch.swapped == 0, "C14.and-given-back-in-turn");
        fl.watch.done();
        check(refs(g.dev.Get()) == devBefore, "C14.no-reference-left-on-the-device", fmt("%lu then %lu", devBefore, refs(g.dev.Get())));
    }
    endLog(L"ic_c14e");
    g_note.clear();
}

// ---------------------------------------------------------------------------------------------------------------------------------
// C15  FAILED RE-READS
// ---------------------------------------------------------------------------------------------------------------------------------
void case15(Gpu& g) {
    // (a) A re-read whose map fails -- a deferred context cannot Map(READ), so it fails the way a lost device's would, an error code and not a fault.
    g_note = "a re-read that cannot be mapped";
    fresh();
    check(beginLog(L"ic_c15a"), "C15.log: the scratch log opens");
    {
        ComPtr<ID3D11DeviceContext> deferred;
        g.dev->CreateDeferredContext(0, &deferred);
        check(deferred != nullptr, "C15.deferred-context: the rig can make a context whose Map(READ) fails");
        ComPtr<ID3D11Buffer> cb = makeCb(g, kStock);
        Flyer fl(g, cb.Get(), view(0));
        for (uint32_t f = 0; f < kRecheck; ++f) {   // frames 0 to 59: flat from frame 4
            fl.draw();
            fl.next();
        }
        fl.draw();   // frame 60: the copy again
        IntroCurveInfo i = introCurveInfo();
        check(fl.watch.copies == 2 && i.staging == 1 && i.flats == 1 && i.copying == 0, "C15.copy-issued-on-frame-60: the pair stays flat while it is pending",
              fmt("%u copies %u staging %u flats %u copying", fl.watch.copies, i.staging, i.flats, i.copying));
        if (deferred) {
            for (uint32_t n = 0; n < kSettle; ++n) fl.next(deferred.Get());   // frames 61 to 64: the read goes through the context that cannot map
        }
        i = introCurveInfo();
        check(i.flats == 1 && i.worlds == 0 && i.staging == 0 && i.rereads == 1 && i.flatToWorld == 0 && !i.standDown && i.wanted,
              "C15.unreadable-reread-leaves-the-pair-flat: not armed, not a fault, nothing held",
              fmt("%u flats %u worlds %u staging %llu re-reads", i.flats, i.worlds, i.staging, static_cast<unsigned long long>(i.rereads)));
        check(fl.watch.stuck == 0, "C15.unreadable-reread-staging-buffer-released", fmt("%u stuck", fl.watch.stuck));
        // The buffer now holds the splash's capture. The pair is copied again a period after the failed copy (frame 120) and armed when that has settled.
        setCb(g, cb.Get(), kCap2);
        uint32_t wrongArm = UINT32_MAX;
        for (uint32_t f = fl.frame; f < 2 * kRecheck + kSettle + 6; ++f) {
            const Wired w = fl.draw();
            if (w.armed != (f >= 2 * kRecheck + kSettle) && wrongArm == UINT32_MAX) wrongArm = f;
            fl.next();
        }
        i = introCurveInfo();
        check(wrongArm == UINT32_MAX, "C15.tried-again-a-period-later: armed from frame 124, not before", fmt("first wrong draw at frame %u", wrongArm));
        check(fl.watch.copies == 3 && i.rereads == 2 && i.flatToWorld == 1 && i.worlds == 1 && i.staging == 0, "C15.counters-after-the-second-try",
              fmt("%u copies %llu re-reads %llu changes", fl.watch.copies, static_cast<unsigned long long>(i.rereads), static_cast<unsigned long long>(i.flatToWorld)));
        check(fl.watch.stuck == 0 && fl.watch.swapped == 0, "C15.every-staging-buffer-given-back");
        fl.watch.done();
    }
    const std::string logA = endLog(L"ic_c15a");
    check(count(logA, "this composite, read again, stays as the game drew it, because the copy of its constants could not be read back.") == 1 &&
              count(logA, "splash curve:") == 3,
          "C15.unreadable-reread-said-once: a reason of another class, then the world line", fmt("%zu lines", count(logA, "splash curve:")));

    // (b) A re-read whose readback faults: the copy was issued with a good context, the read goes through a bad one.
    g_note = "the re-read's readback faults";
    fresh();
    check(beginLog(L"ic_c15b"), "C15.log: the scratch log opens");
    {
        ComPtr<ID3D11Buffer> flat = makeCb(g, kStock), world = makeCb(g, kCap1);
        Flyer fl(g, flat.Get(), view(0));
        for (uint32_t f = 0; f < kRecheck; ++f) {
            fl.draw();
            wired(g, world.Get(), view(1));
            fl.next();
        }
        fl.draw();   // frame 60: the flat pair is copied again
        check(wired(g, world.Get(), view(1)).armed, "C15.the-world-pair-is-armed-before-the-fault");
        check(fl.watch.copies == 2 && introCurveInfo().staging == 1, "C15.copy-in-hand", fmt("%u copies %u staging", fl.watch.copies, introCurveInfo().staging));
        for (uint32_t n = 0; n < kSettle - 1; ++n) fl.next();
        fl.next(badCtx());   // frame 64: due, and read through the bad context
        IntroCurveInfo i = introCurveInfo();
        check(i.standDown && !i.wanted, "C15.readback-fault-stands-this-down");
        check(!wired(g, world.Get(), view(1)).armed && !wired(g, flat.Get(), view(0)).armed, "C15.nothing-armed-after-the-fault: the game's draws, the world pair's too");
        fl.next();
        i = introCurveInfo();
        check(i.staging == 0 && i.entries == 0 && fl.watch.stuck == 0, "C15.readback-fault-lets-go: the re-read's buffer released at the next tick",
              fmt("%u staging %u entries %u stuck", i.staging, i.entries, fl.watch.stuck));
        check(panelCurveSurfaceWanted() && !panelCurveSurfaceInfo().standDown, "C15.the-strip-is-not-stood-down");
        fl.watch.done();
    }
    const std::string logB = endLog(L"ic_c15b");
    check(count(logB, "FEATURE-DISABLED introCurve exhausted its fault budget") == 1 && count(logB, "exhausted its fault budget") == 1 &&
              count(logB, "splash curve: a fault stood this down for the session") == 1,
          "C15.readback-fault-said-once: the guard's line for this module and this module's own");

    // (c) The draw that would issue the re-read faults: the pair is flat, 60 frames old, and its draw goes through a bad context.
    g_note = "the draw that would copy again faults";
    fresh();
    check(beginLog(L"ic_c15c"), "C15.log: the scratch log opens");
    {
        ComPtr<ID3D11Buffer> flat = makeCb(g, kStock), world = makeCb(g, kCap1);
        Flyer fl(g, flat.Get(), view(0));
        for (uint32_t f = 0; f < kRecheck; ++f) {
            fl.draw();
            wired(g, world.Get(), view(1));
            fl.next();
        }
        check(wired(g, world.Get(), view(1)).armed, "C15.the-world-pair-is-armed-before-the-fault");
        setShadow(kVsHash, view(0));
        const bool armed = introCurveOnComposite(badCtx(), 'X', 6, 1);   // frame 60
        fl.watch.look();
        IntroCurveInfo i = introCurveInfo();
        check(!armed && i.standDown && !i.wanted && i.staging == 0 && fl.watch.copies == 1 && i.rereads == 0,
              "C15.draw-fault-stands-this-down: no copy was issued, nothing is held",
              fmt("armed %d, %u staging, %u copies, %llu re-reads", armed ? 1 : 0, i.staging, fl.watch.copies, static_cast<unsigned long long>(i.rereads)));
        check(introCurveGain() == 0.0f && introCurveToward() == 0, "C15.draw-fault-leaves-nothing-armed");
        check(!wired(g, world.Get(), view(1)).armed && !wired(g, flat.Get(), view(0)).armed, "C15.draw-fault-the-games-draws-from-then-on: the world pair's too");
        fl.next();
        i = introCurveInfo();
        check(i.entries == 0 && i.staging == 0, "C15.draw-fault-lets-go", fmt("%u entries %u staging", i.entries, i.staging));
        check(panelCurveSurfaceWanted() && !panelCurveSurfaceInfo().standDown, "C15.the-strip-is-not-stood-down-by-the-draw-fault");
        fl.watch.done();
    }
    const std::string logC = endLog(L"ic_c15c");
    check(count(logC, "FEATURE-DISABLED introCurve exhausted its fault budget") == 1 && count(logC, "splash curve: a fault stood this down for the session") == 1,
          "C15.draw-fault-said-once");
    g_note.clear();
}

// ---------------------------------------------------------------------------------------------------------------------------------
// C16  A WORLD IS FINAL
// ---------------------------------------------------------------------------------------------------------------------------------
void case16(Gpu& g) {
    // A world pair is never copied again: the game may come to hold other bytes in the same buffer, and the pair keeps the reading it has (the known
    // limit intro_curve.h says) until it is forgotten -- which is how a verdict ends.
    g_note = "a world pair for 600 frames";
    fresh();
    check(beginLog(L"ic_c16"), "C16.log: the scratch log opens");
    ComPtr<ID3D11Buffer> cb = makeCb(g, kCap2);
    const ULONG refsBefore = refs(cb.Get());
    const ULONG devBefore = refs(g.dev.Get());
    Flyer fl(g, cb.Get(), view(0));
    unsigned armed = 0, missed = 0, wrongNumbers = 0;
    for (uint32_t f = 0; f <= 600; ++f) {
        if (f == 100) setCb(g, cb.Get(), kStock);   // the buffer is reused for another placement: the pair keeps what it read
        if (f == 150) setCb(g, cb.Get(), cbRunningRight(kCap2).f);   // and again, for a placement whose +x runs the other way: its u direction stays too
        const Wired w = fl.draw();
        if (w.armed) {
            ++armed;
            if (w.gain != kHalfWidth || w.toward != 1 || !w.reverseU) ++wrongNumbers;
        } else if (f >= kSettle) {
            ++missed;
        }
        if (f < 600) fl.next();
    }
    IntroCurveInfo i = introCurveInfo();
    check(armed == 600 - kSettle + 1 && missed == 0 && wrongNumbers == 0, "C16.armed-for-the-life-of-the-pair: frames 4 to 600, with the first reading's numbers",
          fmt("%u armed %u missed %u wrong numbers", armed, missed, wrongNumbers));
    check(fl.watch.copies == 1 && i.rereads == 0 && i.flatToWorld == 0 && i.staging == 0 && i.worlds == 1 && i.flats == 0,
          "C16.never-copied-again: one copy in 600 frames, whatever the buffer comes to hold",
          fmt("%u copies %llu re-reads %u staging %u worlds %u flats", fl.watch.copies, static_cast<unsigned long long>(i.rereads), i.staging, i.worlds, i.flats));

    // Expiry is how the verdict ends: 180 frames unseen is still a pair, 181 is not, and the same objects are a stranger when they come back.
    g_note = "a world pair forgotten";
    for (uint32_t n = 0; n < kExpire; ++n) fl.next();
    check(introCurveInfo().entries == 1 && introCurveInfo().expired == 0, "C16.still-there-180-frames-after-its-last-draw");
    fl.next();
    check(introCurveInfo().entries == 0 && introCurveInfo().expired == 1, "C16.forgotten-181-frames-after-its-last-draw");
    check(!fl.draw().armed && fl.watch.copies == 2, "C16.a-forgotten-pair-is-read-afresh: its next draw is the game's, and copies again",
          fmt("%u copies", fl.watch.copies));
    for (uint32_t n = 0; n < kSettle; ++n) fl.next();
    i = introCurveInfo();
    const Wired again = fl.draw();
    check(i.flats == 0 && i.worlds == 1 && again.armed && !again.reverseU && again.gain == kHalfWidth && again.toward == 1,
          "C16.and-reads-what-the-buffer-holds-now: the placement running right, a world again with the u direction it has now, not the old one",
          fmt("%u flats %u worlds, armed %d, reverseU %d", i.flats, i.worlds, again.armed ? 1 : 0, again.reverseU ? 1 : 0));
    tick(g, 1, true);
    fl.watch.look();
    fl.watch.done();
    releaseStrip();
    check(refs(cb.Get()) == refsBefore, "C16.no-reference-left-on-the-games-buffer", fmt("%lu then %lu", refsBefore, refs(cb.Get())));
    check(refs(g.dev.Get()) == devBefore, "C16.no-reference-left-on-the-device", fmt("%lu then %lu", devBefore, refs(g.dev.Get())));
    const std::string log = endLog(L"ic_c16");
    check(count(log, "splash curve:") == 3 && count(log, "reads as a world-space panel") == 2 && count(log, "read again") == 0 &&
              count(log, "+x runs to the viewer's left, so u runs against x") == 1 && count(log, "+x runs to the viewer's right, so u runs with x") == 1,
          "C16.three-lines: the world line, the world line for the same objects later (running the other way), the retirement",
          fmt("%zu lines", count(log, "splash curve:")));
    check(has(log, "It learned 2 composite(s) (2 world-space, 0 flat) and armed 598 strip draw(s)."),
          "C16.retirement-line-has-no-clause-without-re-reads: the text it always had");
    g_note.clear();
}

// ---------------------------------------------------------------------------------------------------------------------------------
// C17  THE DIRECTION: which way u runs
// ---------------------------------------------------------------------------------------------------------------------------------
void case17(Gpu& g) {
    // (a) Every real placement runs +x to the viewer's left, and is armed with reverseU true (C1 holds each of the four against the whole line). The
    // same placements with the x column negated -- what the on-foot screen's would be -- run it to the right and are armed with reverseU false. Seven
    // pairs (four running left, three running right) side by side, drawn in turn again and again: each draw has ITS pair's direction, the strip in hand
    // follows it, and the log says each.
    g_note = "left-running and right-running pairs side by side";
    fresh();
    check(beginLog(L"ic_c17a"), "C17.log: the scratch log opens");
    {
        // Four left-running pairs (every real vector) and THREE right-running ones, so that a log line that says the opposite is not a count that
        // happens to come out alike.
        const float* const bases[4] = {kCap2, kCap2b, kCap1, kCap1b};
        std::vector<ComPtr<ID3D11Buffer>> lefts, rights;
        for (const float* b : bases) lefts.push_back(makeCb(g, b));
        for (size_t k = 0; k < 3; ++k) rights.push_back(makeCb(g, cbRunningRight(bases[k]).f));
        for (size_t k = 0; k < 4; ++k) {
            wired(g, lefts[k].Get(), view(0));
            if (k < 3) wired(g, rights[k].Get(), view(1));
        }
        tick(g, kSettle);
        unsigned wrong = 0, drawn = 0, stripWrong = 0;
        for (int round = 0; round < 3; ++round) {
            tick(g);
            for (size_t k = 0; k < 4; ++k) {
                const Wired l = wired(g, lefts[k].Get(), view(0));
                const bool leftStrip = panelCurveSurfaceInfo().reversed;
                ++drawn;
                if (!(l.armed && l.reverseU && !l.reverseUAfter)) ++wrong;
                if (!leftStrip) ++stripWrong;
                if (k < 3) {
                    const Wired r = wired(g, rights[k].Get(), view(1));
                    const bool rightStrip = panelCurveSurfaceInfo().reversed;
                    ++drawn;
                    if (!(r.armed && !r.reverseU && !r.reverseUAfter)) ++wrong;
                    if (rightStrip) ++stripWrong;
                }
            }
        }
        check(drawn == 21 && wrong == 0, "C17.each-pair-keeps-its-own-direction: four running left (against x), three running right (with x), drawn in turn",
              fmt("%u draws, %u wrong", drawn, wrong));
        check(stripWrong == 0, "C17.the-strip-in-hand-follows-the-draw: against x after a left-running pair, with x after a right-running one", fmt("%u wrong", stripWrong));
        const IntroCurveInfo i = introCurveInfo();
        check(i.worlds == 7 && i.armed == 21, "C17.all-seven-pairs-are-worlds", fmt("%u worlds %llu armed", i.worlds, static_cast<unsigned long long>(i.armed)));
        releaseStrip();
    }
    const std::string logA = endLog(L"ic_c17a");
    check(count(logA, "+x runs to the viewer's left, so u runs against x; drawn as a 64-column strip") == 4 &&
              count(logA, "+x runs to the viewer's right, so u runs with x; drawn as a 64-column strip") == 3 && count(logA, "reads as a world-space panel") == 7,
          "C17.the-log-says-which-way: four left, three right, one line a pair", fmt("%zu left, %zu right", count(logA, "so u runs against x"), count(logA, "so u runs with x")));

    // (b) A pair read edge-on (the x column and the centre cancel) is no reading: it stays as the game drew it, with the reason and the 20 floats in the
    // log, and is read again like any flat pair. When the game then writes a good placement into the same buffer -- running either way -- the re-read
    // arms it, with that placement's own u direction, on the frame its copy settles (the copy of frame 60 lands at 64). A pair that stays edge-on
    // says nothing more, however often it is read.
    g_note = "a pair read edge-on";
    fresh();
    check(beginLog(L"ic_c17b"), "C17.log: the scratch log opens");
    {
        const Cb edge = cbEdgeOn(kCap2);
        ComPtr<ID3D11Buffer> toLeft = makeCb(g, edge.f), toRight = makeCb(g, edge.f), stays = makeCb(g, edge.f);
        unsigned wrongEarly = 0, wrongLate = 0;
        for (uint32_t f = 0; f < 140; ++f) {
            if (f == 30) {
                setCb(g, toLeft.Get(), kCap2);                        // a good placement, +x running left
                setCb(g, toRight.Get(), cbRunningRight(kCap2).f);     // a good placement, +x running right
            }
            const Wired a = wired(g, toLeft.Get(), view(0));
            const Wired b = wired(g, toRight.Get(), view(1));
            const Wired c = wired(g, stays.Get(), view(2));
            if (f < 64) {
                if (a.armed || b.armed || c.armed || a.reverseU || b.reverseU || c.reverseU) ++wrongEarly;
            } else if (!(a.armed && a.reverseU && b.armed && !b.reverseU && !c.armed && !c.reverseU)) {
                ++wrongLate;
            }
            if (f == 10) {
                const IntroCurveInfo i = introCurveInfo();
                check(i.flats == 3 && i.worlds == 0 && i.learned == 3, "C17.edge-on-is-flat: three pairs, none armed", fmt("%u flats %u worlds", i.flats, i.worlds));
            }
            tick(g);
        }
        check(wrongEarly == 0, "C17.nothing-armed-until-the-reread-has-settled: frames 0 to 63", fmt("%u wrong draws", wrongEarly));
        check(wrongLate == 0, "C17.armed-from-frame-64-each-with-its-own-direction: left-running against x, right-running with x, the one still edge-on flat",
              fmt("%u wrong draws", wrongLate));
        const IntroCurveInfo i = introCurveInfo();
        check(i.worlds == 2 && i.flats == 1 && i.flatToWorld == 2, "C17.two-changed-one-stayed", fmt("%u worlds %u flats %llu changes", i.worlds, i.flats,
                                                                                                    static_cast<unsigned long long>(i.flatToWorld)));
        releaseStrip();
    }
    const std::string logB = endLog(L"ic_c17b");
    check(count(logB, "this composite stays as the game drew it, because the placement's +x cannot be told to run left or right (the panel is seen "
                      "edge-on, or its x column and its centre cancel). Its cb2: ") == 3,
          "C17.edge-on-says-why-and-says-its-floats: one line a pair", fmt("%zu lines", count(logB, "cannot be told to run left or right")));
    check(count(logB, "read again") == 0, "C17.and-the-reads-after-that-say-nothing-more: the same reason is not said again");
    check(count(logB, "+x runs to the viewer's left, so u runs against x; drawn as a 64-column strip") == 1 &&
              count(logB, "+x runs to the viewer's right, so u runs with x; drawn as a 64-column strip") == 1,
          "C17.the-pairs-that-recovered-say-which-way: one line each");
    g_note.clear();
}

// ---------------------------------------------------------------------------------------------------------------------------------
// The runner.
// ---------------------------------------------------------------------------------------------------------------------------------
struct Case {
    const char* id;
    void (*run)(Gpu&);
};
const Case kCases[] = {{"C1", case1}, {"C2", case2}, {"C3", case3}, {"C4", case4}, {"C5", case5},   {"C6", case6},   {"C7", case7},   {"C8", case8},
                       {"C9", case9}, {"C10", case10}, {"C11", case11}, {"C12", case12}, {"C13", case13}, {"C14", case14}, {"C15", case15}, {"C16", case16},
                       {"C17", case17}};

void removeScratch() {
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((g_dir + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) DeleteFileW((g_dir + L"\\" + fd.cFileName).c_str());
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(g_dir.c_str());
}

int run(const std::set<std::string>& only, bool keep) {
    wchar_t temp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, temp);
    g_dir = std::wstring(temp) + L"edvr_ictest_" + std::to_wstring(GetCurrentProcessId());
    CreateDirectoryW(g_dir.c_str(), nullptr);
    g_runtimeProfile = RuntimeProfile::Vr;

    Gpu gpu = makeGpu();
    check(gpu.ok, "C0.gpu: a WARP device is available");
    if (!gpu.ok) {
        std::printf("FAIL: intro curve module: no WARP device\n");
        if (!keep) removeScratch();
        return 1;
    }
    check(reportSystemD3D11Only("intro_curve_module_test"), "C0.system-d3d11: the process runs on System32's d3d11.dll only (no EDVR proxy beside the rig)");

    unsigned ran = 0;
    for (const Case& c : kCases) {
        if (!only.empty() && !only.count(c.id)) continue;
        c.run(gpu);
        ++ran;
    }
    check(ran == (only.empty() ? sizeof(kCases) / sizeof(kCases[0]) : only.size()), "C0.cases: every case asked for ran", fmt("%u cases", ran));

    introCurveShutdown();
    gpu = Gpu{};
    if (Log::get().isOpen()) Log::get().close();
    if (keep) std::printf("the scratch directory is kept: %s\n", narrow(g_dir).c_str());
    else removeScratch();
    return g_failed ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
    bool self = false, dry = false, keep = false;
    std::set<std::string> only;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--self-test")) self = true;
        else if (!std::strcmp(argv[i], "--dry-run")) dry = true;
        else if (!std::strcmp(argv[i], "--keep")) keep = true;
        else if (!std::strcmp(argv[i], "--only") && i + 1 < argc) {
            std::string list = argv[++i];
            for (size_t at = 0; at <= list.size();) {
                size_t comma = list.find(',', at);
                if (comma == std::string::npos) comma = list.size();
                if (comma > at) only.insert(list.substr(at, comma - at));
                at = comma + 1;
            }
        } else {
            std::fputs("usage: intro_curve_module_test --self-test [--only C1,C4] [--keep] | --dry-run\n", stderr);
            return 2;
        }
    }
    if (dry) {
        std::puts("intro_curve_module_test: --dry-run: nothing run, nothing written");
        return 0;
    }
    if (!self) {
        std::fputs("usage: intro_curve_module_test --self-test [--only C1,C4] [--keep] | --dry-run\n", stderr);
        return 2;
    }
    const int rc = run(only, keep);
    if (rc == 0) {
        std::printf("PASS: %u intro curve module checks (%zu cases: the splash's recogniser on WARP)\n", g_checks, only.empty() ? sizeof(kCases) / sizeof(kCases[0]) : only.size());
    } else {
        std::printf("FAIL: intro curve module: %u of %u checks failed, %zu distinct\n", g_failed, g_checks, g_failCount.size());
    }
    return rc;
}
