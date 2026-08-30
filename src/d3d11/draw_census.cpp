#include "draw_census.h"

#include <cstdio>   // _snprintf_s: the sampled-slot list is built before logging
#include <cstdlib>  // _strtoui64: the CB watch's shader hash from config
#include <cstring>  // memcpy: the CB watch's shadow refresh

#include <windows.h>

#include <d3d11.h>

#include "../common/config.h"
#include "../common/guard.h"
#include "../common/log.h"
#include "../common/timing.h"  // the startup schedule's clock
#include "binding_shadow.h"
#include "exposure_fix.h"   // lookupShaderHash: the shader hash registry

namespace edvr {
namespace {

// Three frames: enough that "present in every frame of one census and no
// frame of the other" separates a steady overlay from frame-to-frame churn,
// while two censuses stay near one percent of the default 4 MB log cap.
//
// The DEFAULT, no longer the law: advanced.census_frames raises it to 30 for
// a capture that has to span a progressive build (the FSS body takes longer
// than three frames to tile in, and the interesting frames are all of them).
// Read at census start, clamped, so one census records one configuration.
constexpr uint32_t kCensusFrames = 3;
constexpr uint32_t kCensusFramesMax = 30;

// A frame on foot has been measured at 1174 eye draws, so three frames can
// legitimately want ~3500 lines. The cap exists so a mode this was never
// pointed at cannot eat the log; the end line says when it bit.
//
// Also a default now (advanced.census_lines), because the cap and the frame
// count have to move together: 30 offscreen frames under a 4096-line cap
// records the first eleven and silently drops the tail, which in a BUILD
// capture is the half where the build finishes. The ceiling keeps a typo
// from spending the whole 4 MB log; at ~140 bytes a line, 16384 lines is
// about 2.3 MB, paid only on the sessions that ask for it.
constexpr uint32_t kMaxLines = 4096;
constexpr uint32_t kMaxLinesCeiling = 16384;

// Distinct bound objects seen across one census. The first field capture
// refuted the "a few dozen" estimate this started from: a cockpit census
// interned 160 and missed 1216 more -- Elite runs hundreds of distinct
// eye-sized views and per-state textures through three frames, and the
// table filled before the LATE-frame render targets arrived, which cost the
// exact draws the census existed to name. 512 covers what was measured with
// three times over; past it, tokens degrade to inline resolution (below),
// never to an unusable '?'.
constexpr uint32_t kMaxInterned = 512;

// The startup schedule (advanced.census_at_ms). At most eight moments, each
// naming milliseconds after this session's FIRST frame edge.
//
// WHY A CLOCK AND NOT A KEY OR A SIZE. The three arms that existed before it
// all need something known in advance: a hand on the census key, a journal
// event, or the size of a target somebody has already censused. The startup
// sequence has none. The intro movie's flat phase, the freeze and the
// head-locked phase are all over before a human can react, the first two
// happen before one eye's size has been published to this half at all, and
// census_auto's "two quiet seconds" never arrive for a target that is drawn
// into from the very first frame. A wall clock needs no prior model, which
// during a startup is the whole point (docs/intro-video.md).
//
// Not frames: a loading screen has been measured at 1790 fps and a menu at
// 13, so the same frame number is a different moment on every rig and in
// every session. See common/timing.h -- this is that rule, applied to the
// one instrument that most wants to break it.
constexpr uint32_t kMaxSchedule = 8;

// Everything below runs on the render thread: the draw records come from the
// immediate context's draw hooks (foreign contexts are filtered before the
// census is consulted), and both the arming hotkey and the frame boundary
// live in the Present path. No locks, and none are missing.

// How a bound object can be asked what it is. Views resolve through
// GetResource; resources answer for themselves; a shader answers nothing and
// is worth only its identity.
enum class Kind {
    kView,      // render targets, depth, sampled textures
    kResource,  // constant and vertex buffers, held directly
    kOpaque,    // shaders
};

struct Interned {
    void*        ptr = nullptr;
    ResourceInfo info;
    bool         resolved = false;
};

bool resolveByKind(void* ptr, Kind kind, ResourceInfo* out) {
    if (kind == Kind::kView) return bindingResolve(ptr, out);
    if (kind == Kind::kResource) return bindingResolveResource(ptr, out);
    return false;
}

bool     g_pending = false;      // key pressed, waiting for a frame edge
bool     g_forceOffscreen = false; // the auto arm's rider: this census records
                                 // offscreen draws whatever the ini says,
                                 // because it exists to watch an offscreen build
uint32_t g_framesLeft = 0;       // frames still to record; >0 means capturing
uint32_t g_framesWanted = kCensusFrames;  // this census's length, from config
uint32_t g_maxLines = kMaxLines; // this census's line cap, from config
bool     g_offscreen = false;    // record non-eye draws too, latched at start
uint32_t g_offThisFrame = 0;     // offscreen draws this frame, for the line index
uint32_t g_offDraws = 0;         // offscreen draws this census, for the end line
uint32_t g_copiesThisFrame = 0;  // copies this frame, for the line index
uint32_t g_copies = 0;           // copies this census, for the end line
uint32_t g_dispThisFrame = 0;    // dispatches this frame, for the line index
uint32_t g_dispatches = 0;       // dispatches this census, for the end line
// The startup schedule, read once at the first frame edge -- these are
// moments in THIS session's startup, and re-reading them later would either
// re-fire everything already spent or move a deadline the session has passed.
uint64_t g_scheduleMs[kMaxSchedule] = {0};
uint32_t g_scheduleCount = 0;
uint32_t g_scheduleFired = 0;    // a bitmask over the entries above
uint64_t g_firstFrameMs = 0;     // stamped at the first frame edge
bool     g_scheduleRead = false;
uint32_t g_seq = 0;              // ONE ordinal across every recorded event in
                                 // a frame -- the q= token. The per-kind
                                 // indexes above cannot say whether a copy
                                 // landed between two draws; this can.
// The constant-buffer watch (see the header). Two slots: the watched draw's
// VS b0 and PS b0. The shadow is refreshed by the Map/Unmap tee and dumped
// at each watched draw, so a dump is the bytes the GPU reads for THAT draw.
constexpr uint32_t kCbShadowBytes = 256;
constexpr uint32_t kCbDumpCap = 512;   // dumps per census; four watch slots
                                       // x 2 eyes x 30 frames in the FSS run
struct CbWatch {
    void*    buf = nullptr;     // the buffer object, identity only
    void*    mapped = nullptr;  // its pData between Map and Unmap
    uint32_t bytes = 0;         // ByteWidth, as resolved at registration
    bool     valid = false;     // the shadow holds at least one full write
    uint8_t  shadow[kCbShadowBytes];
};
uint64_t g_cbWatchHash = 0;      // latched at census begin; 0 = off
uint64_t g_cbWatchHash2 = 0;     // optional second hash (comma-separated)

// Which constant-buffer SLOT the watch reads, latched with the hash.
//
// It was b0 and only b0, and twice now that has been the wrong buffer. The
// loading wash's pixel shader reads b2 (docs/loading-scrim.md records the
// dump instrument being unable to reach it), and the intro movie's panel is
// placed by a scale and a transform in the VERTEX shader's b2 --
// EF103A7CB4A8369A, read from its own disassembly, docs/intro-video.md.
// Both times the census reported "no constant buffer" for a draw whose
// entire behaviour was in one, because the column only ever looked at slot
// zero.
//
// 0 keeps the old path exactly: the b0 shadow, which something in the tree
// already hooks. A non-zero slot is read off the context at the draw, the
// same IA-state pattern the PS side has always used.
uint32_t g_cbWatchSlot = 0;

// The DIRECT read, for a buffer the write tee never sees.
//
// The watch shadow is filled from the game's own writes -- the Map/Unmap tee
// and the UpdateSubresource tee, both wired. That is exactly right when the
// question is "what bytes will THIS draw read", and useless when the buffer
// is not written during the window: the dump says "unwritten" and the census
// is spent. The intro movie's panel transform is such a buffer -- two
// 80-byte objects, one per eye, holding the scale and translation the
// composite is placed by, written before any census could arm and reused
// thereafter (docs/intro-video.md).
//
// So: when a dump would say "unwritten", copy the buffer on the GPU instead
// and read it back once the copy has certainly executed. The copy-settle-map
// shape is panel_quad's and quad_probe's, and the settle is why this needs a
// tick of its own -- the census is two frames long and the readback lands
// after it, which the line says.
//
// The write tee stays primary: it is per-DRAW exact, and a copy is not.
constexpr uint32_t kMaxCbReads = 4;
constexpr uint32_t kCbReadSettle = 4;

struct CbRead {
    void*         buf = nullptr;
    ID3D11Buffer* stage = nullptr;
    uint32_t      bytes = 0;
    uint32_t      q = 0;
    uint32_t      ordinal = 0;
    char          letter = 'v';
    uint32_t      dueFrame = 0;   // vscreen's frame counter, not the census's
};
CbRead   g_cbRead[kMaxCbReads];
uint32_t g_cbReadCount = 0;
uint32_t g_lastFrameNo = 0;
CbWatch  g_cbWatch[4];           // [0] = VS b0, [1] = PS b0,
                                 // [2] = CS b0, [3] = CS b1 (dispatches --
                                 // round 21: the reconstruction pair share
                                 // ONE 80-byte cb OBJECT and the per-eye
                                 // rewrite idiom is the engine's own)
uint32_t g_cbDumps = 0;
FaultBudget g_cbBudget("drawCensus.cbWatch", 5);

uint32_t g_censusNo = 0;         // numbers the begin/end lines, so the diff
                                 // tool can pair "absent" with "present"
uint32_t g_frameOrdinal = 0;     // 0-based frame within the running census
uint32_t g_draws = 0;            // draws recorded (kept counting past the cap)
uint32_t g_drawsThisFrame = 0;
uint32_t g_lines = 0;
uint32_t g_overflow = 0;         // intern-table misses
Interned g_tab[kMaxInterned];
uint32_t g_tabCount = 0;

// The table index for a bound object, interning on first sight. Resolution
// happens here, at record time, while the binding is certainly alive --
// resolving at dump time would probe pointers three frames stale, which is
// the exact crash class binding_shadow exists to contain. -1 is "not bound",
// -2 is "table full".
int internOf(void* ptr, Kind kind) {
    if (!ptr) return -1;
    for (uint32_t i = 0; i < g_tabCount; ++i) {
        if (g_tab[i].ptr == ptr) return static_cast<int>(i);
    }
    if (g_tabCount >= kMaxInterned) {
        ++g_overflow;
        return -2;
    }
    Interned& e = g_tab[g_tabCount];
    e.ptr = ptr;
    // Buffers used to be interned by identity alone, because the only resolver
    // was the view one and handing it a buffer reaches GetType through a
    // GetResource-shaped signature. bindingResolveResource is that dance with
    // the first step left out, so a constant buffer now dumps its byte width --
    // the one fact the panel composite's transform is described by ("at most
    // 512 bytes"), and one that used to cost a separate probe to learn.
    // Contents are still not the census's business.
    e.resolved = resolveByKind(ptr, kind, &e.info);
    return static_cast<int>(g_tabCount++);
}

// The token a draw line carries for one bound object: "@N" while the table
// has room, "-" for unbound -- and on a full table the resolved form INLINE
// ("tex512x64f28", "buf256"), so a signature survives the overflow. The
// first field capture wrote '?' instead, and the '?' landed on the late
// render targets and half the sampled slots: the diff ran but could not
// name what it found. Same compact spelling the diff tool normalises the
// id table to, so a slot interned in one census and inline in the other
// still compares equal. Shaders have no inline form, having nothing to
// resolve; measured, buffers intern within the first frames and never
// overflowed.
const char* bindingToken(void* ptr, Kind kind, char* buf, size_t n) {
    if (!ptr) return "-";
    const int id = internOf(ptr, kind);
    if (id >= 0) {
        _snprintf_s(buf, n, _TRUNCATE, "@%d", id);
        return buf;
    }
    ResourceInfo info;
    if (resolveByKind(ptr, kind, &info)) {
        if (info.isTexture2D) {
            _snprintf_s(buf, n, _TRUNCATE, "tex%ux%uf%u", info.a, info.b,
                        info.fmt);
            return buf;
        }
        if (info.isBuffer) {
            _snprintf_s(buf, n, _TRUNCATE, "buf%u", info.a);
            return buf;
        }
    }
    return "?";
}

// The input-assembler and vertex-shader state behind one draw, read straight
// off the context.
//
// Its own budget, not binding_shadow's: a fault here means the IA cannot be
// read, which must not also stop render targets and sampled textures from
// resolving -- those are what every other token on a census line depends on,
// and what the census was built for.
//
// IAGetVertexBuffers and VSGetShader AddRef what they hand back, and both are
// released before this returns. The pointers survive as identities only: the
// context still holds its own reference to anything it has bound, which is
// what makes comparing them afterwards sound. A fault between a Get and its
// Release leaks one reference -- the same bargain bindingResolve documents,
// for the same C2712 reason, capped the same way.
struct DrawState {
    bool     ok = false;      // false leaves the tokens off the line entirely,
                              // which reads as "not measured", not as "none"
    void*    vs = nullptr;
    void*    vb = nullptr;
    uint32_t stride = 0;
    uint32_t offset = 0;
    uint32_t topology = 0;    // D3D11_PRIMITIVE_TOPOLOGY: 4 is a triangle list,
                              // 5 a strip -- how a quad announces itself
};

FaultBudget g_iaBudget("drawCensus.drawState", 5);

void readDrawState(ID3D11DeviceContext* ctx, DrawState* out) {
    if (!ctx) return;
    guardedBudget(g_iaBudget, [&] {
        ID3D11VertexShader* vs = nullptr;
        ctx->VSGetShader(&vs, nullptr, nullptr);
        out->vs = vs;
        if (vs) vs->Release();

        ID3D11Buffer* vb = nullptr;
        UINT stride = 0, offset = 0;
        ctx->IAGetVertexBuffers(0, 1, &vb, &stride, &offset);
        out->vb = vb;
        out->stride = stride;
        out->offset = offset;
        if (vb) vb->Release();

        D3D11_PRIMITIVE_TOPOLOGY topo = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
        ctx->IAGetPrimitiveTopology(&topo);
        out->topology = static_cast<uint32_t>(topo);
        out->ok = true;
    });
}

// --- the constant-buffer watch ----------------------------------------------

// Point one watch slot at the buffer a watched draw has bound. Re-pointing is
// normal (the game can recreate the buffer between zooms); the shadow starts
// over because bytes from the old object are not evidence about the new one.
void cbWatchRegister(uint32_t slot, void* buf) {
    CbWatch& w = g_cbWatch[slot];
    if (w.buf == buf) return;
    w.buf = buf;
    w.mapped = nullptr;
    w.valid = false;
    w.bytes = 0;
    ResourceInfo info;
    if (buf && bindingResolveResource(buf, &info) && info.isBuffer) {
        w.bytes = info.a < kCbShadowBytes ? info.a : kCbShadowBytes;
    }
    static const char kSlotLetter[4] = {'v', 'p', 'x', 'y'};
    char tok[24];
    Log::get().note("DCW register %c cb=%s bytes=%u", kSlotLetter[slot & 3],
                    bindingToken(buf, Kind::kResource, tok, sizeof(tok)),
                    w.bytes);
}

// One shadow dump: the bytes the GPU will read for the draw that asked. All
// floats on one line -- 52 of them for the 208-byte camera block -- because
// the offline diff pairs lines by (frame, stage) and splitting a dump across
// lines is how half a buffer gets compared against the wrong eye.
// Queue a GPU copy of a watched buffer the tee has not seen written, to be
// read back and logged a few frames from now. One per distinct buffer per
// census; the two eyes' objects are distinct, so both are caught.
void cbReadQueue(ID3D11DeviceContext* ctx, uint32_t slot, uint32_t q,
                 char letter) {
    CbWatch& w = g_cbWatch[slot];
    if (!ctx || !w.buf || !w.bytes || g_cbReadCount >= kMaxCbReads) return;
    for (uint32_t i = 0; i < g_cbReadCount; ++i) {
        if (g_cbRead[i].buf == w.buf) return;
    }
    guardedBudget(g_cbBudget, [&] {
        ID3D11Device* dev = nullptr;
        ctx->GetDevice(&dev);
        if (!dev) return;
        D3D11_BUFFER_DESC sd{};
        sd.Usage = D3D11_USAGE_STAGING;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        sd.ByteWidth = w.bytes;
        ID3D11Buffer* stage = nullptr;
        if (SUCCEEDED(dev->CreateBuffer(&sd, nullptr, &stage)) && stage) {
            D3D11_BOX box{};
            box.right = sd.ByteWidth;
            box.bottom = 1;
            box.back = 1;
            ctx->CopySubresourceRegion(stage, 0, 0, 0, 0,
                                       static_cast<ID3D11Resource*>(w.buf), 0,
                                       &box);
            CbRead& r = g_cbRead[g_cbReadCount++];
            r.buf = w.buf;
            r.stage = stage;
            r.bytes = sd.ByteWidth;
            r.q = q;
            r.ordinal = g_frameOrdinal;
            r.letter = letter;
            r.dueFrame = g_lastFrameNo + kCbReadSettle;
        }
        dev->Release();
    });
}

void cbWatchDump(ID3D11DeviceContext* ctx, uint32_t slot, uint32_t q) {
    CbWatch& w = g_cbWatch[slot];
    if (!w.buf || g_cbDumps >= kCbDumpCap) return;
    ++g_cbDumps;
    static const char kSlotLetter[4] = {'v', 'p', 'x', 'y'};
    char tok[24];
    if (!w.valid) {
        Log::get().note("DCW %u q=%u %c cb=%s unwritten -- not written while "
                        "this census ran; reading it directly instead, a DCW "
                        "read line follows in %u frames.",
                        g_frameOrdinal, q, kSlotLetter[slot & 3],
                        bindingToken(w.buf, Kind::kResource, tok, sizeof(tok)),
                        kCbReadSettle);
        cbReadQueue(ctx, slot, q, kSlotLetter[slot & 3]);
        return;
    }
    const uint32_t nf = w.bytes / 4;
    char fl[1000];
    size_t at = 0;
    const float* f = reinterpret_cast<const float*>(w.shadow);
    for (uint32_t i = 0; i < nf && at + 16 < sizeof(fl); ++i) {
        const int m = _snprintf_s(fl + at, sizeof(fl) - at, _TRUNCATE, " %.6g",
                                  static_cast<double>(f[i]));
        if (m < 0) break;
        at += static_cast<size_t>(m);
    }
    fl[at] = '\0';
    Log::get().note("DCW %u q=%u %c cb=%s b=%u h=%016llX f=%s", g_frameOrdinal,
                    q, kSlotLetter[slot & 3],
                    bindingToken(w.buf, Kind::kResource, tok, sizeof(tok)),
                    w.bytes,
                    static_cast<unsigned long long>(fnv1a64(w.shadow, w.bytes)),
                    fl);
}

// A draw running the watched shader: learn its b0 objects, dump their
// shadows. The PS buffer is read off the context here, the IA-state pattern
// -- nothing hooks PSSetConstantBuffers, and two COM calls on the handful of
// watched draws a frame is the whole cost.
void cbWatchOnDraw(ID3D11DeviceContext* ctx, uint32_t q) {
    if (g_cbWatchSlot == 0) {
        cbWatchRegister(0, bindingGet(BindSlot::VsCb0));
    } else {
        // Nothing hooks VSSetConstantBuffers past b0, so a watched slot is
        // asked of the context at the draw -- the pattern the PS side below
        // has always used, and paid only on the handful of matched draws a
        // frame.
        guardedBudget(g_cbBudget, [&] {
            ID3D11Buffer* vb = nullptr;
            ctx->VSGetConstantBuffers(g_cbWatchSlot, 1, &vb);
            cbWatchRegister(0, vb);
            if (vb) vb->Release();
        });
    }
    guardedBudget(g_cbBudget, [&] {
        ID3D11Buffer* pb = nullptr;
        ctx->PSGetConstantBuffers(g_cbWatchSlot, 1, &pb);
        cbWatchRegister(1, pb);
        if (pb) pb->Release();
    });
    cbWatchDump(ctx, 0, q);
    cbWatchDump(ctx, 1, q);
}

// A dispatch running a watched shader: the compute stage's b0 and b1,
// learned and dumped exactly the way the draw form does its stages. Round
// 21 of the black squares: the eye-image dump proved the squares SYMMETRIC
// in the eye textures, so the split is born in the per-eye reconstruction
// dispatches -- whose 80-byte constant block is one shared OBJECT for both
// eyes, the rewrite-per-eye idiom the exposure fix was built around. These
// dumps are the bytes each eye's dispatch actually read.
void cbWatchOnDispatch(ID3D11DeviceContext* ctx, uint32_t q) {
    guardedBudget(g_cbBudget, [&] {
        ID3D11Buffer* cb[2] = {};
        ctx->CSGetConstantBuffers(0, 2, cb);
        cbWatchRegister(2, cb[0]);
        cbWatchRegister(3, cb[1]);
        for (ID3D11Buffer* b : cb) {
            if (b) b->Release();
        }
    });
    cbWatchDump(ctx, 2, q);
    cbWatchDump(ctx, 3, q);
}

void dumpInternTable() {
    // res= is the underlying resource's identity, and it is the column the FSS
    // hunt was missing: an SRV and an RTV over the SAME texture intern as two
    // different @ids, and until two id lines could show one res value there
    // was no way to say "the target the body is drawn into IS the texture the
    // composites sample" from a log. Per-session noise across censuses, which
    // is why the diff tool parses past it and keys on nothing in it.
    for (uint32_t i = 0; i < g_tabCount; ++i) {
        const Interned& e = g_tab[i];
        if (!e.resolved) {
            Log::get().note("DC id @%u ?", i);
        } else if (e.info.isTexture2D) {
            Log::get().note("DC id @%u tex %ux%u fmt=%u res=%p", i, e.info.a,
                            e.info.b, e.info.fmt, e.info.resource);
        } else if (e.info.isBuffer) {
            Log::get().note("DC id @%u buf %u res=%p", i, e.info.a,
                            e.info.resource);
        } else {
            Log::get().note("DC id @%u ?", i);
        }
    }
}

void finish() {
    dumpInternTable();
    Log::get().note("DC end census=%u draws=%u off=%u copies=%u disp=%u "
                    "lines=%u interned=%u overflow=%u truncated=%u",
                    g_censusNo, g_draws, g_offDraws, g_copies, g_dispatches,
                    g_lines, g_tabCount, g_overflow,
                    g_draws > g_lines ? g_draws - g_lines : 0);
}

}  // namespace

bool drawCensusArmed() { return g_pending || g_framesLeft > 0; }

void drawCensusRequest() {
    if (drawCensusArmed()) {
        Log::get().note("DC: the census key was pressed while a census is "
                        "already running; ignored. One at a time.");
        return;
    }
    g_pending = true;
    Log::get().note("DC: census armed -- the next whole frames of eye-texture "
                    "draws will be logged. Diff two of these with "
                    "tools/diff_draw_census.py.");
}

void drawCensusAutoRequest() {
    // Silent when armed: the caller is a per-draw trigger, and the ordinary
    // case -- the build keeps drawing while its own census runs -- must not
    // write a line per draw.
    if (drawCensusArmed()) return;
    g_pending = true;
    g_forceOffscreen = true;
    // The caller logs WHY (the size that tripped it); this side owns only the
    // census mechanics, which the begin line below reports as it always has.
}

// The shared body. The only thing the two entry points disagree about is the
// tag and the index; every binding read below is identical, which is the
// reason an offscreen line can be read with the same eyes as an eye line.
static void recordDraw(ID3D11DeviceContext* ctx, char kind, uint32_t count,
                       uint32_t instances, const char* tag, uint32_t index) {
    // The shared ordinal advances for every event that ARRIVES, before the
    // line cap -- a capped census keeps truthful q values on whatever lines
    // it does write, instead of renumbering the survivors.
    const uint32_t q = g_seq++;
    if (g_lines >= g_maxLines) return;
    ++g_lines;

    char rb[24], db[24], cb[24], s0b[24], s1b[24], s2b[24], s3b[24];
    const char* r = bindingToken(bindingGet(BindSlot::Rtv0), Kind::kView, rb, sizeof(rb));
    const char* d = bindingToken(bindingGet(BindSlot::Dsv0), Kind::kView, db, sizeof(db));
    const char* c = bindingToken(bindingGet(BindSlot::VsCb0), Kind::kResource, cb,
                                 sizeof(cb));
    const char* s0 = bindingToken(bindingGet(BindSlot::PsSrv0), Kind::kView, s0b, sizeof(s0b));
    const char* s1 = bindingToken(bindingGet(BindSlot::PsSrv1), Kind::kView, s1b, sizeof(s1b));
    const char* s2 = bindingToken(bindingGet(BindSlot::PsSrv2), Kind::kView, s2b, sizeof(s2b));
    const char* s3 = bindingToken(bindingGet(BindSlot::PsSrv3), Kind::kView, s3b, sizeof(s3b));

    // The IA/VS tail, present only when the probe answered. Absent means the
    // budget is spent or the read faulted -- never "nothing was bound", which
    // has its own spelling ("-").
    // 128, not the 80 this started at. Every token bindingToken writes is
    // capped at 23 characters by its own buffer, and two of those plus three
    // ten-digit numbers and their labels is 97 -- so 80 truncated its own
    // worst case, silently, in exactly the shape a stride of 4294967295 would
    // have arrived in. Sized from the caps rather than from what buffers
    // "realistically" hold.
    char tail[160] = "";
    DrawState st;
    readDrawState(ctx, &st);
    uint64_t vh = 0;
    if (st.ok) {
        // The shader's content hash beside its pointer token. The pointer
        // is per-session noise the differ rightly ignores; the HASH is
        // stable across sessions and is the one key that cannot collide
        // between two draws running different code -- the geyser hunt
        // (2026-08-23) found particle plumes and rock meshes sharing the
        // entire size-level signature, separable by nothing the census
        // recorded. vh=0 means the shader was created before the hooks
        // went in.
        vh = lookupShaderHash(st.vs);
        char vsb[24], vbb[24];
        _snprintf_s(tail, sizeof(tail), _TRUNCATE,
                    " vs=%s vh=%016llX vb=%s sd=%u of=%u tp=%u",
                    bindingToken(st.vs, Kind::kOpaque, vsb, sizeof(vsb)),
                    static_cast<unsigned long long>(vh),
                    bindingToken(st.vb, Kind::kResource, vbb, sizeof(vbb)),
                    st.stride, st.offset, st.topology);
    }

    // PS slots 4-7, read straight off the context the way the IA tail is.
    //
    // WHY (2026-08-25, the black squares): the census has only ever recorded
    // sampler slots 0-3, so every "the composites' inputs are identical"
    // claim this hunt produced was a claim about FOUR slots. The FSS reveal
    // then turned out to be TILED and PER-EYE -- black squares filling in,
    // seen in one eye and not the other -- and a per-eye tile map bound at
    // t4 or above was invisible to every capture this file ever made, while
    // the DCX lines were recording the per-eye compute chain that builds
    // one. Absent tokens mean the probe did not answer, never "nothing
    // bound", exactly as the IA tail spells it.
    char xt[112] = "";
    char pt[32] = "";
    if (st.ok) {
        ID3D11ShaderResourceView* xs[4] = {};
        ID3D11PixelShader* ps = nullptr;
        bool got = false;
        guardedBudget(g_iaBudget, [&] {
            ctx->PSGetShaderResources(4, 4, reinterpret_cast<ID3D11ShaderResourceView**>(xs));
            ctx->PSGetShader(&ps, nullptr, nullptr);
            got = true;
        });
        if (got) {
            char x0b[24], x1b[24], x2b[24], x3b[24];
            _snprintf_s(xt, sizeof(xt), _TRUNCATE, " x=%s,%s,%s,%s",
                        bindingToken(xs[0], Kind::kView, x0b, sizeof(x0b)),
                        bindingToken(xs[1], Kind::kView, x1b, sizeof(x1b)),
                        bindingToken(xs[2], Kind::kView, x2b, sizeof(x2b)),
                        bindingToken(xs[3], Kind::kView, x3b, sizeof(x3b)));
            // The PIXEL shader's content hash beside the vertex one -- the
            // FSS mask hunt needed the composite's PS named and the census
            // had only ever recorded half the pipeline. ph=0 is a shader
            // created before the hooks, exactly like vh.
            _snprintf_s(pt, sizeof(pt), _TRUNCATE, " ph=%016llX",
                        static_cast<unsigned long long>(lookupShaderHash(ps)));
        }
        for (ID3D11ShaderResourceView* v : xs) {
            if (v) v->Release();
        }
        if (ps) ps->Release();
    }

    // WHERE THE DRAW LANDS, which this has never recorded (2026-08-30).
    //
    // The black-planet hunt reached a draw that is ISSUED for both eyes,
    // six times each, with byte-identical shared inputs -- and produces
    // nothing in one of them. A live skip probe proved it is the draw that
    // paints the body: remove it from the good eye and the body goes black
    // there too. So the pixels are being asked for and not arriving, and
    // the census had no column that could show why.
    //
    // Viewport and scissor are the plainest way that happens: a draw with a
    // viewport of the wrong size, or an empty scissor, runs to completion
    // and rasterises nowhere. Every other channel for this draw has been
    // read and matched -- shaders, textures, index counts, order, render
    // target, and b0 -- which makes the state NOBODY has looked at the
    // place to look.
    //
    // Two COM calls on recorded draws only, alongside the four the sampler
    // probe above already makes. "vp=none" means the probe did not answer,
    // exactly as the absent tokens elsewhere on this line do.
    char vt[112] = "";
    {
        D3D11_VIEWPORT vp[1] = {};
        UINT nvp = 1;
        D3D11_RECT sc[1] = {};
        UINT nsc = 1;
        bool got = false;
        guardedBudget(g_iaBudget, [&] {
            ctx->RSGetViewports(&nvp, vp);
            ctx->RSGetScissorRects(&nsc, sc);
            got = true;
        });
        if (got) {
            char scb[48];
            if (nsc >= 1) {
                _snprintf_s(scb, sizeof(scb), _TRUNCATE, "%ld,%ld-%ld,%ld",
                            sc[0].left, sc[0].top, sc[0].right, sc[0].bottom);
            } else {
                _snprintf_s(scb, sizeof(scb), _TRUNCATE, "off");
            }
            if (nvp >= 1) {
                _snprintf_s(vt, sizeof(vt), _TRUNCATE,
                            " vp=%.0f,%.0f+%.0fx%.0f z=%.3f-%.3f sc=%s",
                            vp[0].TopLeftX, vp[0].TopLeftY, vp[0].Width,
                            vp[0].Height, vp[0].MinDepth, vp[0].MaxDepth, scb);
            } else {
                _snprintf_s(vt, sizeof(vt), _TRUNCATE, " vp=none sc=%s", scb);
            }
        }
    }

    Log::get().note(
        "%s %u #%u %c n=%u i=%u r=%s d=%s c=%s s=%s,%s,%s,%s%s%s%s%s q=%u",
        tag, g_frameOrdinal, index, kind, count, instances, r, d, c, s0, s1,
        s2, s3, tail, xt, pt, vt, q);

    // The CB watch, after the draw's own line so a DCW dump always follows
    // the draw it belongs to. Any recorded draw can match -- DC for the FSS
    // composite, DCO if a future hunt watches an offscreen builder.
    if ((g_cbWatchHash && vh == g_cbWatchHash) ||
        (g_cbWatchHash2 && vh == g_cbWatchHash2)) {
        cbWatchOnDraw(ctx, q);
    }
}

void drawCensusEyeDraw(ID3D11DeviceContext* ctx, char kind, uint32_t count,
                       uint32_t instances, uint32_t eyeDrawIndex) {
    if (g_framesLeft == 0) return;   // pending counts draws only once started
    ++g_draws;
    ++g_drawsThisFrame;
    recordDraw(ctx, kind, count, instances, "DC", eyeDrawIndex);
}

bool drawCensusWantsOffscreen() { return g_offscreen; }

void drawCensusOffDraw(ID3D11DeviceContext* ctx, char kind, uint32_t count,
                       uint32_t instances) {
    if (g_framesLeft == 0 || !g_offscreen) return;
    ++g_offDraws;
    recordDraw(ctx, kind, count, instances, "DCO", g_offThisFrame++);
}

// The direct-read draw record: every token off the calling context, no
// shadow. Round seventeen of the black squares: the ring draws read per-eye
// surfaces NOTHING recorded ever wrote, and the remaining writer classes
// were exactly the ones the census never carried -- indirect draws (slots
// 39/40, GPU-driven like the DispatchIndirect middle round sixteen caught),
// and draws on deferred contexts, whose bindings the owner-context shadow
// can never describe. This function records both, with the same t=/args=
// provenance tails the dispatch line grew.
void drawCensusDrawDirect(ID3D11DeviceContext* ctx, char kind, uint32_t count,
                          uint32_t instances, bool foreignCtx,
                          void* indirectArgs, uint32_t indirectOff) {
    if (g_framesLeft == 0 || !ctx) return;
    ++g_draws;
    ++g_drawsThisFrame;
    const uint32_t q = g_seq++;
    if (g_lines >= g_maxLines) return;
    ++g_lines;

    ID3D11RenderTargetView* rtv = nullptr;
    ID3D11DepthStencilView* dsv = nullptr;
    ID3D11Buffer* cb0 = nullptr;
    ID3D11ShaderResourceView* sr[8] = {};
    ID3D11PixelShader* ps = nullptr;
    bool got = false;
    guardedBudget(g_iaBudget, [&] {
        ctx->OMGetRenderTargets(1, &rtv, &dsv);
        ctx->VSGetConstantBuffers(0, 1, &cb0);
        ctx->PSGetShaderResources(0, 8,
            reinterpret_cast<ID3D11ShaderResourceView**>(sr));
        ctx->PSGetShader(&ps, nullptr, nullptr);
        got = true;
    });

    char rb[24] = "-", db[24] = "-", cb[24] = "-";
    char sb[8][24];
    const char* st[8];
    for (int i = 0; i < 8; ++i) st[i] = "-";
    char pt[32] = "";
    const char* r = "-";
    const char* d = "-";
    const char* c = "-";
    if (got) {
        r = bindingToken(rtv, Kind::kView, rb, sizeof(rb));
        d = bindingToken(dsv, Kind::kView, db, sizeof(db));
        c = bindingToken(cb0, Kind::kResource, cb, sizeof(cb));
        for (int i = 0; i < 8; ++i) {
            st[i] = bindingToken(sr[i], Kind::kView, sb[i], sizeof(sb[i]));
        }
        _snprintf_s(pt, sizeof(pt), _TRUNCATE, " ph=%016llX",
                    static_cast<unsigned long long>(lookupShaderHash(ps)));
    }

    char tail[160] = "";
    DrawState dst2;
    readDrawState(ctx, &dst2);
    if (dst2.ok) {
        char vsb[24], vbb[24];
        _snprintf_s(tail, sizeof(tail), _TRUNCATE,
                    " vs=%s vh=%016llX vb=%s sd=%u of=%u tp=%u",
                    bindingToken(dst2.vs, Kind::kOpaque, vsb, sizeof(vsb)),
                    static_cast<unsigned long long>(lookupShaderHash(dst2.vs)),
                    bindingToken(dst2.vb, Kind::kResource, vbb, sizeof(vbb)),
                    dst2.stride, dst2.offset, dst2.topology);
    }

    char prov[64] = "";
    if (indirectArgs) {
        char ab[24];
        _snprintf_s(prov, sizeof(prov), _TRUNCATE, " args=%s+%u t=%si",
                    bindingToken(indirectArgs, Kind::kResource, ab, sizeof(ab)),
                    indirectOff, foreignCtx ? "f" : "");
    } else if (foreignCtx) {
        _snprintf_s(prov, sizeof(prov), _TRUNCATE, " t=f");
    }

    Log::get().note(
        "DC %u #%u %c n=%u i=%u r=%s d=%s c=%s s=%s,%s,%s,%s%s x=%s,%s,%s,%s%s%s q=%u",
        g_frameOrdinal, g_drawsThisFrame - 1, kind, count, instances, r, d, c,
        st[0], st[1], st[2], st[3], tail, st[4], st[5], st[6], st[7], pt, prov,
        q);

    if (rtv) rtv->Release();
    if (dsv) dsv->Release();
    if (cb0) cb0->Release();
    for (ID3D11ShaderResourceView* v : sr) {
        if (v) v->Release();
    }
    if (ps) ps->Release();
}

void drawCensusStructCount(void* dst, uint32_t dstOff, void* srcView,
                           bool foreignCtx) {
    if (g_framesLeft == 0) return;
    ++g_copies;
    ++g_copiesThisFrame;
    const uint32_t q = g_seq++;
    if (g_lines >= g_maxLines) return;
    ++g_lines;
    // The one call that moves a GPU-side element count into an argument
    // buffer -- the write that arms every DrawIndirect/DispatchIndirect
    // consumer, and the round-seventeen answer to "nothing writes the
    // args". dst is a resource, src is a UAV: the tokens say which list
    // fed which argument buffer.
    char db[24], sb[24];
    Log::get().note("DCS %u #%u dst=%s off=%u src=%s%s q=%u",
                    g_frameOrdinal, g_copiesThisFrame - 1,
                    bindingToken(dst, Kind::kResource, db, sizeof(db)), dstOff,
                    bindingToken(srcView, Kind::kView, sb, sizeof(sb)),
                    foreignCtx ? " t=f" : "", q);
}

void drawCensusCopy(char kind, void* dst, uint32_t dstSub, uint32_t dstX,
                    uint32_t dstY, void* src, uint32_t srcSub, bool hasBox,
                    uint32_t left, uint32_t top, uint32_t right, uint32_t bottom,
                    bool foreignCtx) {
    if (g_framesLeft == 0) return;
    ++g_copies;
    ++g_copiesThisFrame;
    const uint32_t q = g_seq++;
    if (g_lines >= g_maxLines) return;
    ++g_lines;

    // Resources, not views: both arguments arrive as ID3D11Resource*, which is
    // the case bindingToken's kResource form exists for. Handing them to the
    // view form would run GetResource on something that is not a view -- the
    // mistake binding_shadow.h documents at length.
    char db[24], sb[24];
    const char* d = bindingToken(dst, Kind::kResource, db, sizeof(db));
    const char* s = bindingToken(src, Kind::kResource, sb, sizeof(sb));

    char box[64] = "";
    if (hasBox) {
        _snprintf_s(box, sizeof(box), _TRUNCATE, " box=%u,%u-%u,%u", left, top,
                    right, bottom);
    }
    Log::get().note("DCC %u #%u %c dst=%s sub=%u at=%u,%u src=%s sub=%u%s%s q=%u",
                    g_frameOrdinal, g_copiesThisFrame - 1, kind, d, dstSub, dstX,
                    dstY, s, srcSub, box, foreignCtx ? " t=f" : "", q);
}

void drawCensusResolve(void* dst, uint32_t dstSub, void* src, uint32_t srcSub,
                       uint32_t fmt) {
    if (g_framesLeft == 0) return;
    ++g_copies;
    ++g_copiesThisFrame;
    const uint32_t q = g_seq++;
    if (g_lines >= g_maxLines) return;
    ++g_lines;

    char db[24], sb[24];
    const char* d = bindingToken(dst, Kind::kResource, db, sizeof(db));
    const char* s = bindingToken(src, Kind::kResource, sb, sizeof(sb));
    Log::get().note("DCC %u #%u V dst=%s sub=%u at=0,0 src=%s sub=%u fmt=%u q=%u",
                    g_frameOrdinal, g_copiesThisFrame - 1, d, dstSub, s, srcSub,
                    fmt, q);
}

void drawCensusCbNoteMap(void* resource, void* data) {
    if (!g_cbWatchHash) return;
    for (CbWatch& w : g_cbWatch) {
        if (w.buf == resource) w.mapped = data;
    }
}

void drawCensusCbNoteUnmap(void* resource) {
    if (!g_cbWatchHash) return;
    for (CbWatch& w : g_cbWatch) {
        if (w.buf == resource && w.mapped && w.bytes) {
            // Before the real Unmap, while the mapping is still live. The
            // same bargain every tee in vscreen strikes, under this module's
            // own budget: a fault reading one write must not stop the next.
            void* src = w.mapped;
            const uint32_t n = w.bytes;
            guardedBudget(g_cbBudget, [&] {
                memcpy(w.shadow, src, n);
                w.valid = true;
            });
            w.mapped = nullptr;
        }
    }
}

void drawCensusCbNoteUpdate(void* resource, const void* data, uint32_t bytes) {
    if (!g_cbWatchHash || !data) return;
    for (CbWatch& w : g_cbWatch) {
        if (w.buf == resource && w.bytes) {
            const uint32_t n = bytes && bytes < w.bytes ? bytes : w.bytes;
            guardedBudget(g_cbBudget, [&] {
                memcpy(w.shadow, data, n);
                w.valid = true;
            });
        }
    }
}

void drawCensusDispatch(ID3D11DeviceContext* ctx, uint32_t x, uint32_t y,
                        uint32_t z, bool foreignCtx, void* indirectArgs,
                        uint32_t indirectOff) {
    if (g_framesLeft == 0) return;
    ++g_dispatches;
    ++g_dispThisFrame;
    const uint32_t q = g_seq++;
    if (g_lines >= g_maxLines) return;
    ++g_lines;

    // What this dispatch can WRITE: UAV slots 0-7, read straight off the
    // context. This used the CsUav0-3 shadow until round sixteen of the
    // black squares measured a reconstruction chain whose middle was
    // invisible -- E861's outputs had no recorded reader and B742's inputs
    // no recorded writer -- and three blind spots explained it together:
    // dispatches on DEFERRED contexts (which reach these thunks and were
    // passed through unrecorded), DispatchIndirect (never hooked), and UAV
    // slots 4-7 (never shadowed). Direct reads close the third and work on
    // any context, which closes the first; the shadow only ever knew the
    // immediate one.
    char ub[8][24];
    const char* ut[8];
    for (int i = 0; i < 8; ++i) ut[i] = "-";

    // What it READS: CS SRVs 0-7 and CS b0-b1, straight off the context the
    // way a draw line's x= tail is taken.
    //
    // WHY (2026-08-25, the black squares, round fourteen): twelve rounds
    // proved the two eye composites byte-equivalent, and the per-eye temporal
    // reconstruction (E861F611375E7ECC + B74273EC13F7CD59) became the last
    // writer standing -- with its masks and its accumulator BOTH exonerated
    // by pair-sync nulls. The divergence therefore rides its per-frame
    // INPUTS, and those are CS SRVs: the one binding class no census line
    // ever carried. Absent tokens mean the probe did not answer, never
    // "nothing bound", the x= tail's spelling exactly.
    char st[232] = "";
    char cbt[64] = "";
    uint64_t ch = 0;
    if (ctx) {
        ID3D11UnorderedAccessView* uv[8] = {};
        ID3D11ShaderResourceView* sr[8] = {};
        ID3D11Buffer* cb[2] = {};
        ID3D11ComputeShader* cs = nullptr;
        bool got = false;
        guardedBudget(g_iaBudget, [&] {
            ctx->CSGetUnorderedAccessViews(0, 8,
                reinterpret_cast<ID3D11UnorderedAccessView**>(uv));
            ctx->CSGetShaderResources(0, 8,
                reinterpret_cast<ID3D11ShaderResourceView**>(sr));
            ctx->CSGetConstantBuffers(0, 2,
                reinterpret_cast<ID3D11Buffer**>(cb));
            ctx->CSGetShader(&cs, nullptr, nullptr);
            got = true;
        });
        if (got) {
            ch = lookupShaderHash(cs);
            for (int i = 0; i < 8; ++i) {
                ut[i] = bindingToken(uv[i], Kind::kView, ub[i], sizeof(ub[i]));
            }
            char sb[8][24];
            const char* tk[8];
            for (int i = 0; i < 8; ++i) {
                tk[i] = bindingToken(sr[i], Kind::kView, sb[i], sizeof(sb[i]));
            }
            _snprintf_s(st, sizeof(st), _TRUNCATE,
                        " s=%s,%s,%s,%s,%s,%s,%s,%s", tk[0], tk[1], tk[2],
                        tk[3], tk[4], tk[5], tk[6], tk[7]);
            char c0b[24], c1b[24];
            _snprintf_s(cbt, sizeof(cbt), _TRUNCATE, " cb=%s,%s",
                        bindingToken(cb[0], Kind::kResource, c0b, sizeof(c0b)),
                        bindingToken(cb[1], Kind::kResource, c1b, sizeof(c1b)));
        }
        for (ID3D11UnorderedAccessView* v : uv) {
            if (v) v->Release();
        }
        for (ID3D11ShaderResourceView* v : sr) {
            if (v) v->Release();
        }
        for (ID3D11Buffer* b : cb) {
            if (b) b->Release();
        }
        if (cs) cs->Release();
    }

    // The provenance tail: t=f for a deferred/foreign context (the q= is the
    // moment the call was RECORDED into its command list, not played back),
    // t=i for DispatchIndirect (group counts live GPU-side; n= is 0,0,0 and
    // args= names the argument buffer and byte offset), t=fi for both.
    char tail[64] = "";
    if (indirectArgs) {
        char ab[24];
        _snprintf_s(tail, sizeof(tail), _TRUNCATE, " args=%s+%u t=%si",
                    bindingToken(indirectArgs, Kind::kResource, ab, sizeof(ab)),
                    indirectOff, foreignCtx ? "f" : "");
    } else if (foreignCtx) {
        _snprintf_s(tail, sizeof(tail), _TRUNCATE, " t=f");
    }
    Log::get().note(
        "DCX %u #%u n=%u,%u,%u ch=%016llX u=%s,%s,%s,%s,%s,%s,%s,%s%s%s%s q=%u",
        g_frameOrdinal, g_dispThisFrame - 1, x, y, z,
        static_cast<unsigned long long>(ch), ut[0], ut[1], ut[2], ut[3],
        ut[4], ut[5], ut[6], ut[7], st, cbt, tail, q);

    // The compute CB watch, after the dispatch's own line the way the draw
    // form follows its draw.
    if (ch && ((g_cbWatchHash && ch == g_cbWatchHash) ||
               (g_cbWatchHash2 && ch == g_cbWatchHash2))) {
        cbWatchOnDispatch(ctx, q);
    }
}

// advanced.census_at_ms: arm a census at named moments after the session's
// first frame. Read once, fired at most once per entry, and never while
// another census is running -- an entry that comes due mid-census simply
// stays due, and fires at the first frame edge after that census ends. The
// line says the moment it actually landed at, not the one it asked for.
static void runCensusSchedule(uint32_t frameNo) {
    if (!g_scheduleRead) {
        g_scheduleRead = true;
        g_firstFrameMs = stampMs();
        const std::string spec =
            Config::get().getString("advanced.census_at_ms", "");
        const char* p = spec.c_str();
        while (*p && g_scheduleCount < kMaxSchedule) {
            char* end = nullptr;
            const unsigned long v = strtoul(p, &end, 10);
            if (end == p) break;
            g_scheduleMs[g_scheduleCount++] = static_cast<uint64_t>(v);
            p = end;
            while (*p == ',' || *p == ' ') ++p;
        }
        if (g_scheduleCount) {
            Log::get().note(
                "DC: advanced.census_at_ms holds %u moment(s) -- a census is "
                "armed at each of them, measured from this first frame edge, "
                "recording offscreen draws regardless of census_offscreen. "
                "For the startup sequence, which no keypress can reach.",
                g_scheduleCount);
        }
    }
    if (!g_scheduleCount || drawCensusArmed()) return;
    const uint64_t elapsed = nowMs() - g_firstFrameMs;
    for (uint32_t i = 0; i < g_scheduleCount; ++i) {
        if (g_scheduleFired & (1u << i)) continue;
        if (elapsed < g_scheduleMs[i]) continue;
        g_scheduleFired |= (1u << i);
        g_pending = true;
        g_forceOffscreen = true;
        Log::get().note(
            "DC: census armed by advanced.census_at_ms entry %u of %u -- it "
            "asked for %llu ms and landed at %llu ms, frame %u.",
            i + 1, g_scheduleCount,
            static_cast<unsigned long long>(g_scheduleMs[i]),
            static_cast<unsigned long long>(elapsed), frameNo);
        return;
    }
}

void drawCensusTick(ID3D11DeviceContext* ctx) {
    if (!g_cbReadCount || !ctx) return;
    uint32_t left = 0;
    for (uint32_t i = 0; i < g_cbReadCount; ++i) {
        CbRead& r = g_cbRead[i];
        if (!r.stage) continue;
        if (g_lastFrameNo < r.dueFrame) {
            ++left;
            continue;
        }
        guardedBudget(g_cbBudget, [&] {
            D3D11_MAPPED_SUBRESOURCE m{};
            if (FAILED(ctx->Map(r.stage, 0, D3D11_MAP_READ, 0, &m)) || !m.pData) {
                Log::get().note("DCW read q=%u %c -- the copy could not be "
                                "mapped.", r.q, r.letter);
                return;
            }
            const float* f = static_cast<const float*>(m.pData);
            const uint32_t nf = r.bytes / 4;
            char fl[1000];
            size_t at = 0;
            for (uint32_t k = 0; k < nf && at + 16 < sizeof(fl); ++k) {
                const int n = _snprintf_s(fl + at, sizeof(fl) - at, _TRUNCATE,
                                          " %.6g", static_cast<double>(f[k]));
                if (n < 0) break;
                at += static_cast<size_t>(n);
            }
            fl[at] = '\0';
            char tok[24];
            Log::get().note(
                "DCW read %u q=%u %c cb=%s b=%u h=%016llX f=%s -- copied off "
                "the GPU, not from a write; these are the buffer's contents, "
                "not necessarily what that one draw read.",
                r.ordinal, r.q, r.letter,
                bindingToken(r.buf, Kind::kResource, tok, sizeof(tok)), r.bytes,
                static_cast<unsigned long long>(fnv1a64(m.pData, r.bytes)), fl);
            ctx->Unmap(r.stage, 0);
        });
        r.stage->Release();
        r.stage = nullptr;
    }
    if (!left) {
        for (CbRead& r : g_cbRead) {
            if (r.stage) r.stage->Release();
            r = CbRead();
        }
        g_cbReadCount = 0;
    }
}

void drawCensusFrameBoundary(uint32_t frameNo) {
    g_lastFrameNo = frameNo;
    runCensusSchedule(frameNo);
    if (g_framesLeft > 0) {
        Log::get().note("DC frame %u draws=%u off=%u copies=%u disp=%u",
                        g_frameOrdinal, g_drawsThisFrame, g_offThisFrame,
                        g_copiesThisFrame, g_dispThisFrame);
        g_drawsThisFrame = 0;
        g_offThisFrame = 0;
        g_copiesThisFrame = 0;
        g_dispThisFrame = 0;
        g_seq = 0;
        ++g_frameOrdinal;
        if (--g_framesLeft == 0) finish();
    }
    if (g_pending) {
        g_pending = false;
        ++g_censusNo;
        // Latched here, not read per draw: a census must record one
        // configuration throughout, and the draw path is the last place that
        // should touch a settings map.
        g_framesWanted = static_cast<uint32_t>(Config::get().getIntInRange(
            "advanced.census_frames", static_cast<int>(kCensusFrames), 1,
            static_cast<int>(kCensusFramesMax)));
        g_maxLines = static_cast<uint32_t>(Config::get().getIntInRange(
            "advanced.census_lines", static_cast<int>(kMaxLines), 256,
            static_cast<int>(kMaxLinesCeiling)));
        g_framesLeft = g_framesWanted;
        g_frameOrdinal = 0;
        g_draws = 0;
        g_drawsThisFrame = 0;
        g_lines = 0;
        g_overflow = 0;
        g_tabCount = 0;
        g_offThisFrame = 0;
        g_offDraws = 0;
        g_copiesThisFrame = 0;
        g_copies = 0;
        g_dispThisFrame = 0;
        g_dispatches = 0;
        g_seq = 0;
        // The auto arm's rider wins over the ini: a census fired at an
        // offscreen build that recorded no offscreen draws is the exact
        // capture this module already wasted a day on once.
        g_offscreen = Config::get().getBool("advanced.census_offscreen", false) ||
                      g_forceOffscreen;
        g_forceOffscreen = false;
        // The CB watch's shader, and a fresh dump budget. The watch SLOTS
        // deliberately survive from the last census: the buffer pointers
        // re-register on the first matched draw anyway, and a shadow already
        // holding the buffer's last write is a better first dump than
        // "unwritten".
        {
            const std::string cw =
                Config::get().getString("advanced.census_cb_watch", "");
            char* end = nullptr;
            g_cbWatchHash =
                cw.empty() ? 0 : _strtoui64(cw.c_str(), &end, 16);
            g_cbWatchHash2 =
                (end && *end == ',') ? _strtoui64(end + 1, nullptr, 16) : 0;
            g_cbWatchSlot = static_cast<uint32_t>(Config::get().getIntInRange(
                "advanced.census_cb_slot", 0, 0, 13));
        }
        g_cbDumps = 0;
        for (Interned& e : g_tab) e = Interned();
        Log::get().note("DC begin census=%u frames=%u frame=%u offscreen=%s",
                        g_censusNo, g_framesWanted, frameNo,
                        g_offscreen ? "yes" : "no");
        if (g_cbWatchHash) {
            Log::get().note("DCW watching vs=%016llX -- every recorded draw "
                            "running that shader dumps its b%u constants "
                            "(vertex and pixel stage)",
                            static_cast<unsigned long long>(g_cbWatchHash),
                            g_cbWatchSlot);
        }
    }
}

}  // namespace edvr
