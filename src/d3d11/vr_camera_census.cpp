// The VR camera census (vr_camera_census.h; the decisions and every line's text are vr_camera_census_core.h).
//
// THREADS. Everything runs on the game's render thread -- the thread that calls Present and so calls
// vrCameraCensusFrameBoundary, which makes it the injector's owner thread -- except the off-thread observer, which is
// lock-free and touches only the VrCensusOffThread table. The refresh detour's two halves (observePre before the game's
// body, observePost after it) run on the owner thread, inside the game's own call: NO I/O, no allocation and no lock
// there, ever. They record into fixed tables; the boundary, once a frame, prints.
//
// KEY OFF. g_wanted is false and g_state is null: the detour is not installed, nothing is allocated, nothing is logged.
// Every entry point returns before touching anything else (tools\vr_camera_census_test's source scan reads these
// functions' first statements). The episodes' per-draw hook (detail::g_vrCensusJoinDraw, vr_camera_census.h) is set only
// by goLive, which only the boundary reaches after the key is on, and is null again the moment the sampled frame is printed
// or the key goes off.
#include "vr_camera_census.h"

#include <windows.h>
#include <d3d11_1.h>
#include <wrl/client.h>

#include <cstring>
#include <new>

#include "../common/config.h"
#include "../common/log.h"
#include "../common/runtime_profile.h"
#include "binding_shadow.h"
#include "depth_probe.h"
#include "flat_camera_inject.h"
#include "flat_camera_phase.h"
#include "flat_compute_readback.h"
#include "journal_watch.h"
#include "temporal_pass.h"
#include "ui_layer.h"
#include "vr_camera_census_core.h"
#include "vr_world_route.h"
#include "vscreen.h"

namespace edvr {
namespace {

using Microsoft::WRL::ComPtr;

// What the refresh's pre half knows while the game's body runs; the post half completes it.
struct Pending {
    bool valid = false;
    VrCensusCall* record = nullptr;   // null when the frame's buffer is full or no sequence is being recorded
    uintptr_t camera = 0, view = 0, ctx = 0;
    uint32_t ordinal = 0, callerRva = 0, draw = 0;
    bool drawKnown = false;
    bool timed = false, injected = false;   // this call's halves are being timed (VrCensusCpu); the detour injected for it
    VrCensusTone tone = VrCensusTone::None;
};

struct State {
    bool active = false;
    VrCensusFoot foot = VrCensusFoot::Off;   // what Elite's journal said at the last boundary
    bool named = false;               // a draw named the 2D screen's source in the frame that just ended (ui_layer.h), read at the boundary
    bool guiKnown = false;            // Status.json's GuiFocus, as the last boundary read it
    uint32_t gui = 0;
    VrCensusPhase phase;              // what the world route chose for the frame in progress, latched at the boundary that opened it
    uint64_t frame = 0;               // the frame in progress, 1-based; 0 before the first boundary
    uint64_t lastWindowMs = 0;
    uint32_t tick = 0;                // 5 s windows since the census started
    uint32_t sequencesLogged = 0;
    bool announced = false, episodesAnnounced = false;
    bool suppressedNoted[static_cast<size_t>(VrCensusLines::kCount)] = {};
    VrCensusFrame current;
    Pending pending;
    VrCensusCameraTable cameras;
    VrCensusOffThread offThread;
    uint64_t offThreadReported = 0;   // the off-thread table's total at the last 5 s line
    VrCensusWindow window;
    VrCensusBudget budget;
    VrCensusEyeBudget eye;
    ComPtr<ID3D11Buffer> staging;     // the 64-byte readback of an eye's rows (and of an episode's join rows), made at the first one
    // The episodes (vr_camera_census_core.h): triggers, the sampled frame's calls, its join, and the two things each 5 s window adds.
    VrCensusEpisodes episodes;
    bool keyOnPending = false;        // the census just turned on: the key-on trigger fires at the boundary that follows
    VrCensusEpisodeFrame epFrame;     // the sampled frame's calls (the frame buffer above stays what it was: the first three on-foot sequences)
    VrCensusJoin join;
    VrCensusDepthCache depthCache;
    const void* joinLastDsv = nullptr;           // the join's last draw: the same depth and shaders again is the same row, and costs three compares
    uint64_t joinLastVs = 0, joinLastPs = 0;
    VrCensusJoinRow* joinLastRow = nullptr;
    bool joinLastRelevant = false;
    VrCensusRuns runs;                // the on-foot naming runs (H2)
    VrCensusCpu cpu;                  // the observer halves' own CPU (D)
    int64_t qpcFreq = 0;
};
State* g_state = nullptr;             // allocated at the first boundary with the key on, never freed
bool g_wanted = false;                // render thread; vrCameraCensusWanted()

// ---- guarded reads of the game's memory (no C++ object lives in a function that has a __try) ------------------------
bool readCamera(uintptr_t camera, VrCensusSnap* out) noexcept {
    __try {
        std::memcpy(out->bytes, reinterpret_cast<const void*>(camera + kVrCensusSnapFrom), kVrCensusSnapBytes);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool readU32(uintptr_t at, uint32_t* out) noexcept {
    __try {
        *out = *reinterpret_cast<const uint32_t*>(at);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

void say(State* s, VrCensusLines cls, const char* line) {
    if (s->budget.take(cls)) { Log::get().note("%s", line); return; }
    // The class is capped: the line is counted, not printed, and the first time says so.
    const size_t i = static_cast<size_t>(cls);
    if (!s->suppressedNoted[i] && s->budget.take(VrCensusLines::Info)) {
        s->suppressedNoted[i] = true;
        Log::get().note("vr camera census: line budget reached for %s lines (cap %u): further lines of that kind are "
                        "counted, not printed", vrCensusLinesName(cls), VrCensusBudget::kCap[i]);
    }
}

// The clock for the observer halves' own CPU (D): the performance counter, read only for the one call in kEvery that is timed.
int64_t ticksNow() noexcept {
    LARGE_INTEGER t{};
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

// ---- the refresh detour's observer (flat_camera_inject.h) ----------------------------------------------------------
// Pre: the call's place in the frame and its record. Returns true: the return is redirected so the post half runs.
bool observePre(const FlatCameraObserveCall& call) noexcept {
    State* s = g_state;
    if (!s || !s->active) return false;
    const bool timed = s->cpu.pick();   // one call in kEvery has its two halves timed (VrCensusCpu): a counter, and the clock only on that call
    const int64_t t0 = timed ? ticksNow() : 0;
    uint32_t draw = 0;
    bool toneSeen = false;
    uint64_t routeFrame = 0;
    const bool have = vrWorldRouteDrawProgress(&draw, &toneSeen, &routeFrame);
    const VrCensusTone tone = !have ? VrCensusTone::None : toneSeen ? VrCensusTone::After : VrCensusTone::Before;
    if (have) {
        s->window.progressSeen = true;
        s->current.progress = true;
        if (toneSeen) s->current.toneSeen = true;
    }
    const uint32_t callerRva = static_cast<uint32_t>(call.callerRva);
    s->window.noteCall(call.kindReadable, call.kind, callerRva, call.camera, tone, call.window != 0, call.willInject);
    // The sampled frame of an episode records EVERY call (aboard ones included), into the buffer of its own, tallied by kind and caller over all of
    // them. Otherwise a sequence is recorded only while one is still wanted and neither the journal nor the route's phase rules the frame out (the last
    // one is printed at the boundary that finds its frame sampled); any other call is counted and nothing else.
    const bool episodeFrame = s->episodes.live();
    const bool recording = !episodeFrame && vrCensusMayRecord(s->foot, s->phase) && vrCensusPrintsSequence(true, s->sequencesLogged);
    VrCensusCall* record = nullptr;
    if (episodeFrame) { ++s->current.calls; record = s->epFrame.add(call.kindReadable, call.kind, callerRva); }
    else if (recording) record = s->current.add();
    else ++s->current.calls;
    Pending& p = s->pending;
    p.valid = true;
    p.record = record;
    p.camera = call.camera;
    p.view = call.p2;
    p.ctx = call.ctx;
    p.ordinal = s->current.calls;
    p.callerRva = callerRva;
    p.draw = draw;
    p.drawKnown = have;
    p.tone = tone;
    p.timed = timed;
    p.injected = call.willInject;
    if (record) {
        record->camera = call.camera;
        record->view = call.p2;
        record->kind = call.kind;
        record->kindReadable = call.kindReadable;
        record->willInject = call.willInject;   // what the detour decided for this call, before the body runs
        record->role = call.role;
        record->callerRva = callerRva;
        record->draw = draw;
        record->drawKnown = have;
        record->tone = tone;
        uint32_t flags = 0;
        record->preFlags = readU32(call.camera + kVrCensusFlags, &flags) ? flags : 0;
    }
    if (timed) s->cpu.notePre(call.willInject, static_cast<uint64_t>(ticksNow() - t0));
    return true;
}

// The post half's own time, when its call is the one in kEvery being timed: read at its start, recorded however the half returns.
struct PostTimer {
    State* s;
    bool timed, injected;
    int64_t t0;
    PostTimer(State* state, bool isTimed, bool wasInjected) noexcept
        : s(state), timed(isTimed), injected(wasInjected), t0(isTimed ? ticksNow() : 0) {}
    ~PostTimer() { if (timed) s->cpu.notePost(injected, static_cast<uint64_t>(ticksNow() - t0)); }
    PostTimer(const PostTimer&) = delete;
    PostTimer& operator=(const PostTimer&) = delete;
};

// Post: what the body derived. One guarded copy of the camera, the rows the composer wrote for it, its tangents.
void observePost(uintptr_t camera, uintptr_t /*ctx*/) noexcept {
    State* s = g_state;
    if (!s || !s->active) return;
    const Pending p = s->pending;
    s->pending.valid = false;
    if (!p.valid || p.camera != camera) return;
    const PostTimer timer(s, p.timed, p.injected);
    ++s->window.posts;
    VrCensusSnap snap;
    if (!readCamera(camera, &snap)) return;
    VrCensusSig sig;
    vrCensusSigFromSnap(snap, &sig);
    float tangents[4] = {};
    const bool tangentsValid = vrCensusTangents(sig, tangents);
    if (p.record) {   // the rows are only for a call that is being recorded: none once the sequences are out
        p.record->postSeen = true;
        p.record->postFlags = sig.flags;
        p.record->rowsValid = vrCensusComposeRows(snap, p.record->rows);
        std::memcpy(p.record->axes, snap.bytes + (kVrCensusAxes - kVrCensusSnapFrom), sizeof(p.record->axes));   // the view axes: an episode matches the pass's rows to them
        p.record->axesValid = true;
    }
    VrCensusCameraTable::Call first;
    first.frame = s->frame;
    first.callerRva = p.callerRva;
    first.ordinal = p.ordinal;
    first.draw = p.draw;
    first.drawKnown = p.drawKnown;
    first.tone = p.tone;
    first.view = p.view;
    first.ctx = p.ctx;
    s->cameras.note(camera, sig, tangents, tangentsValid, first);
}

// Any thread but the owner's: counted into the lock-free table, nothing more.
void observeOffThread(uintptr_t camera, uint64_t callerRva, uint32_t kind, bool kindReadable, uint32_t thread) noexcept {
    State* s = g_state;
    if (!s) return;
    s->offThread.note(thread, camera, kind, kindReadable, callerRva);
}

const FlatCameraObserver g_observer = {&observePre, &observePost, &observeOffThread};

// What Elite's own journal says about the commander, asked once a frame at the boundary (journal_watch.h: atomic peeks).
VrCensusFoot currentFoot() {
    return vrCensusFootFrom(journalWatchActive(), journalOnFootKnown(), journalOnFoot());
}

// What the world route chose for the frame that is running NOW (vr_world_route.h vrWorldRouteWorldPhase): whether it is
// jittering this frame and the phase it asked the injector for, in render pixels. The route's boundary runs before the census's,
// so asked at the census's boundary it is the frame that STARTS there (latched into s->phase for that frame's whole run, its
// sequence header and its sampling decision at the next boundary), and asked at an eye draw it is the running frame's.
// False (not jittering) leaves the phase out of the decision. This is the census's one question of the route's phase.
VrCensusPhase readPhase() {
    VrCensusPhase p;
    float x = 0.0f, y = 0.0f;
    p.jittering = vrWorldRouteWorldPhase(&x, &y);
    p.x = x;
    p.y = y;
    return p;
}

// ---- the key ---------------------------------------------------------------------------------------------------------
bool readWanted() {
    if (!runtimeVrProfile()) return false;   // a flat profile reads the key off already (Config refuses it); asked twice
    const std::string text = Config::get().getString("advanced.vr_camera_census", "off");
    return vrCameraCensusWantedFor(true, vrCameraCensusKeyFromText(text.c_str()));
}

// ---- the boundary's work ------------------------------------------------------------------------------------------
void activate(State* s) {
    s->active = true;
    s->foot = currentFoot();
    s->phase = VrCensusPhase{};                 // the first boundary latches the first frame's
    s->frame = 0;
    s->current.reset();
    s->pending = Pending{};
    s->window.reset();
    s->lastWindowMs = GetTickCount64();
    // The episodes' state starts afresh with the census (their session counters and cap stay): no flip is judged against a reading from before it
    // ran, the key-on trigger fires at the boundary that follows, and the naming runs, the observer timing and the per-draw hook start clean.
    s->named = false;
    s->guiKnown = false;
    s->gui = 0;
    s->episodes.restart();
    s->keyOnPending = true;
    s->runs = VrCensusRuns{};
    s->cpu.resetWindow();
    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    s->qpcFreq = frequency.QuadPart;
    detail::g_vrCensusJoinDraw = nullptr;
    flatCameraInjectSetObserver(&g_observer);   // before the hook can exist: its first call already reports
    flatCameraInjectPause(false);               // a hook that was paused by a key-off reopens its gate
    if (!s->announced && s->budget.take(VrCensusLines::Info)) {
        s->announced = true;
        Log::get().note("vr camera census: on (advanced.vr_camera_census); the census itself never writes a camera (the route's injection, when it runs, is its own); "
                        "owner thread %lu; 5 s lines: %u windows, then one per %u; cameras first %u, call sequences first %u and "
                        "eye draws first %u on-foot frames (tone drawn, journal read=%s says on foot; while the route jitters, "
                        "only a non-zero phase); line budget %u",
                        static_cast<unsigned long>(GetCurrentThreadId()), kVrCensusEveryWindow, kVrCensusThinTo,
                        static_cast<unsigned>(VrCensusCameraTable::kCapacity), kVrCensusMaxSequences, kVrCensusMaxEyeFrames,
                        journalWatchActive() ? "yes" : "no: the tone alone decides", VrCensusBudget::capTotal());
    }
    if (!s->episodesAnnounced && s->budget.take(VrCensusLines::Info)) {
        s->episodesAnnounced = true;
        Log::get().note("vr camera census: episodes: up to %u, one frame each, %u frames after a trigger (journal on-foot flip, naming flip held %u frames, GuiFocus change, "
                        "key on); each logs its calls, its first draws into the screen's and eyes' depths joined to the b1 rows read, the pass's rows; 5 s windows add "
                        "the episodes' counters, naming runs and the observer's CPU (1 call in %u timed)",
                        kVrCensusMaxEpisodes, kVrCensusEpisodeDelay, kVrCensusNamingHold, VrCensusCpu::kEvery);
    }
}

void deactivate() {
    g_wanted = false;
    detail::g_vrCensusJoinDraw = nullptr;       // an episode's frame that was running is dropped with the census: no draw reaches the join again
    flatCameraInjectSetObserver(nullptr);
    // The relay's gate closes, so the game's refresh runs straight through, only when the detour is quiet: while the world route
    // injects (or a camera it injected still waits for its flush) it is the route's to keep open, and a census key-off must not close it
    // for a frame (flatCameraVrQuiet is true for a census-only process, where nothing else needs the gate).
    flatCameraInjectPause(flatCameraVrQuiet());
    State* s = g_state;
    if (!s) return;
    s->active = false;
    s->pending = Pending{};
    s->current.reset();
    s->staging.Reset();
    s->episodes.abandon();
    if (s->budget.take(VrCensusLines::Info))
        Log::get().note("vr camera census: off (advanced.vr_camera_census); the census observer is detached and the refresh hook stays "
                        "in place (the route's, if it injects) until the game exits");
}

void printCameraLines(State* s) {
    char line[kVrCensusLineBytes + 16];
    for (size_t i = 0; i < s->cameras.used(); ++i) {
        if (s->cameras.takeCameraLine(i)) {
            vrCensusFormatCamera(line, kVrCensusLineBytes + 1, s->cameras.at(i));
            say(s, VrCensusLines::Camera, line);
        }
        if (s->cameras.takeChangeLine(i)) {
            vrCensusFormatChanged(line, kVrCensusLineBytes + 1, s->cameras.at(i));
            say(s, VrCensusLines::Changed, line);
        }
    }
}

void printOffThread(State* s) {
    char line[kVrCensusLineBytes + 16];
    VrCensusOffThread::Entry e;
    while (s->offThread.takeNew(&e)) {
        vrCensusFormatOtherThread(line, kVrCensusLineBytes + 1, e);
        say(s, VrCensusLines::Thread, line);
    }
}

// The frame that ended was an on-foot frame (the tone was seen): its whole call sequence, in order.
void printSequence(State* s) {
    char line[kVrCensusLineBytes + 16];
    const VrCensusFrame& f = s->current;
    ++s->sequencesLogged;
    vrCensusFormatSequence(line, kVrCensusLineBytes + 1, s->frame, s->sequencesLogged, s->foot, s->phase, f.calls, f.recorded);
    say(s, VrCensusLines::Call, line);
    for (uint32_t i = 0; i < f.recorded; ++i) {
        vrCensusFormatCall(line, kVrCensusLineBytes + 1, s->frame, i + 1, f.call[i]);
        say(s, VrCensusLines::Call, line);
    }
}

// The sampled frame of an episode has ended (the boundary that follows it, before anything is reset): its header, the calls that print, the temporal pass's
// chosen rows with the calls whose view axes they equal, and the join. Every match is made here, in memory, over ALL the calls the frame recorded -- not only
// the ones that print -- so a call that is not printed (the line cap) is still found, and the calls that matched something print first.
void printEpisode(State* s) {
    VrCensusEpisodePrint in;
    in.n = s->episodes.started();
    in.frame = s->frame;
    in.armedFrame = s->episodes.armedFrame();
    in.trigger = s->episodes.trigger();
    in.foot = s->foot;
    in.guiKnown = s->guiKnown;
    in.gui = s->gui;
    in.named = s->named;
    in.phase = s->phase;
    // The pass's chosen rows: read-only, and standing now (its own boundary, which resets them, runs after the census's).
    in.haveChosen = temporalPassChosenRows(in.chosen, &in.chosenBound);
    vrCensusPrintEpisode([s](VrCensusLines cls, const char* line) { say(s, cls, line); }, in, s->epFrame, s->join);
}

// The frame that just ended is accounted and printed; the next one starts empty.
void rollFrame(State* s) {
    if (s->frame) {
        // The frame that ended was run under the phase latched at the boundary that opened it (s->phase, not yet replaced).
        const bool sampled = vrCensusSamplesFrame(s->current.toneSeen, s->current.progress, s->foot, s->phase);
        ++s->window.frames;
        if (s->current.toneSeen) ++s->window.toneFrames;
        if (sampled) ++s->window.onFootFrames;
        s->runs.noteFrame(s->foot == VrCensusFoot::Yes, s->named);   // H2: the journal says on foot, and did a draw name the screen's source
        printCameraLines(s);      // a camera's line precedes the sequence that names it
        printOffThread(s);
        // The frame the journal flips at was recorded under the old word (nothing): it has no calls to print and must not
        // spend one of the sequences as an empty header. The next frame is recorded under the new word.
        if (vrCensusPrintsSequence(sampled, s->sequencesLogged) && s->current.recorded > 0) printSequence(s);
        // The sampled frame of an episode: it records into its own buffer, so the sequence above never printed it; its lines follow the camera lines
        // that name its cameras, and the per-draw hook is let go before anything else can run.
        if (s->episodes.live()) {
            detail::g_vrCensusJoinDraw = nullptr;
            printEpisode(s);
            s->episodes.finish();
        }
    }
    s->current.reset();
    s->pending = Pending{};
    ++s->frame;
}

void tickWindow(State* s) {
    const uint64_t now = GetTickCount64();
    if (now - s->lastWindowMs < 5000) return;
    s->lastWindowMs = now;
    ++s->tick;
    ++s->window.windows;
    if (!vrCensusWindowPrints(s->tick)) return;   // the counters keep adding; the line that prints covers every window
    VrCensusWindowText text;
    text.hook = flatCameraInjectObserveStatus();
    text.foot = s->foot;
    const uint64_t total = s->offThread.total();
    text.offThread = total - s->offThreadReported;
    text.offThreadOverflow = s->offThread.overflow();
    text.camerasTotal = static_cast<uint32_t>(s->cameras.used());
    text.cameraTableOverflow = s->cameras.overflow();
    char line[kVrCensusLineBytes + 16];
    vrCensusFormatWindow(line, kVrCensusLineBytes + 1, s->window, text);
    say(s, VrCensusLines::Window, line);
    // The window's three companions, over the same windows (the 5 s line is full at 400 characters): the episodes' counters -- zeros included, so an absent line is what "the
    // episode code never ran" looks like --, the on-foot naming runs and the observer halves' CPU (counts that end with this print).
    VrCensusEpisodeCounters counters;
    counters.taken = s->episodes.started();
    counters.triggers = s->episodes.triggers();
    counters.skipped = s->episodes.skipped();
    counters.state = s->episodes.stateName();
    counters.trigger = s->episodes.trigger();
    counters.armedFrame = s->episodes.armedFrame();
    counters.sampleFrame = s->episodes.sampleFrame();
    vrCensusFormatEpisodeCounters(line, kVrCensusLineBytes + 1, counters, s->window.windows, !s->episodes.idle());
    say(s, VrCensusLines::Counters, line);
    vrCensusFormatRuns(line, kVrCensusLineBytes + 1, s->runs, s->window.windows);
    say(s, VrCensusLines::Runs, line);
    vrCensusFormatCpu(line, kVrCensusLineBytes + 1, s->window, s->cpu, s->qpcFreq, s->window.windows);
    say(s, VrCensusLines::Cpu, line);
    s->runs.resetWindow();
    s->cpu.resetWindow();
    s->offThreadReported = total;
    s->window.reset();
}

// ---- the eye draw's readback ---------------------------------------------------------------------------------------
// VS constant buffer slot 1 -> a 64-byte staging copy of rows 270..273 -> Map. One stall per call, at most eight calls a
// session (and, for an episode's join, the first draw of each kind of depth: at most four a frame, ten frames a session).
// The census's own D3D calls step past the hooks (FlatComputeInternalScope). With wantRows false it only says which buffer
// is bound at slot 1, where it starts and how big it is (the join's signatures after the first of a depth): no copy, no stall.
bool readEyeRows(State* s, ID3D11DeviceContext* ctx, float rows[16], uint64_t* b1Out, uint32_t* firstOut, uint32_t* bytesOut,
                 const char** why, bool wantRows = true) {
    ComPtr<ID3D11Buffer> b1;
    UINT first = 0, num = 0;
    ComPtr<ID3D11DeviceContext1> ctx1;
    if (SUCCEEDED(ctx->QueryInterface(IID_PPV_ARGS(&ctx1))) && ctx1) ctx1->VSGetConstantBuffers1(1, 1, b1.GetAddressOf(), &first, &num);
    else ctx->VSGetConstantBuffers(1, 1, b1.GetAddressOf());
    if (!b1) { *why = "no-b1"; return false; }
    D3D11_BUFFER_DESC bd{};
    b1->GetDesc(&bd);
    *b1Out = reinterpret_cast<uint64_t>(b1.Get());
    *firstOut = first;
    *bytesOut = bd.ByteWidth;
    if (!wantRows) return true;
    const uint32_t offset = (first + 270u) * 16u;
    if (bd.ByteWidth < offset + 64u) { *why = "b1-too-small"; return false; }
    if (!s->staging) {
        ComPtr<ID3D11Device> device;
        b1->GetDevice(&device);
        D3D11_BUFFER_DESC sd{};
        sd.ByteWidth = 64;
        sd.Usage = D3D11_USAGE_STAGING;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (!device || FAILED(device->CreateBuffer(&sd, nullptr, &s->staging))) { *why = "staging"; return false; }
    }
    const D3D11_BOX box{offset, 0, 0, offset + 64u, 1, 1};
    ctx->CopySubresourceRegion(s->staging.Get(), 0, 0, 0, 0, b1.Get(), 0, &box);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(ctx->Map(s->staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)) || !mapped.pData) { *why = "map"; return false; }
    std::memcpy(rows, mapped.pData, 64);
    ctx->Unmap(s->staging.Get(), 0);
    return true;
}

// ---- the episodes' join -----------------------------------------------------------------------------------------------
// What a depth view the draws bound is to the join: the 2D screen's depth (its size is the panel's), an eye's (an eye-sized one; which eye the depth probe's
// settled scene pair says, else unknown), or nothing it joins. Resolved once a view per episode frame (the cache), through the shadow's guarded instrument resolver.
const VrCensusDepthCache::Entry* classifyDepth(State* s, void* dsv) {
    ++s->join.views;
    ResourceInfo info{};
    bool relevant = false;
    VrCensusJoinDepth depth = VrCensusJoinDepth::Screen;
    uint32_t w = 0, h = 0;
    if (bindingResolveProbe(dsv, &info) && info.isTexture2D) {
        w = info.a;
        h = info.b;
        uint32_t panelW = 0, panelH = 0;
        if (vScreenPanelSize(&panelW, &panelH) && w == panelW && h == panelH) {
            relevant = true;
        } else if (vScreenIsEyeSized(w, h)) {
            relevant = true;
            depth = VrCensusJoinDepth::EyeUnknown;
            int eye = -1, target = -1;
            if (depthProbeCurrentSceneEyeOf(static_cast<ID3D11DepthStencilView*>(dsv), &eye, &target) && (eye == 0 || eye == 1))
                depth = eye == 0 ? VrCensusJoinDepth::Eye0 : VrCensusJoinDepth::Eye1;
        }
    }
    return s->depthCache.put(dsv, relevant, depth, w, h);
}

// Does the bound depth-stencil state write depth? An unset state is the default one: depth test on, writes all.
int8_t depthWriteOf(ID3D11DeviceContext* ctx) {
    ComPtr<ID3D11DepthStencilState> state;
    UINT reference = 0;
    ctx->OMGetDepthStencilState(&state, &reference);
    if (!state) return 1;
    D3D11_DEPTH_STENCIL_DESC desc{};
    state->GetDesc(&desc);
    return desc.DepthEnable && desc.DepthWriteMask == D3D11_DEPTH_WRITE_MASK_ALL ? 1 : 0;
}

// The facts of a signature's first draw: whether it writes depth and which constant buffer is at VS slot 1 (its size), and for the first signature of a depth the
// rows 270..273 as well (one stall). The census's own state reads and copy step past the hooks.
void readJoinFacts(State* s, ID3D11DeviceContext* ctx, VrCensusJoinRow* row, bool wantRows) {
    FlatComputeInternalScope internal;   // the hooks step aside for the join's state reads, copy, Map and Unmap
    row->depthWrite = depthWriteOf(ctx);
    float rows[16] = {};
    uint64_t b1 = 0;
    uint32_t first = 0, bytes = 0;
    const char* why = nullptr;
    const bool read = readEyeRows(s, ctx, rows, &b1, &first, &bytes, &why, wantRows);
    if (b1) {
        row->b1Bound = true;
        row->b1 = b1;
        row->b1First = first;
        row->b1Bytes = bytes;
    }
    row->rowsAsked = wantRows;
    if (!wantRows) { row->why = "skip"; return; }
    if (read) {
        row->rowsRead = true;
        std::memcpy(row->rows, rows, sizeof(row->rows));
    } else {
        row->why = why;
    }
}

// The route's per-draw hook calls this (through detail::g_vrCensusJoinDraw, set for the sampled frame of an episode only) for every game draw on the owner
// context, before the game's own draw is issued. The draw's depth, vertex shader and pixel shader are one signature; a draw into a depth that is neither the
// screen's nor an eye's is nothing to the join. The common draw costs a few loads and compares; a new signature costs a few state reads, and the first of a
// depth the readback of its rows.
void joinDraw(ID3D11DeviceContext* ctx, uint32_t ordinal) {
    State* s = g_state;
    if (!s || !s->active || !ctx || !s->episodes.live()) return;
    ++s->join.seen;                      // every draw the hook is handed: a frame that prints seen=0 never reached it
    void* dsv = bindingGet(BindSlot::Dsv0);
    if (!dsv) return;
    const uint64_t vs = bindingShaderHash(BindSlot::Vs), ps = bindingShaderHash(BindSlot::Ps);
    if (dsv == s->joinLastDsv && vs == s->joinLastVs && ps == s->joinLastPs) {   // the same depth and shaders as the draw before: the same row (or still nothing)
        if (s->joinLastRow) ++s->joinLastRow->draws;
        else if (s->joinLastRelevant) ++s->join.overflowDraws;
        return;
    }
    s->joinLastDsv = dsv;
    s->joinLastVs = vs;
    s->joinLastPs = ps;
    s->joinLastRow = nullptr;
    s->joinLastRelevant = false;
    const VrCensusDepthCache::Entry* d = s->depthCache.find(dsv);
    if (!d) d = classifyDepth(s, dsv);
    if (!d->relevant) return;
    s->joinLastRelevant = true;
    VrCensusJoinRow* row = s->join.find(d->depth, vs, ps);
    if (row) { ++row->draws; s->joinLastRow = row; return; }
    const bool firstOfDepth = !s->join.hasDepth(d->depth);
    row = s->join.add(d->depth, d->w, d->h, vs, ps, ordinal);
    if (!row) return;                    // the table is full: counted in the join's overflow
    s->joinLastRow = row;
    readJoinFacts(s, ctx, row, firstOfDepth);
}

// The armed frame is the one that starts now: its calls go into the episode's buffer, and the route's per-draw hook reaches the join until the boundary that ends it.
void goLive(State* s) {
    s->epFrame.reset();
    s->join.begin();
    s->depthCache.clear();
    s->joinLastDsv = nullptr;
    s->joinLastVs = s->joinLastPs = 0;
    s->joinLastRow = nullptr;
    s->joinLastRelevant = false;
    detail::g_vrCensusJoinDraw = &joinDraw;
}

// The episodes' step of a boundary, after the frame that ended was rolled (s->frame is the frame that starts): the key-on trigger, the triggers the ended frame's
// readings make (the journal's on-foot flips, the naming held, GuiFocus), and the armed frame going live. A trigger that was skipped is said once.
void episodeStep(State* s) {
    VrCensusEpisodes::Inputs in;
    in.foot = s->foot;
    in.named = s->named;
    in.guiKnown = s->guiKnown;
    in.gui = s->gui;
    if (s->keyOnPending) {
        s->keyOnPending = false;
        s->episodes.keyOn(s->frame);
    }
    const bool ended = s->frame > 1;      // frame 1 starts at the boundary that turned the census on: nothing has ended before it
    if (s->episodes.boundary(s->frame, ended, in)) goLive(s);
    const char* why = nullptr;
    if (s->episodes.takeSkipNote(&why) && s->budget.take(VrCensusLines::Info))
        Log::get().note("vr camera census: episodes: a trigger (%u of %u so far) arrived while %s: it is counted (triggers= and skipped= in the 5 s lines) and never "
                        "sampled; this is said once", s->episodes.skipped(), s->episodes.triggers(), why);
}

}  // namespace

// ---- the public entry points ------------------------------------------------------------------------------------
bool vrCameraCensusWanted() { return g_wanted; }

void vrCameraCensusFrameBoundary() {
    const bool wanted = readWanted();
    if (!wanted) {
        if (g_wanted) deactivate();   // the key went off while the census ran: detach, close the gate, say so once
        return;
    }
    State* s = g_state;
    if (!s) {
        s = new (std::nothrow) State;
        if (!s) return;
        g_state = s;                   // published before the observer is registered (the release store below)
    }
    if (!s->active) activate(s);
    g_wanted = true;
    flatCameraInjectDisarm();          // the Present edge: the window closes and THIS thread is the owner
    s->foot = currentFoot();           // the journal's word for the frame that ended, and for the one that starts
    s->named = uiLayerLastFrameNamed();// did a draw name the 2D screen's source in the frame that ended (the layer's boundary ran first: ui_layer.h)
    s->guiKnown = journalGuiFocus(&s->gui);   // Status.json's GuiFocus, when it is readable (on foot it is not)
    rollFrame(s);                      // the frame that ended, under the phase latched when it began
    episodeStep(s);                    // the triggers its readings make, and the armed frame that starts now going live
    s->phase = readPhase();            // the route's boundary ran first: this is the choice for the frame that starts
    flatCameraInjectObserveFrame();    // installs the hook once (observe-only), opens the window
    tickWindow(s);
}

void vrCameraCensusEyeDraw(ID3D11DeviceContext* ctx, uint32_t eye) {
    if (!g_wanted) return;
    State* s = g_state;
    if (!s || !s->active || !ctx || eye > 1) return;
    ++s->window.eyeDraws;
    uint32_t draw = 0;
    bool toneSeen = false;
    uint64_t routeFrame = 0;
    const bool have = vrWorldRouteDrawProgress(&draw, &toneSeen, &routeFrame);
    if (have) {
        s->window.progressSeen = true;
        s->current.progress = true;
        if (toneSeen) s->current.toneSeen = true;
    }
    // Only a frame the census samples is read back: the tone drawn before this draw and the journal, when it is read, saying
    // on foot; or, with no draw progress at all, the journal alone saying on foot; and, while the route jitters, a non-zero
    // phase (asked here, where the running frame's choice is wanted, and printed on the line).
    const VrCensusPhase phase = readPhase();
    if (!vrCensusSamplesFrame(toneSeen, have, s->foot, phase)) return;
    ++s->window.eyeOnFoot;
    if (!s->eye.take(s->frame)) return;

    float rows[16] = {};
    uint64_t b1 = 0;
    uint32_t first = 0, bytes = 0;
    const char* why = nullptr;
    bool read = false;
    {
        FlatComputeInternalScope internal;   // the hooks step aside for the census's own copy, Map and Unmap
        read = readEyeRows(s, ctx, rows, &b1, &first, &bytes, &why);
    }
    double measX = 0, measY = 0;
    bool measured = false;
    if (read) {
        float six[6][4] = {};   // rows 270..275 in flatCameraMeasureRowShift's shape; 274 and 275 are unused by it
        std::memcpy(six, rows, sizeof(rows));
        measured = flatCameraMeasureRowShift(six, measX, measY);
    }
    char line[kVrCensusLineBytes + 16];
    vrCensusFormatEye(line, kVrCensusLineBytes + 1, eye, s->frame, s->foot, phase, true, draw, b1, first, bytes,
                      read ? rows : nullptr, measured, measX, measY, read ? nullptr : why);
    say(s, VrCensusLines::Eye, line);
    uint64_t sequence = 0;
    float frustum[4] = {}, shift[2] = {};
    const bool known = nativeTemporalEyeGeometry(eye, &sequence, frustum, shift);
    vrCensusFormatEyeGeometry(line, kVrCensusLineBytes + 1, eye, s->frame, known, sequence, frustum, shift, measured, measX,
                              measY);
    say(s, VrCensusLines::Eye, line);
}

}  // namespace edvr
