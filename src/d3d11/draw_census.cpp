#include "draw_census.h"

#include <cstdio>   // _snprintf_s: the sampled-slot list is built before logging

#include <windows.h>

#include <d3d11.h>

#include "../common/guard.h"
#include "../common/log.h"
#include "binding_shadow.h"
#include "exposure_fix.h"   // lookupShaderHash: the shader hash registry

namespace edvr {
namespace {

// Three frames: enough that "present in every frame of one census and no
// frame of the other" separates a steady overlay from frame-to-frame churn,
// while two censuses stay near one percent of the default 4 MB log cap.
constexpr uint32_t kCensusFrames = 3;

// A frame on foot has been measured at 1174 eye draws, so three frames can
// legitimately want ~3500 lines. The cap exists so a mode this was never
// pointed at cannot eat the log; the end line says when it bit.
constexpr uint32_t kMaxLines = 4096;

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
uint32_t g_framesLeft = 0;       // frames still to record; >0 means capturing
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

void dumpInternTable() {
    for (uint32_t i = 0; i < g_tabCount; ++i) {
        const Interned& e = g_tab[i];
        if (!e.resolved) {
            Log::get().note("DC id @%u ?", i);
        } else if (e.info.isTexture2D) {
            Log::get().note("DC id @%u tex %ux%u fmt=%u", i, e.info.a, e.info.b,
                            e.info.fmt);
        } else if (e.info.isBuffer) {
            Log::get().note("DC id @%u buf %u", i, e.info.a);
        } else {
            Log::get().note("DC id @%u ?", i);
        }
    }
}

void finish() {
    dumpInternTable();
    Log::get().note("DC end census=%u draws=%u lines=%u interned=%u overflow=%u "
                    "truncated=%u",
                    g_censusNo, g_draws, g_lines, g_tabCount, g_overflow,
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
    Log::get().note("DC: census armed -- the next %u whole frames of eye-texture "
                    "draws will be logged. Diff two of these with "
                    "tools/diff_draw_census.py.",
                    kCensusFrames);
}

void drawCensusEyeDraw(ID3D11DeviceContext* ctx, char kind, uint32_t count,
                       uint32_t instances, uint32_t eyeDrawIndex) {
    if (g_framesLeft == 0) return;   // pending counts draws only once started
    ++g_draws;
    ++g_drawsThisFrame;
    if (g_lines >= kMaxLines) return;
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
    if (st.ok) {
        // The shader's content hash beside its pointer token. The pointer
        // is per-session noise the differ rightly ignores; the HASH is
        // stable across sessions and is the one key that cannot collide
        // between two draws running different code -- the geyser hunt
        // (2026-08-23) found particle plumes and rock meshes sharing the
        // entire size-level signature, separable by nothing the census
        // recorded. vh=0 means the shader was created before the hooks
        // went in.
        char vsb[24], vbb[24];
        _snprintf_s(tail, sizeof(tail), _TRUNCATE,
                    " vs=%s vh=%016llX vb=%s sd=%u of=%u tp=%u",
                    bindingToken(st.vs, Kind::kOpaque, vsb, sizeof(vsb)),
                    static_cast<unsigned long long>(lookupShaderHash(st.vs)),
                    bindingToken(st.vb, Kind::kResource, vbb, sizeof(vbb)),
                    st.stride, st.offset, st.topology);
    }

    Log::get().note("DC %u #%u %c n=%u i=%u r=%s d=%s c=%s s=%s,%s,%s,%s%s",
                    g_frameOrdinal, eyeDrawIndex, kind, count, instances, r, d, c,
                    s0, s1, s2, s3, tail);
}

void drawCensusFrameBoundary(uint32_t frameNo) {
    if (g_framesLeft > 0) {
        Log::get().note("DC frame %u draws=%u", g_frameOrdinal, g_drawsThisFrame);
        g_drawsThisFrame = 0;
        ++g_frameOrdinal;
        if (--g_framesLeft == 0) finish();
    }
    if (g_pending) {
        g_pending = false;
        ++g_censusNo;
        g_framesLeft = kCensusFrames;
        g_frameOrdinal = 0;
        g_draws = 0;
        g_drawsThisFrame = 0;
        g_lines = 0;
        g_overflow = 0;
        g_tabCount = 0;
        for (Interned& e : g_tab) e = Interned();
        Log::get().note("DC begin census=%u frames=%u frame=%u", g_censusNo,
                        kCensusFrames, frameNo);
    }
}

}  // namespace edvr
