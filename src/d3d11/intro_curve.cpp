#include "intro_curve.h"

#include <windows.h>

#include <d3d11.h>

#include <cstdio>
#include <cstring>
#include <new>

#include "../common/guard.h"
#include "../common/log.h"
#include "binding_shadow.h"
#include "intro_curve_math.h"
#include "panel_curve.h"

namespace edvr {
namespace {

// cb2 is five float4s. Both the shader's declaration (CB2[5]) and the buffers the census found (80 bytes each) say so.
constexpr uint32_t kCbFloats = 20;
constexpr uint32_t kCbBytes = kCbFloats * 4;

// The constant buffer slot the placement lives in, from the disassembly (the same number intro_panel.cpp uses).
constexpr uint32_t kVsSlot = 2;

// Frames to let the GPU copy execute before it is mapped: intro_panel.cpp's number, and quad_probe's before it, for the same reason --
// long enough that the map never stalls the render thread, short enough to be learned within a blink.
constexpr uint32_t kSettleFrames = 4;

// The table: 16 pairs is a few dozen times what one intro uses (a constants buffer per eye, a surface or two). A 17th pushes the pair
// drawn least recently out.
constexpr uint32_t kMaxEntries = 16;

// A pair not drawn for this many frames is forgotten: three seconds at 60 Hz, two at 90. A pair is drawn every frame of its phase, so this
// only ever drops pairs that are gone -- and a pair the game freed and re-created at the same addresses is learned afresh. (It was 600, ten
// seconds at 60 Hz: long enough for a pair the movie had stopped drawing to be in the table still, verdict and all, when the splash came to
// reuse its addresses.)
constexpr uint32_t kExpireFrames = 180;

// A FLAT PAIR IS READ AGAIN. The game reuses its per-eye 80-byte constant buffers and its 1920x1080 surfaces, and nothing says the cut from
// the movie to the splash gives them new identities: a pair first read as screen-space would then stay flat for ever, and the splash would
// inherit it. So a flat pair whose last copy is this old is copied again at its next draw, once a second at 60 Hz, 80 bytes each -- the same
// copy as the first, read back and judged exactly as a first read is. It stays flat while the copy is in flight, so nothing changes until a
// read says otherwise. A WORLD pair is never read again (intro_curve.h says why that is a known limit).
constexpr uint32_t kFlatRecheckFrames = 60;

// Lines about pairs left as the game drew them: a flat pair's first reading, and a re-read that gives a different reason. A game that turned
// its buffers over every frame would otherwise write one per learn. THE CAP IS FOR FLAT LINES ALONE: a world-space line is always written,
// because it is the line a flight must find (and a pair read flat first and world later writes its world line whenever the re-read lands,
// however many flat lines were written before it).
constexpr uint32_t kMaxFlatLines = 12;

enum class State : uint8_t {
    kFree,      // no pair
    kCopying,   // the first copy is in flight, due kSettleFrames after it
    kWorld,     // read as a world-space panel: armed, with halfWidth and toward
    kFlat,      // read as anything else: left as the game drew it (and read again every kFlatRecheckFrames it is drawn)
};

struct Entry {
    State         state = State::kFree;
    void*         buffer = nullptr;    // the game's cb2 buffer: an identity, no reference held (the bargain binding_shadow.h documents)
    void*         surface = nullptr;   // the view the composite samples at PS slot 0: likewise
    ID3D11Buffer* stage = nullptr;     // the copy in flight: a first read's (kCopying) or a flat pair's re-read, at most one a pair
    uint32_t      dueFrame = 0;        // the frame the copy may be mapped on
    uint32_t      lastSeen = 0;        // the frame this pair was last drawn on
    uint32_t      born = 0;            // arrival order, to tell two pairs seen the same frame apart
    uint32_t      readFrame = 0;       // the frame the pair's constants were last copied: the snapshot's own time, and where the wait to the next starts
    const char*   flatWhy = nullptr;   // a flat pair: the CLASS of the reason last said for it (a static string; a re-read for the same one says nothing)
    float         halfWidth = 0.0f;    // kWorld only: cb2[0].x, the strip's gain
    int           toward = 0;          // kWorld only: +1 or -1, the strip's direction
    bool          reverseU = false;    // kWorld only: the placement's +x runs to the viewer's LEFT, so the strip's u must run against x
};

// The module's own reasons a pair is left as the game drew it, beside introReadWorldCb's. Each static string is the reason's class.
const char kWhySmall[] = "its constant buffer is smaller than the bytes the shader declares";
const char kWhyNoDevice[] = "no device could be had from the context to copy its constants with";
const char kWhyNoStage[] = "the buffer to copy its constants into could not be created";
const char kWhyNoRead[] = "the copy of its constants could not be read back";

// The first fault stands this down, as the strip's own does: a fault here may have left the context in an odd state, and what this buys is
// never worth a second one.
constexpr int kFaultBudget = 1;
FaultBudget g_budget("introCurve", kFaultBudget);

Entry    g_entry[kMaxEntries];
uint32_t g_frame = 0;
uint32_t g_born = 0;

bool g_retired = false;          // the intro is over; stood down for good
bool g_wantedSeen = false;       // the strip was asked for on some frame before the intro ended
bool g_evictNoted = false;       // the table-full line has been said
bool g_capNoted = false;         // the flat-line cap has been said
uint32_t g_flatLines = 0;

// The armed draw's numbers, copied out of the entry so that nothing the table does can move them under the caller.
float g_armedGain = 0.0f;
int   g_armedToward = 0;
bool  g_armedReverseU = false;

uint64_t g_learnedWorld = 0;
uint64_t g_learnedFlat = 0;
uint64_t g_rereads = 0;
uint64_t g_flatToWorld = 0;
uint64_t g_armedDraws = 0;
uint64_t g_evicted = 0;
uint64_t g_expired = 0;

void dropStage(Entry& e) {
    if (e.stage) {
        e.stage->Release();
        e.stage = nullptr;
    }
}

void clearEntry(Entry& e) {
    dropStage(e);
    e = Entry();
}

void releaseAll() {
    for (Entry& e : g_entry) clearEntry(e);
}

// Said when a fault has just spent the budget: from then on every composite is the game's own.
void noteStandDown() {
    if (g_budget.shouldRun()) return;
    Log::get().note("splash curve: a fault stood this down for the session -- every composite is drawn as the game drew it.");
}

// What a stood-down module still holds, let go on its own guard: the budget is already spent, so nothing is charged. Nothing is held after
// the first call, and the rest are sixteen empty entries.
void releaseAfterFault() {
    guarded("introCurve.release", [] { releaseAll(); });
}

// Whether a line about a pair left as the game drew it may be written: the first kMaxFlatLines, then one line saying the rest are not.
// World-space lines never come here.
bool flatLineAllowed() {
    if (g_flatLines < kMaxFlatLines) {
        ++g_flatLines;
        return true;
    }
    if (!g_capNoted) {
        g_capNoted = true;
        Log::get().note("splash curve: %u lines about composites left as the game drew them are written; the rest are not (a world-space one is always "
                        "written, and the counts in the retirement line stay exact).",
                        kMaxFlatLines);
    }
    return false;
}

// The 20 floats on one line, cb2[0] first and four to a group: what the next flight reads to see what the game placed.
void formatCb(const float* f, char* out, size_t size) {
    size_t at = 0;
    out[0] = '\0';
    for (uint32_t i = 0; i < kCbFloats && at + 1 < size; ++i) {
        const char* sep = i == 0 ? "" : (i % 4 == 0 ? " | " : " ");
        const int n = _snprintf_s(out + at, size - at, _TRUNCATE, "%s%.6g", sep, static_cast<double>(f[i]));
        if (n < 0) break;
        at += static_cast<size_t>(n);
    }
}

// The two ways a line about a pair left as the game drew it begins: a first reading's, and a re-read's that gave another reason.
const char kLeadFirst[] = "this composite";
const char kLeadAgain[] = "this composite, read again,";

// A line about a pair left as the game drew it: the reason and, when the copy was read, the 20 floats. THE FLAT LINES ARE CAPPED, a first
// reading's and a re-read's alike (flatLineAllowed); a world-space line never is.
void flatLine(const char* lead, const char* why, const float* f) {
    if (!flatLineAllowed()) return;
    if (f) {
        char text[512];
        formatCb(f, text, sizeof(text));
        Log::get().note("splash curve: %s stays as the game drew it, because %s. Its cb2: %s.", lead, why, text);
    } else {
        Log::get().note("splash curve: %s stays as the game drew it, because %s.", lead, why);
    }
}

// A pair's first reading is flat: it is learned flat, and said (with its reason and the 20 floats, when it has them).
void settleFlat(Entry& e, const char* cls, const char* why, const float* f) {
    e.state = State::kFlat;
    e.flatWhy = cls;
    ++g_learnedFlat;
    flatLine(kLeadFirst, why, f);
}

// A flat pair read again and still flat: nothing changes, and nothing is said unless the reason is of a different class from the last one said
// for this pair -- then one line, a flat line like any other (capped). The class is the reason's own text: two reads with the same one agree.
void noteStillFlat(Entry& e, const char* cls, const char* why, const float* f) {
    if (e.flatWhy && std::strcmp(e.flatWhy, cls) == 0) return;
    e.flatWhy = cls;
    flatLine(kLeadAgain, why, f);
}

// A world reading, a first one or a flat pair's re-read: the same line, and never capped. The reading is ok only when introReadWorldCb could say
// which way the placement's +x runs on the screen (w.xDir is then kLeft or kRight); the pair keeps what it said, and the strip's u runs against
// x when +x runs to the viewer's left -- the intro composite's own placement does, and the game's six-index quad compensates in its vertex data.
void settleWorld(Entry& e, const IntroWorldCb& w, const float* f) {
    e.state = State::kWorld;
    e.flatWhy = nullptr;
    e.halfWidth = w.halfWidth;
    e.toward = w.toward;
    e.reverseU = w.xDir == IntroXDir::kLeft;
    ++g_learnedWorld;
    const PanelCurveInfo strip = panelCurveInfo();
    Log::get().note(
        "splash curve: the game-placed composite (VS %08X) reads as a world-space panel: half-width %.3f m, depth toward the viewer %+d "
        "(cb2[3].w %.3f, cb2[4].w %.3f); +x runs to the viewer's %s, so u runs %s x; drawn as a %d-column strip at curvature %.3f.",
        static_cast<unsigned>(kIntroCompositeVsHash >> 32), static_cast<double>(w.halfWidth), w.toward, static_cast<double>(f[15]),
        static_cast<double>(f[19]), e.reverseU ? "left" : "right", e.reverseU ? "against" : "with", strip.segments,
        static_cast<double>(strip.curvature));
}

Entry* findEntry(void* buffer, void* surface) {
    for (Entry& e : g_entry) {
        if (e.state != State::kFree && e.buffer == buffer && e.surface == surface) return &e;
    }
    return nullptr;
}

// A free entry; with none free, the pair drawn least recently (the earlier arrival of two equals) is pushed out, its readback buffer with it.
Entry* claimEntry() {
    Entry* oldest = nullptr;
    for (Entry& e : g_entry) {
        if (e.state == State::kFree) {
            clearEntry(e);   // a free entry holds nothing, unless a fault stopped its setup half way
            return &e;
        }
        if (!oldest || e.lastSeen < oldest->lastSeen || (e.lastSeen == oldest->lastSeen && e.born < oldest->born)) oldest = &e;
    }
    ++g_evicted;
    if (!g_evictNoted) {
        g_evictNoted = true;
        Log::get().note("splash curve: more than %u composites in use at once; the one drawn least recently was forgotten, and is learned "
                        "again if it returns. Said once.",
                        kMaxEntries);
    }
    clearEntry(*oldest);
    return oldest;
}

// Why a copy could not be made: the reason's class (a static string) and the reason as the line says it.
struct CopyFail {
    const char* cls = nullptr;
    char text[96] = "";
};

// The one-shot copy of the game's constants for a pair, its first read and every re-read alike. True: the copy is in flight (the staging
// buffer held, due kSettleFrames on). False: nothing was copied, and `fail` says why. The attempt is the pair's read either way: the wait
// to its next starts here.
bool issueCopy(ID3D11DeviceContext* ctx, ID3D11Buffer* cb, Entry& e, CopyFail& fail) {
    e.readFrame = g_frame;
    D3D11_BUFFER_DESC bd{};
    cb->GetDesc(&bd);
    if (bd.ByteWidth < kCbBytes) {
        fail.cls = kWhySmall;
        _snprintf_s(fail.text, _TRUNCATE, "its constant buffer is %u bytes, under the %u the shader declares", static_cast<unsigned>(bd.ByteWidth),
                    static_cast<unsigned>(kCbBytes));
        return false;
    }
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    if (!dev) {
        fail.cls = kWhyNoDevice;
        _snprintf_s(fail.text, _TRUNCATE, "%s", kWhyNoDevice);
        return false;
    }
    D3D11_BUFFER_DESC sd{};
    sd.Usage = D3D11_USAGE_STAGING;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    sd.ByteWidth = kCbBytes;
    const HRESULT made = dev->CreateBuffer(&sd, nullptr, &e.stage);
    dev->Release();
    if (FAILED(made) || !e.stage) {
        e.stage = nullptr;
        fail.cls = kWhyNoStage;
        _snprintf_s(fail.text, _TRUNCATE, "%s", kWhyNoStage);
        return false;
    }
    D3D11_BOX box{};
    box.right = kCbBytes;
    box.bottom = 1;
    box.back = 1;
    ctx->CopySubresourceRegion(e.stage, 0, 0, 0, 0, static_cast<ID3D11Resource*>(cb), 0, &box);
    e.dueFrame = g_frame + kSettleFrames;
    return true;
}

// A new pair's first copy. Leaves the entry kCopying, or kFlat with the reason said when the copy cannot be made.
void startCopy(ID3D11DeviceContext* ctx, ID3D11Buffer* cb, Entry& e) {
    CopyFail fail;
    if (issueCopy(ctx, cb, e, fail)) {
        e.state = State::kCopying;
        return;
    }
    settleFlat(e, fail.cls, fail.text, nullptr);
}

// A flat pair's copy again, issued where its buffer is bound. The pair stays flat while it is in flight; a copy that cannot be made is a read
// that read flat for that reason.
void startReread(ID3D11DeviceContext* ctx, ID3D11Buffer* cb, Entry& e) {
    CopyFail fail;
    if (!issueCopy(ctx, cb, e, fail)) noteStillFlat(e, fail.cls, fail.text, nullptr);
}

// A settled copy, read and judged. The staging buffer is let go whatever the answer.
void readBack(ID3D11DeviceContext* ctx, Entry& e) {
    // A flat pair's copy again: it stayed flat while the copy was in flight, and it is judged now exactly as a first read is.
    const bool reread = e.state == State::kFlat;
    float f[kCbFloats] = {};
    bool have = false;
    if (e.stage) {
        D3D11_MAPPED_SUBRESOURCE m{};
        if (SUCCEEDED(ctx->Map(e.stage, 0, D3D11_MAP_READ, 0, &m))) {
            if (m.pData) {
                std::memcpy(f, m.pData, sizeof(f));
                have = true;
            }
            ctx->Unmap(e.stage, 0);
        }
    }
    dropStage(e);
    if (reread) ++g_rereads;
    if (!have) {
        if (reread) noteStillFlat(e, kWhyNoRead, kWhyNoRead, nullptr);
        else settleFlat(e, kWhyNoRead, kWhyNoRead, nullptr);
        return;
    }
    const IntroWorldCb w = introReadWorldCb(f);
    if (!w.ok) {
        if (reread) noteStillFlat(e, w.why, w.why, f);
        else settleFlat(e, w.why, w.why, f);
        return;
    }
    if (reread) ++g_flatToWorld;
    settleWorld(e, w, f);
}

// The draw path's body, under the budget: find or start this draw's pair, copy a flat pair again when it is due, and arm the draw when the
// pair is a world-space panel.
bool recognise(ID3D11DeviceContext* ctx) {
    ID3D11Buffer* cb = nullptr;
    ctx->VSGetConstantBuffers(kVsSlot, 1, &cb);
    if (!cb) return false;
    void* const surface = bindingGet(BindSlot::PsSrv0);
    Entry* e = findEntry(cb, surface);
    if (e) {
        e->lastSeen = g_frame;
        if (e->state == State::kFlat && !e->stage && g_frame - e->readFrame >= kFlatRecheckFrames) startReread(ctx, cb, *e);
    } else {
        e = claimEntry();
        e->buffer = cb;
        e->surface = surface;
        e->born = ++g_born;
        e->lastSeen = g_frame;
        startCopy(ctx, cb, *e);
    }
    cb->Release();
    if (e->state != State::kWorld) return false;
    g_armedGain = e->halfWidth;
    g_armedToward = e->toward;
    g_armedReverseU = e->reverseU;
    ++g_armedDraws;
    return true;
}

// The frame's table work, under the budget: forget the pairs that are gone, read the copies that have settled (a first read's or a flat
// pair's re-read: the pair holds a staging buffer while one is in flight).
void serviceEntries(ID3D11DeviceContext* ctx) {
    for (Entry& e : g_entry) {
        if (e.state == State::kFree) continue;
        if (g_frame - e.lastSeen > kExpireFrames) {
            ++g_expired;
            clearEntry(e);
            continue;
        }
        if (e.stage && g_frame >= e.dueFrame && ctx) readBack(ctx, e);
    }
}

void retire() {
    g_retired = true;
    g_armedGain = 0.0f;
    g_armedToward = 0;
    g_armedReverseU = false;
    const char* fault = g_budget.shouldRun() ? "" : "; a fault had already stood it down";
    if (g_learnedWorld + g_learnedFlat || g_armedDraws) {
        // The re-reads are said only when there were any: with none the line is what it always was.
        char reread[96] = "";
        if (g_rereads) {
            _snprintf_s(reread, _TRUNCATE, ", %llu re-read(s), %llu flat-to-world change(s)", static_cast<unsigned long long>(g_rereads),
                        static_cast<unsigned long long>(g_flatToWorld));
        }
        Log::get().note(
            "splash curve: a rendered scene arrived -- the intro is over and this stands down for the session. It learned %llu composite(s) "
            "(%llu world-space, %llu flat) and armed %llu strip draw(s)%s%s.",
            static_cast<unsigned long long>(g_learnedWorld + g_learnedFlat), static_cast<unsigned long long>(g_learnedWorld),
            static_cast<unsigned long long>(g_learnedFlat), static_cast<unsigned long long>(g_armedDraws), reread, fault);
    } else if (g_wantedSeen) {
        Log::get().note(
            "splash curve: a rendered scene arrived and no game-placed composite was ever seen (the splash was skipped, or its draw did not "
            "match) -- the intro is over and this stands down for the session%s.",
            fault);
    }
    guarded("introCurve.retire", [] { releaseAll(); });
}

}  // namespace

bool introCurveWants() {
    return !g_retired && g_budget.shouldRun() && panelCurveSurfaceWanted();
}

bool introCurveOnComposite(ID3D11DeviceContext* ctx, char kind, uint32_t count, uint32_t instances) {
    // Whatever this call decides, a draw it does not arm leaves nothing armed.
    g_armedGain = 0.0f;
    g_armedToward = 0;
    g_armedReverseU = false;
    if (!ctx || !introCurveWants()) return false;
    // The composite's shape, from the census: a six-index instanced quad, through the intro composite's vertex shader.
    if (kind != 'X' || count != 6 || instances != 1) return false;
    if (bindingShaderHash(BindSlot::Vs) != kIntroCompositeVsHash) return false;
    bool armed = false;
    if (!guardedBudget(g_budget, [&] { armed = recognise(ctx); })) {
        noteStandDown();
        return false;
    }
    return armed;
}

float introCurveGain() {
    return g_armedGain;
}

int introCurveToward() {
    return g_armedToward;
}

bool introCurveReverseU() {
    return g_armedReverseU;
}

void introCurveEndDraw() {
    g_armedGain = 0.0f;
    g_armedToward = 0;
    g_armedReverseU = false;
}

void introCurveTick(ID3D11DeviceContext* ctx, bool sceneFrame) {
    ++g_frame;
    if (g_retired) return;
    if (!g_wantedSeen && panelCurveSurfaceWanted()) g_wantedSeen = true;
    // A rendered scene means the intro is over: stand down for the session whether or not a composite was ever seen -- the scope rule
    // intro_panel.h states, and the one that keeps the on-foot HUD's six-index composite from ever being taken for the splash's.
    if (sceneFrame) {
        retire();
        return;
    }
    if (!g_budget.shouldRun()) {
        releaseAfterFault();
        return;
    }
    if (!guardedBudget(g_budget, [&] { serviceEntries(ctx); })) noteStandDown();
}

void introCurveShutdown() {
    g_armedGain = 0.0f;
    g_armedToward = 0;
    g_armedReverseU = false;
    guarded("introCurve.shutdown", [] { releaseAll(); });
}

IntroCurveInfo introCurveInfo() {
    IntroCurveInfo info;
    info.wanted = introCurveWants();
    info.retired = g_retired;
    info.standDown = !g_budget.shouldRun();
    for (const Entry& e : g_entry) {
        if (e.state != State::kFree) ++info.entries;
        if (e.state == State::kCopying) ++info.copying;
        if (e.state == State::kWorld) ++info.worlds;
        if (e.state == State::kFlat) ++info.flats;
        if (e.stage) ++info.staging;
    }
    info.learned = g_learnedWorld + g_learnedFlat;
    info.rereads = g_rereads;
    info.flatToWorld = g_flatToWorld;
    info.armed = g_armedDraws;
    info.evicted = g_evicted;
    info.expired = g_expired;
    return info;
}

#ifdef EDVR_INTRO_CURVE_RIG
ID3D11Buffer* introCurveStageForTest(void* buffer, void* surface) {
    const Entry* e = findEntry(buffer, surface);
    return e ? e->stage : nullptr;
}

void introCurveResetForTest() {
    releaseAll();
    g_frame = 0;
    g_born = 0;
    g_retired = false;
    g_wantedSeen = false;
    g_evictNoted = false;
    g_capNoted = false;
    g_flatLines = 0;
    g_armedGain = 0.0f;
    g_armedToward = 0;
    g_armedReverseU = false;
    g_learnedWorld = 0;
    g_learnedFlat = 0;
    g_rereads = 0;
    g_flatToWorld = 0;
    g_armedDraws = 0;
    g_evicted = 0;
    g_expired = 0;
    g_budget.~FaultBudget();
    new (&g_budget) FaultBudget("introCurve", kFaultBudget);
}
#endif

}  // namespace edvr
