#include "draw_census.h"

#include <cstdio>   // _snprintf_s: the sampled-slot list is built before logging
#include <cstdlib>  // _strtoui64: the CB watch's shader hash from config
#include <cstring>  // memcpy: the CB watch's shadow refresh

#include <windows.h>

#include <d3d11.h>

#include "../common/config.h"
#include "../common/guard.h"
#include "../common/log.h"
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
uint32_t g_seq = 0;              // ONE ordinal across every recorded event in
                                 // a frame -- the q= token. The per-kind
                                 // indexes above cannot say whether a copy
                                 // landed between two draws; this can.
// The constant-buffer watch (see the header). Two slots: the watched draw's
// VS b0 and PS b0. The shadow is refreshed by the Map/Unmap tee and dumped
// at each watched draw, so a dump is the bytes the GPU reads for THAT draw.
constexpr uint32_t kCbShadowBytes = 256;
constexpr uint32_t kCbDumpCap = 256;   // dumps per census; 2 stages x 2 eyes
                                       // x 30 frames = 120 in the FSS run
struct CbWatch {
    void*    buf = nullptr;     // the buffer object, identity only
    void*    mapped = nullptr;  // its pData between Map and Unmap
    uint32_t bytes = 0;         // ByteWidth, as resolved at registration
    bool     valid = false;     // the shadow holds at least one full write
    uint8_t  shadow[kCbShadowBytes];
};
uint64_t g_cbWatchHash = 0;      // latched at census begin; 0 = off
CbWatch  g_cbWatch[2];           // [0] = VS b0, [1] = PS b0
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
    char tok[24];
    Log::get().note("DCW register %c cb=%s bytes=%u", slot == 0 ? 'v' : 'p',
                    bindingToken(buf, Kind::kResource, tok, sizeof(tok)),
                    w.bytes);
}

// One shadow dump: the bytes the GPU will read for the draw that asked. All
// floats on one line -- 52 of them for the 208-byte camera block -- because
// the offline diff pairs lines by (frame, stage) and splitting a dump across
// lines is how half a buffer gets compared against the wrong eye.
void cbWatchDump(uint32_t slot, uint32_t q) {
    CbWatch& w = g_cbWatch[slot];
    if (!w.buf || g_cbDumps >= kCbDumpCap) return;
    ++g_cbDumps;
    char tok[24];
    if (!w.valid) {
        Log::get().note("DCW %u q=%u %c cb=%s unwritten", g_frameOrdinal, q,
                        slot == 0 ? 'v' : 'p',
                        bindingToken(w.buf, Kind::kResource, tok, sizeof(tok)));
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
                    q, slot == 0 ? 'v' : 'p',
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
    cbWatchRegister(0, bindingGet(BindSlot::VsCb0));
    guardedBudget(g_cbBudget, [&] {
        ID3D11Buffer* pb = nullptr;
        ctx->PSGetConstantBuffers(0, 1, &pb);
        cbWatchRegister(1, pb);
        if (pb) pb->Release();
    });
    cbWatchDump(0, q);
    cbWatchDump(1, q);
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
    if (st.ok) {
        ID3D11ShaderResourceView* xs[4] = {};
        bool got = false;
        guardedBudget(g_iaBudget, [&] {
            ctx->PSGetShaderResources(4, 4, reinterpret_cast<ID3D11ShaderResourceView**>(xs));
            got = true;
        });
        if (got) {
            char x0b[24], x1b[24], x2b[24], x3b[24];
            _snprintf_s(xt, sizeof(xt), _TRUNCATE, " x=%s,%s,%s,%s",
                        bindingToken(xs[0], Kind::kView, x0b, sizeof(x0b)),
                        bindingToken(xs[1], Kind::kView, x1b, sizeof(x1b)),
                        bindingToken(xs[2], Kind::kView, x2b, sizeof(x2b)),
                        bindingToken(xs[3], Kind::kView, x3b, sizeof(x3b)));
        }
        for (ID3D11ShaderResourceView* v : xs) {
            if (v) v->Release();
        }
    }

    Log::get().note("%s %u #%u %c n=%u i=%u r=%s d=%s c=%s s=%s,%s,%s,%s%s%s q=%u",
                    tag, g_frameOrdinal, index, kind, count, instances, r, d, c,
                    s0, s1, s2, s3, tail, xt, q);

    // The CB watch, after the draw's own line so a DCW dump always follows
    // the draw it belongs to. Any recorded draw can match -- DC for the FSS
    // composite, DCO if a future hunt watches an offscreen builder.
    if (g_cbWatchHash && vh == g_cbWatchHash) cbWatchOnDraw(ctx, q);
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

void drawCensusCopy(char kind, void* dst, uint32_t dstSub, uint32_t dstX,
                    uint32_t dstY, void* src, uint32_t srcSub, bool hasBox,
                    uint32_t left, uint32_t top, uint32_t right, uint32_t bottom) {
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
    Log::get().note("DCC %u #%u %c dst=%s sub=%u at=%u,%u src=%s sub=%u%s q=%u",
                    g_frameOrdinal, g_copiesThisFrame - 1, kind, d, dstSub, dstX,
                    dstY, s, srcSub, box, q);
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

void drawCensusDispatch(uint32_t x, uint32_t y, uint32_t z) {
    if (g_framesLeft == 0) return;
    ++g_dispatches;
    ++g_dispThisFrame;
    const uint32_t q = g_seq++;
    if (g_lines >= g_maxLines) return;
    ++g_lines;

    // What this dispatch can WRITE: UAV slots 0-3, from the shadow the
    // exposure fix's CSSetUnorderedAccessViews hook has always maintained.
    // UAVs are views, so the kView form applies. The compute shader itself is
    // named by content hash, same as vh= on a draw line, so a writer found
    // here can be pinned or skipped by the tools that already exist.
    char u0b[24], u1b[24], u2b[24], u3b[24];
    const char* u0 = bindingToken(bindingGet(BindSlot::CsUav0), Kind::kView, u0b, sizeof(u0b));
    const char* u1 = bindingToken(bindingGet(BindSlot::CsUav1), Kind::kView, u1b, sizeof(u1b));
    const char* u2 = bindingToken(bindingGet(BindSlot::CsUav2), Kind::kView, u2b, sizeof(u2b));
    const char* u3 = bindingToken(bindingGet(BindSlot::CsUav3), Kind::kView, u3b, sizeof(u3b));
    Log::get().note("DCX %u #%u n=%u,%u,%u ch=%016llX u=%s,%s,%s,%s q=%u",
                    g_frameOrdinal, g_dispThisFrame - 1, x, y, z,
                    static_cast<unsigned long long>(
                        lookupShaderHash(bindingGet(BindSlot::Cs))),
                    u0, u1, u2, u3, q);
}

void drawCensusFrameBoundary(uint32_t frameNo) {
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
            g_cbWatchHash = cw.empty() ? 0 : _strtoui64(cw.c_str(), nullptr, 16);
        }
        g_cbDumps = 0;
        for (Interned& e : g_tab) e = Interned();
        Log::get().note("DC begin census=%u frames=%u frame=%u offscreen=%s",
                        g_censusNo, g_framesWanted, frameNo,
                        g_offscreen ? "yes" : "no");
        if (g_cbWatchHash) {
            Log::get().note("DCW watching vs=%016llX -- every recorded draw "
                            "running that shader dumps its b0 constants",
                            static_cast<unsigned long long>(g_cbWatchHash));
        }
    }
}

}  // namespace edvr
