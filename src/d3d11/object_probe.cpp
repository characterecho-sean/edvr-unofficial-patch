#include "object_probe.h"

#include <cstdio>
#include <cstring>
#include <vector>

#include <windows.h>

#include <d3d11.h>

#include "../common/config.h"
#include "../common/guard.h"
#include "../common/log.h"
#include "../common/temporal_mode.h"
#include "../common/timing.h"
#include "binding_shadow.h"
#include "draw_census.h"
#include "depth_probe.h"
#include "eye_draw_snapshot.h"
#include "object_classification_probe.h"
#include "eye_tonemap_snapshot.h"
#include "eye_panel_snapshot.h"
#include "eye_depth_capture.h"
#include "gui_draw_snapshot.h"
#include "cull_gate_probe.h"
#include "kinematic_eval_hook.h"

namespace edvr {

// objectProbeWantsDraws and objectProbeLedgerActive read these from the
// header with no call: asked per eye draw, and the build has no /GL to
// fold a cross-TU getter.
namespace detail {
bool g_objectProbeOn = false;
bool g_objectProbeLedgerOn = false;
}  // namespace detail

namespace {

constexpr uint32_t kRecordBytes = 336;   // docs/per-object-motion.md, question 5: 71 of 71 shaders
constexpr uint32_t kPoolSlot = 33;       // ...at t33, likewise
constexpr uint32_t kChecksPerFrame = 4;  // instanced eye draws asked for t33 before a frame gives up
constexpr uint64_t kRecheckMs = 1000;    // once the pool is known, one look a second
constexpr uint32_t kReadAfter = 2;       // frames before a copy is asked for (never waited on)
constexpr uint32_t kDropAfter = 30;      // ...and after which a copy still in flight is given up
constexpr int      kRing = 6;            // the ledger's per-frame pool copies, two to three in flight
constexpr uint32_t kAbsentFrames = 600;  // frames with the probe on and no pool before saying so

bool     g_verbose = false;   // advanced.object_probe: the absent-pool note
bool     g_wasOn = false;
uint32_t g_checksLeft = 0;
uint64_t g_checkMs = 0;
ID3D11Buffer* g_pool = nullptr;      // held (AddRef) while recognised
uint32_t g_poolBytes = 0;
uint32_t g_records = 0;
bool     g_noted = false;
uint32_t g_frame = 0;
uint32_t g_framesWithoutPool = 0;
bool     g_absentNoted = false;
bool     g_dumpDirMade = false;

// Minimal since 2026-09-23: this ring fed the estimation's pair-copy (kept/
// diffed roles, a scene co-copy for the camera fallback, a QPC stamp for the
// pair's interval) as well as the ledger's own per-frame pool copy. Only the
// ledger's need survives -- one staging copy of the pool, read back late.
struct Slot {
    ID3D11Buffer* staging = nullptr;
    uint32_t bytes = 0;
    uint32_t frame = 0;
    bool     inUse = false;
};
Slot g_ring[kRing];

// THE EYE RUN'S LEDGER (object_probe.h says why). Held only between the arm
// and the write, some twenty frames; the copies ride the pool's own staging
// pattern -- issued at the boundary, read back late, never waited on.
constexpr int      kLedgerFrames = 20;          // the run's sixteen crops and slack either side
constexpr uint32_t kLedgerBonesMax = 1u << 20;  // the palette's first megabyte: rows to 21845; the station's bases reached 16413 (2026-09-10)
constexpr int      kLedgerRing = 4;
constexpr int      kLedgerCrops = 32;
// Row layout is versioned by the draws file's header (version 1 = 24-byte
// rows, version 2 = 40-byte rows with ps/rt): tools/eye_run_ledger.py parses
// both, so the two must stay in step.
struct LedgerDraw {
    uint64_t vs;             // the bound vertex shader's hash (the binding shadow's; 0 = one the registry had not met)
    uint32_t count;          // the vertex or index count
    uint32_t instances;
    uint32_t startInstance;  // StartInstanceLocation: where its records' indices sit in the instance stream
    uint32_t pad0;           // version 2: keeps ps 8-aligned; zero
    uint64_t ps;             // version 2: the bound pixel shader's hash, same source as vs (0 = unknown)
    int32_t  rt;             // version 2: the census's intern id of the bound RTV (drawCensusIntern):
                             // >= 0 the same @N a census line's r= token carries, -1 no RTV bound
                             // (a depth-only or shadow pass), -2 the census's table was full
    uint8_t  kind;           // 'D' 'I' 'N' 'X'
    uint8_t  pool;           // 1 = t33 HELD the pool at this draw (asked of the context; armed only) -- which
                             // it does on every draw after a pool draw, the game never unbinding it;
                             // whether the shader READS it is the desk's question (tools/eye_run_ledger.py --pool-vs)
    uint16_t pad;
};
static_assert(sizeof(LedgerDraw) == 40, "tools/eye_run_ledger.py reads 40-byte rows (version 2)");
struct LedgerCopy {
    ID3D11Buffer* staging = nullptr;
    uint32_t bytes = 0;
    uint32_t frame = 0;
    bool     inUse = false;
};
// What wants a pool copy every frame: the ledger alone, for the run's
// per-frame pool file (the stepped parts' tracking that also did, and its
// diagnostics opt-in, retired with the estimation on 2026-09-23).
bool ledgerCopyWanted() { return detail::g_objectProbeLedgerOn; }
wchar_t  g_ledgerStamp[16] = L"";
uint32_t g_ledgerFrame0 = 0, g_ledgerLastFrame = 0;
int      g_ledgerCropFrame[kLedgerCrops];
std::vector<uint8_t>    g_ledgerPool[kLedgerFrames];
std::vector<uint8_t>    g_ledgerInst[kLedgerFrames];
std::vector<LedgerDraw> g_ledgerDraws[kLedgerFrames];
ID3D11Buffer* g_inst = nullptr;    // the instance stream, held (AddRef) while armed
uint32_t g_instBytes = 0, g_instStride = 0;
// The palettes: every distinct 48-byte structured buffer a pool draw binds
// at t38 while armed, up to four (the first run's single copy, the 8 MB
// one, read all zeros at the bases the hub's records carry: the census of
// 2026-09-09 counted four such buffers, and the hub's is another).
constexpr int kLedgerPalettes = 4;
ID3D11Buffer* g_palette[kLedgerPalettes] = {};   // held while armed
uint32_t g_paletteBytes[kLedgerPalettes] = {};
int      g_paletteCount = 0;
bool     g_paletteSeen[kLedgerPalettes] = {};   // copied at this frame's first pool draw binding it (not at the boundary: the run of 05:37 read zeros there, the game having discarded them for the next frame)
// [0] the instance stream's copies, [1..4] the palettes'
LedgerCopy g_ledgerRing[1 + kLedgerPalettes][kLedgerRing];
std::vector<uint8_t> g_ledgerPalette[kLedgerPalettes][kLedgerFrames];
uint32_t g_ledgerSkipped = 0;
// THE AUX CAPTURES: the big instanced draws that read no pool -- a station's
// ring built of segments, its lights, a sprite batch -- placed by a world
// matrix in their per-draw constant buffer (cb2, the rows the dumped
// shaders multiply by) and per-instance data at t0 or in a vertex stream.
// The first draw of each such shader in a frame has cb2, t0 and its first
// two vertex buffers copied (kLedgerAuxBytes of each at most), for how each
// turns from frame to frame.
constexpr int      kLedgerAux = 8;            // shaders watched, in order of first appearance
constexpr uint32_t kLedgerAuxMin = 50;        // instances a draw needs to be watched
constexpr uint32_t kLedgerAuxBytes = 65536;   // of each buffer
constexpr uint64_t kOrbitalLineVs = 0xC7FA0C0F5DD49180ull;
constexpr int      kAuxWhat = 4;              // cb2, t0, vb0, vb1
struct AuxSlot {
    uint64_t vs = 0;
    uint32_t instances = 0, count = 0;
    uint32_t stride[kAuxWhat] = {};   // t0's structure stride, the vertex buffers' strides
    bool     seenThisFrame = false;
    LedgerCopy ring[kAuxWhat][kLedgerRing];
};
AuxSlot g_aux[kLedgerAux];
int     g_auxCount = 0;
EyeDrawSnapshot g_drawSnapshot;
EyeTonemapSnapshot g_tonemapSnapshot;
EyePanelSnapshot g_panelSnapshot;
EyeDepthCapture g_eyeDepthCapture;
// The ledger records the submitted draw. Delay the panel's copies until
// the native draw runs, after texture/constant substitutions have begun.
struct PanelCaptureArgs {
    ID3D11DeviceContext* ctx = nullptr;
    uint32_t frame = 0, ordinal = 0, count = 0, instances = 0;
    uint32_t startInstance = 0, start = 0;
    int32_t base = 0;
    char kind = 0;
} g_panelCaptureArgs;
uint32_t g_panelSkipped = 0;
EyeDrawSnapshot g_eyeMeshSnapshot;
GuiDrawSnapshot g_guiSnapshot;

// The cull gate probe (advanced.cull_gate_capture) rides the eye run like
// the depth capture: armed with the ledger, it observes the engine's
// per-view gate and the draw-item builder for the run's first three
// complete frames (cull_gate_probe.h), and the eye-mesh snapshot keeps the
// geometry of the first one's pool draws (EDVRDRW1 version 9). State for
// this run's log lines; the probe itself is a process-lifetime global.
bool        g_gateRun = false;          // armed with this eye run (key on)
bool        g_gateHooked = false;       // the relays accepted the probe
const char* g_gateHookStatus = "off";
bool        g_gateBuilderHooked = false;
bool        g_gateFrustumOk = false;
uint8_t     g_gateVisGlobal = 0;
const char* g_gatePartStatus = "off";    // FUN_1442B3FC0's hook: "hooked" or why it stood down
uint32_t    g_gateGeoFrame = 0;         // the frame armGateProbe armed the pool-draw geometry for (0: none)
static_assert(CullGateProbe::kNoRow == kGateProbeNoRow,
              "the builder bracket's thread-local row and the probe's 'no row' must agree");

// FUN_1404F4E10's first sixteen bytes in the hash-verified executable
// (analysis EliteDangerous64.exe at RVA 0x4F4E10): movups xmm3,[r8];
// xor r9d,r9d; movups xmm2,[rdx]; movaps xmm0,xmm3; movzx r8d,word[rcx+44].
constexpr uint8_t kFrustumPrologue[16] = {0x41,0x0F,0x10,0x18,0x45,0x33,0xC9,0x0F,
                                          0x10,0x12,0x0F,0x28,0xC3,0x44,0x0F,0xB7};

bool gateReadImage(void* dst, uintptr_t src, size_t n) {
    __try { std::memcpy(dst, reinterpret_cast<const void*>(src), n); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

void closeGateProbe() {
    kinematicEvalSetGateProbeObservers(nullptr, nullptr, nullptr);
    if (g_gateHooked) kinematicEvalGateProbeDetach();
    cullGateProbe.disarm();
}

void armGateProbe() {
    g_gateRun = true;
    g_gateHooked = false;
    g_gatePartStatus = "off";
    g_gateGeoFrame = 0;
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    uint8_t prologue[sizeof(kFrustumPrologue)] = {};
    g_gateFrustumOk = base && gateReadImage(prologue, base + CullGateProbe::kFrustumRva, sizeof(prologue)) &&
                      std::memcmp(prologue, kFrustumPrologue, sizeof(prologue)) == 0;
    g_gateVisGlobal = 0;
    if (base) gateReadImage(&g_gateVisGlobal, base + CullGateProbe::kVisGlobalRva, 1);
    const uint32_t first = g_ledgerFrame0 + 1;   // the run's first complete frame (its first is partial)
    const auto frustum = g_gateFrustumOk
        ? reinterpret_cast<CullGateProbe::FrustumFn>(base + CullGateProbe::kFrustumRva) : nullptr;
    if (!cullGateProbe.arm(first, frustum, g_gateVisGlobal)) {
        g_gateHookStatus = "no_memory";
        Log::get().note("cull gate probe: could not allocate its window buffers for eye run %ls; "
                        "nothing will be captured (advanced.cull_gate_capture).", g_ledgerStamp);
        return;
    }
    kinematicEvalSetGateProbeObservers(&cullGateProbeGateObserver, &cullGateProbeBuilderObserver,
                                       &cullGateProbePartObserver);
    g_gateHookStatus = kinematicEvalGateProbeAttach();
    g_gateHooked = std::strcmp(g_gateHookStatus, "installed") == 0;
    g_gateBuilderHooked = g_gateHooked && kinematicEvalBuilderHooked();
    // The per-part test's own patch is installed by the attach, after its
    // build-keyed signature; it stands down alone ("hooked" or the reason).
    g_gatePartStatus = g_gateHooked ? kinematicEvalPartTestStatus() : "not attempted (engine hooks refused)";
    cullGateProbe.setPartHooked(g_gateHooked && std::strcmp(g_gatePartStatus, "hooked") == 0);
    if (!g_gateHooked) {
        kinematicEvalSetGateProbeObservers(nullptr, nullptr, nullptr);
        cullGateProbe.disarm();
    }
    g_eyeMeshSnapshot.clearGeometryFate();
    g_eyeMeshSnapshot.armGeometry(first);
    g_gateGeoFrame = first;
    Log::get().note("cull gate probe: armed with eye run %ls for ledger frames %u..%u: engine hooks %s "
                    "(the traversal's per-view gate FUN_14430EFE0 through the evaluator relay; the "
                    "draw-item builder FUN_1442B4420 %s), the builder's plane test FUN_1404F4E10 %s, "
                    "the builder's per-part test FUN_1442B3FC0 %s, visibility global DAT_145ea3399 = %u; "
                    "pool-draw geometry for frame %u in the eye mesh snapshot. No reject, nothing on screen "
                    "(advanced.cull_gate_capture).",
                    g_ledgerStamp, first, cullGateProbe.lastFrame(), g_gateHookStatus,
                    g_gateBuilderHooked ? "hooked" : "NOT hooked (no builder verdicts)",
                    g_gateFrustumOk ? "matched" : "MISMATCHED (builder frustum verdicts absent)",
                    cullGateProbe.partHooked() ? "hooked (prologue, builder frame and call site matched)"
                                               : g_gatePartStatus,
                    static_cast<unsigned>(g_gateVisGlobal), first);
    if (g_gateHooked && !cullGateProbe.partHooked())
        Log::get().note("cull gate probe: the per-part test FUN_1442B3FC0 STOOD DOWN (%s): no part rows will be "
                        "captured for eye run %ls; the gate and builder rows are unaffected "
                        "(advanced.cull_gate_capture).", g_gatePartStatus, g_ledgerStamp);
}
struct AuxFrame {   // one watched shader's buffers in one frame
    uint64_t vs;
    uint32_t instances, count;
    uint32_t stride[kAuxWhat];
    std::vector<uint8_t> bytes[kAuxWhat];
};
std::vector<AuxFrame> g_ledgerAux[kLedgerFrames];
void releaseCopy(LedgerCopy& c) {
    if (c.staging) c.staging->Release();
    c.staging = nullptr;
    c.bytes = 0;
    c.inUse = false;
}
// The gate probe's geometry arm at a mid-run pool release, said when it
// happens; the run's line at the ledger write repeats the outcome.
void noteGeometryRelease(EyeDrawSnapshot::GeoRelease r) {
    const auto& geo = g_eyeMeshSnapshot;
    if (r == EyeDrawSnapshot::kGeoRearmed)
        Log::get().note("eye mesh snapshot: the pool was re-seen at ledger frame %u after the cull gate probe armed the "
                        "pool-draw geometry for frame %u; the release cleared the arm before any draw of that frame "
                        "was mapped, so it is re-armed (advanced.cull_gate_capture).",
                        g_frame + 1, geo.geoFrame);
    else if (r == EyeDrawSnapshot::kGeoLost)
        Log::get().note("eye mesh snapshot: pool-draw geometry for frame %u disarmed by pool release at ledger frame %u "
                        "(the frame is %s; %u of its draws already mapped are gone): the version 9 section will be "
                        "empty (advanced.cull_gate_capture).",
                        geo.geoLostFrame, geo.geoLostAt, geo.geoLostAt > geo.geoLostFrame ? "over" : "in progress",
                        geo.geoLostDraws);
}

void ledgerRelease() {
    g_drawSnapshot.reset();
    g_tonemapSnapshot.reset();
    g_panelSnapshot.reset();
    g_eyeDepthCapture.reset();
    g_panelCaptureArgs = {};
    g_panelSkipped = 0;
    // A pool re-seen mid-run (objectProbeOnEyeDraw) releases every snapshot
    // learned on the old one. The gate probe's one-shot geometry arm comes
    // back when the release cost it nothing, and the log says which (design
    // doc §10: run 152632's arm was wiped here, 60 ms after it was made).
    // Only while an armed ledger runs: the arm path and the ledger write
    // reach here with the ledger off and reset outright.
    if (g_gateRun && detail::g_objectProbeLedgerOn)
        noteGeometryRelease(g_eyeMeshSnapshot.resetKeepingGeometry(g_frame + 1));
    else
        g_eyeMeshSnapshot.reset();
    g_guiSnapshot.reset();
    if (g_inst) g_inst->Release();
    g_inst = nullptr;
    g_instBytes = 0;
    g_instStride = 0;
    for (int i = 0; i < kLedgerPalettes; ++i) {
        if (g_palette[i]) g_palette[i]->Release();
        g_palette[i] = nullptr;
        g_paletteBytes[i] = 0;
    }
    g_paletteCount = 0;
    for (bool& seen : g_paletteSeen) seen = false;
    for (auto& ring : g_ledgerRing) {
        for (LedgerCopy& c : ring) releaseCopy(c);
    }
    for (AuxSlot& a : g_aux) {
        a.vs = 0;
        a.instances = a.count = 0;
        a.seenThisFrame = false;
        for (auto& ring : a.ring) {
            for (LedgerCopy& c : ring) releaseCopy(c);
        }
    }
    g_auxCount = 0;
}

FaultBudget g_budget("objectProbe", 5);

void releaseRing() {
    for (Slot& s : g_ring) {
        if (s.staging) s.staging->Release();
        s = Slot();
    }
}

void releasePool() {
    if (g_pool) g_pool->Release();
    g_pool = nullptr;
    g_poolBytes = 0;
    g_records = 0;
    ledgerRelease();   // the instance stream and the palette went with the pool they were learned on
}

// One frame of the pool to disk: a 32-byte header, the scene block when one
// is given (the ledger gives none), the pool. tools/eye_run_ledger.py reads
// it. stamp names the file by the eye run it belongs to (the ledger)
// instead of the clock.
bool writeDump(const uint8_t* pool, uint32_t poolBytes, const uint8_t* scene, uint32_t sceneBytes,
               uint32_t frame, wchar_t* path, size_t pathN, const wchar_t* stamp = nullptr) {
    const std::wstring dir = Log::get().dir() + L"\\pool";
    if (!g_dumpDirMade) {
        g_dumpDirMade = true;
        CreateDirectoryW(dir.c_str(), nullptr);
    }
    if (stamp) {
        _snwprintf_s(path, pathN, _TRUNCATE, L"%s\\pool_%s_%u.bin", dir.c_str(), stamp, frame);
    } else {
        SYSTEMTIME st{};
        GetLocalTime(&st);
        _snwprintf_s(path, pathN, _TRUNCATE, L"%s\\pool_%02u%02u%02u_%u.bin", dir.c_str(),
                     static_cast<unsigned>(st.wHour), static_cast<unsigned>(st.wMinute),
                     static_cast<unsigned>(st.wSecond), frame);
    }
    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        path[0] = 0;
        return false;
    }
    struct Header {
        char     magic[8];
        uint32_t version;
        uint32_t frame;
        uint32_t poolBytes;
        uint32_t sceneBytes;
        uint32_t recordBytes;
        uint32_t records;
    };
    static_assert(sizeof(Header) == 32, "the reader assumes a 32-byte header");
    Header hd = {{'E', 'D', 'V', 'R', 'P', 'O', 'O', 'L'}, 1u, frame, poolBytes, sceneBytes,
                 kRecordBytes, poolBytes / kRecordBytes};
    DWORD w = 0;
    bool ok = WriteFile(h, &hd, sizeof(hd), &w, nullptr) != 0;
    if (ok && sceneBytes) ok = WriteFile(h, scene, sceneBytes, &w, nullptr) != 0;
    if (ok) ok = WriteFile(h, pool, poolBytes, &w, nullptr) != 0;
    CloseHandle(h);
    return ok;
}

bool makeStaging(ID3D11Device* dev, uint32_t bytes, ID3D11Buffer** out) {
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = bytes;
    bd.Usage = D3D11_USAGE_STAGING;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    return SUCCEEDED(dev->CreateBuffer(&bd, nullptr, out)) && *out;
}

bool ensureSlot(ID3D11DeviceContext* ctx, Slot& s) {
    if (s.staging && s.bytes == g_poolBytes) return true;
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    if (!dev) return false;
    if (s.staging) { s.staging->Release(); s.staging = nullptr; }
    const bool ok = makeStaging(dev, g_poolBytes, &s.staging);
    s.bytes = ok ? g_poolBytes : 0;
    dev->Release();
    return ok;
}

void issueCopy(ID3D11DeviceContext* ctx) {
    for (Slot& s : g_ring) {
        if (s.inUse) continue;
        if (!ensureSlot(ctx, s)) { ++g_ledgerSkipped; return; }
        ctx->CopyResource(s.staging, g_pool);
        s.frame = g_frame;
        s.inUse = true;
        return;
    }
    ++g_ledgerSkipped;
}

// THE LEDGER's two buffers, learned on a pool draw once armed: the vertex
// buffer of stride 8 among the first four slots (the record and model-data
// indices, 8 bytes an instance -- the census of 2026-09-09 16:58 showed one
// 128 KB buffer shared by every pool draw, each draw's StartInstanceLocation
// its window into it), and the 48-byte structured buffer at t38, every
// pool-reading shader's palette (the dump of 2026-09-09: 62 of 62 skin by it).
void ledgerLearn(ID3D11DeviceContext* ctx) {
    if (!g_inst) {
        ID3D11Buffer* vbs[4] = {};
        UINT strides[4] = {}, offsets[4] = {};
        ctx->IAGetVertexBuffers(0, 4, vbs, strides, offsets);
        int pick = -1;
        for (int i = 0; i < 4; ++i) {
            if (vbs[i] && strides[i] == 8 && pick < 0) pick = i;
        }
        for (int i = 0; i < 4 && pick < 0; ++i) {
            if (vbs[i]) pick = i;
        }
        for (int i = 0; i < 4; ++i) {
            if (!vbs[i]) continue;
            if (i == pick) {
                D3D11_BUFFER_DESC bd{};
                vbs[i]->GetDesc(&bd);
                g_inst = vbs[i];   // the Get's reference is the one held
                g_instBytes = bd.ByteWidth;
                g_instStride = strides[i];
            } else {
                vbs[i]->Release();
            }
        }
    }
    if (g_paletteCount < kLedgerPalettes) {
        ID3D11ShaderResourceView* srv = nullptr;
        ctx->VSGetShaderResources(38, 1, &srv);
        if (srv) {
            ResourceInfo info;
            if (bindingResolve(srv, &info) && info.isBuffer && info.b == 48) {
                ID3D11Resource* res = nullptr;
                srv->GetResource(&res);
                if (res) {
                    ID3D11Buffer* buf = nullptr;
                    res->QueryInterface(__uuidof(ID3D11Buffer), reinterpret_cast<void**>(&buf));
                    res->Release();
                    if (buf) {
                        bool known = false;
                        for (int i = 0; i < g_paletteCount; ++i) known = known || g_palette[i] == buf;
                        if (known) {
                            buf->Release();
                        } else {
                            g_palette[g_paletteCount] = buf;   // the QueryInterface reference is the one held
                            g_paletteBytes[g_paletteCount] = info.a;
                            ++g_paletteCount;
                        }
                    }
                }
            }
            srv->Release();
        }
    }
}

// A watched shader's buffers, copied at its first draw of the frame: cb2
// whole, t0 when it is a buffer, the first two vertex buffers -- each up to
// kLedgerAuxBytes, into the slot's rings, read back at the boundary.
bool auxStage(ID3D11DeviceContext* ctx, ID3D11Device*& dev, LedgerCopy* ring, ID3D11Buffer* src, uint32_t whole,
              uint32_t cap = kLedgerAuxBytes) {
    const uint32_t bytes = whole < cap ? whole : cap;
    if (!src || !bytes) return false;
    LedgerCopy* c = nullptr;
    for (int i = 0; i < kLedgerRing; ++i) {
        if (!ring[i].inUse) { c = &ring[i]; break; }
    }
    if (!c) return false;
    if (c->staging && c->bytes != bytes) releaseCopy(*c);
    if (!c->staging) {
        if (!dev) ctx->GetDevice(&dev);
        if (!dev || !makeStaging(dev, bytes, &c->staging)) return false;
        c->bytes = bytes;
    }
    if (bytes == whole) {
        ctx->CopyResource(c->staging, src);
    } else {
        D3D11_BOX box{0, 0, 0, bytes, 1, 1};
        ctx->CopySubresourceRegion(c->staging, 0, 0, 0, 0, src, 0, &box);
    }
    c->frame = g_frame + 1;   // this frame's, as ledgerNoteDraw counts it
    c->inUse = true;
    return true;
}
void auxCapture(ID3D11DeviceContext* ctx, uint64_t vs, uint32_t count, uint32_t instances) {
    int slot = -1;
    for (int i = 0; i < g_auxCount; ++i) {
        if (g_aux[i].vs == vs) { slot = i; break; }
    }
    if (slot < 0) {
        if (g_auxCount >= kLedgerAux) return;
        slot = g_auxCount++;
        g_aux[slot].vs = vs;
    }
    AuxSlot& a = g_aux[slot];
    if (a.seenThisFrame) return;
    a.seenThisFrame = true;
    a.instances = instances;
    a.count = count;
    ID3D11Device* dev = nullptr;
    ID3D11Buffer* cb = nullptr;
    ctx->VSGetConstantBuffers(2, 1, &cb);
    if (cb) {
        D3D11_BUFFER_DESC bd{};
        cb->GetDesc(&bd);
        if (!auxStage(ctx, dev, a.ring[0], cb, bd.ByteWidth)) ++g_ledgerSkipped;
        cb->Release();
    }
    ID3D11ShaderResourceView* srv = nullptr;
    ctx->VSGetShaderResources(0, 1, &srv);
    if (srv) {
        ResourceInfo info;
        if (bindingResolve(srv, &info) && info.isBuffer) {
            ID3D11Resource* res = nullptr;
            srv->GetResource(&res);
            if (res) {
                ID3D11Buffer* buf = nullptr;
                res->QueryInterface(__uuidof(ID3D11Buffer), reinterpret_cast<void**>(&buf));
                res->Release();
                if (buf) {
                    a.stride[1] = info.b;
                    if (!auxStage(ctx, dev, a.ring[1], buf, info.a)) ++g_ledgerSkipped;
                    buf->Release();
                }
            }
        }
        srv->Release();
    }
    ID3D11Buffer* vbs[2] = {};
    UINT strides[2] = {}, offsets[2] = {};
    ctx->IAGetVertexBuffers(0, 2, vbs, strides, offsets);
    for (int i = 0; i < 2; ++i) {
        if (!vbs[i]) continue;
        D3D11_BUFFER_DESC bd{};
        vbs[i]->GetDesc(&bd);
        a.stride[2 + i] = strides[i];
        // Orbital lines have six instances, with their transforms in VB1
        // rather than b0/b2. Keep the full 8194-vertex stroke on explicit
        // eye runs; log offsets so the copied bindings can be reconstructed.
        const uint32_t cap=vs==kOrbitalLineVs ? 256*1024 : kLedgerAuxBytes;
        if (!auxStage(ctx, dev, a.ring[2 + i], vbs[i], bd.ByteWidth,cap)) ++g_ledgerSkipped;
        if(vs==kOrbitalLineVs) Log::get().note("eye orbital inputs: frame %u vb%d offset %u stride %u bytes %u (copy cap %u).",g_frame+1,i,offsets[i],strides[i],bd.ByteWidth,cap);
        vbs[i]->Release();
    }
    if (dev) dev->Release();
}

// A palette's copy at this frame's first pool draw binding it, the megabyte
// the bases reach into (kLedgerBonesMax): at the boundary the copies read
// zeros, the game having discarded and rewritten them for the next frame
// before present (the run of 05:37). Three COM calls a pool draw until every
// palette in hand has been seen this frame.
void paletteCapture(ID3D11DeviceContext* ctx) {
    bool all = g_paletteCount > 0;
    for (int i = 0; i < g_paletteCount; ++i) all = all && g_paletteSeen[i];
    if (all || g_paletteCount == 0) return;
    ID3D11ShaderResourceView* srv = nullptr;
    ctx->VSGetShaderResources(38, 1, &srv);
    if (!srv) return;
    ID3D11Resource* res = nullptr;
    srv->GetResource(&res);
    srv->Release();
    if (!res) return;
    ID3D11Buffer* buf = nullptr;
    res->QueryInterface(__uuidof(ID3D11Buffer), reinterpret_cast<void**>(&buf));
    res->Release();
    if (!buf) return;
    for (int i = 0; i < g_paletteCount; ++i) {
        if (g_palette[i] != buf || g_paletteSeen[i]) continue;
        g_paletteSeen[i] = true;
        ID3D11Device* dev = nullptr;
        if (!auxStage(ctx, dev, g_ledgerRing[1 + i], buf, g_paletteBytes[i], kLedgerBonesMax)) ++g_ledgerSkipped;
        if (dev) dev->Release();
    }
    buf->Release();
}

// One eye draw's row while armed. The pool question is asked of the context
// -- three COM calls on each instanced draw for twenty frames, a millisecond
// or two a frame, and only then.
void ledgerNoteDraw(ID3D11DeviceContext* ctx, char kind, uint32_t count, uint32_t instances,
                    uint32_t startInstance,uint32_t start,int32_t base) {
    const uint32_t frame = g_frame + 1;   // this frame's draws precede its boundary, where g_frame steps
    if (frame < g_ledgerFrame0 || frame > g_ledgerLastFrame) return;
    LedgerDraw d{};
    d.vs = bindingGet(BindSlot::Vs) ? bindingShaderHash(BindSlot::Vs) : 0;
    d.ps = bindingGet(BindSlot::Ps) ? bindingShaderHash(BindSlot::Ps) : 0;
    d.count = count;
    d.instances = instances;
    d.startInstance = startInstance;
    d.kind = static_cast<uint8_t>(kind);
    // The target the draw lands in, as the census would name it: one
    // OMGetRenderTargets and one intern per draw, armed intervals only. The
    // desk's duplication study needs per-row pass identity -- same-pass
    // repeats cull, per-eye/per-pass repeats do not -- and the RTV's census
    // intern id is both stable across the run and joinable to census lines.
    ID3D11RenderTargetView* rtv = nullptr;
    ID3D11DepthStencilView* dsv = nullptr;
    ctx->OMGetRenderTargets(1, &rtv, g_eyeDepthCapture.enabled() ? &dsv : nullptr);
    d.rt = drawCensusIntern(rtv);
    if (rtv) rtv->Release();
    if (g_panelCaptureArgs.ctx) {
        ++g_panelSkipped;
        g_panelCaptureArgs = {};
    }
    if (d.vs == EyePanelSnapshot::kVs && bindingShaderHash(BindSlot::Ps) == EyePanelSnapshot::kPs) {
        g_panelCaptureArgs = {ctx, frame,
            static_cast<uint32_t>(g_ledgerDraws[frame-g_ledgerFrame0].size()),
            count, instances, startInstance, start, base, kind};
    }
    if(d.vs==EyeTonemapSnapshot::kVs)
        g_tonemapSnapshot.captureRequested(ctx,g_ledgerFrame0,g_ledgerLastFrame,frame,
                                 static_cast<uint32_t>(g_ledgerDraws[frame-g_ledgerFrame0].size()),
                                 d.vs,bindingShaderHash(BindSlot::Ps),kind,count,static_cast<uint32_t>(base),instances,startInstance);
    if (EyeDrawSnapshot::watches(d.vs)) {
        g_drawSnapshot.capture(ctx, frame, static_cast<uint32_t>(g_ledgerDraws[frame - g_ledgerFrame0].size()),
                               d.vs, bindingShaderHash(BindSlot::Ps), kind, count, instances, startInstance,start,base);
    }
    g_eyeMeshSnapshot.captureEyeMesh(ctx,frame,static_cast<uint32_t>(g_ledgerDraws[frame-g_ledgerFrame0].size()),
                                   d.vs,bindingShaderHash(BindSlot::Ps),kind,count,instances,startInstance,start,base);
    if ((kind == 'X' || kind == 'N') && instances && g_pool) {
        guardedBudget(g_budget, [&] {
            // Every big instanced draw first, whatever t33 holds: the pool's own
            // draws carry a handful of instances each, and the draws this is for
            // bind nothing at t33 -- the run of 05:37 asked the pool question
            // first and left on that before reaching here.
            if (instances >= kLedgerAuxMin || d.vs==kOrbitalLineVs) auxCapture(ctx, d.vs, count, instances);
            ID3D11ShaderResourceView* srv = nullptr;
            ctx->VSGetShaderResources(kPoolSlot, 1, &srv);
            if (!srv) return;
            ID3D11Resource* res = nullptr;
            srv->GetResource(&res);
            if (res) {
                ID3D11Buffer* buf = nullptr;
                res->QueryInterface(__uuidof(ID3D11Buffer), reinterpret_cast<void**>(&buf));
                res->Release();
                if (buf) {
                    d.pool = buf == g_pool ? 1 : 0;
                    buf->Release();
                }
            }
            srv->Release();
            if (d.pool) {
                if (!g_inst || g_paletteCount < kLedgerPalettes) ledgerLearn(ctx);
                paletteCapture(ctx);
            }
        });
    }
    // The eye run's depth capture (advanced.eye_depth_capture) keys the pass
    // off the same single OMGetRenderTargets above; its poolDraw verdict is
    // the ledger's own t33 test from the block just run. Eye identity is the
    // depth probe's scene pair -- depthProbeSceneEyeOf(dsv) -- NOT the
    // frame's first-seen DSVs: the offscreen stages run before the eye
    // passes and reach this ledger with their own depth targets, and the
    // first flight interned those into both slots (190 declines, no files).
    if (dsv) {
        int eye = -1;
        depthProbeSceneEyeOf(dsv, &eye, nullptr);
        g_eyeDepthCapture.noteEyeDraw(ctx, frame, dsv, d.pool != 0, eye);
        dsv->Release();
    }
    // The cull gate probe's occluder inventory (EDVRDRW1 version 9): every
    // pool draw of its armed frame, both eyes, keyed by this row's ordinal;
    // one copy per distinct mesh. Returns at once unless the probe armed it.
    if (d.pool && instances)
        g_eyeMeshSnapshot.captureGeometry(ctx, frame, static_cast<uint32_t>(g_ledgerDraws[frame - g_ledgerFrame0].size()),
                                          d.vs, d.ps, kind, count, instances, startInstance, start, base);
    g_ledgerDraws[frame - g_ledgerFrame0].push_back(d);
}

// The boundary's copies of the two buffers (the pool's own copy is issued
// beside them), into their rings; the palette's first megabyte only.
void ledgerIssue(ID3D11DeviceContext* ctx) {
    ID3D11Device* dev = nullptr;
    for (int what = 0; what < 1; ++what) {   // the instance stream; the palettes are copied at their draws (paletteCapture)
        ID3D11Buffer* src = g_inst;
        const uint32_t whole = g_instBytes;
        const uint32_t bytes = whole;
        if (!src || !bytes) continue;
        LedgerCopy* c = nullptr;
        for (LedgerCopy& s : g_ledgerRing[what]) {
            if (!s.inUse) { c = &s; break; }
        }
        if (!c) { ++g_ledgerSkipped; continue; }
        if (c->staging && c->bytes != bytes) {
            c->staging->Release();
            c->staging = nullptr;
            c->bytes = 0;
        }
        if (!c->staging) {
            if (!dev) ctx->GetDevice(&dev);
            if (!dev || !makeStaging(dev, bytes, &c->staging)) { ++g_ledgerSkipped; continue; }
            c->bytes = bytes;
        }
        if (bytes == whole) {
            ctx->CopyResource(c->staging, src);
        } else {
            D3D11_BOX box{0, 0, 0, bytes, 1, 1};
            ctx->CopySubresourceRegion(c->staging, 0, 0, 0, 0, src, 0, &box);
        }
        c->frame = g_frame;
        c->inUse = true;
    }
    if (dev) dev->Release();
}

// The late readbacks, kept by frame; a copy still in flight past kDropAfter
// is given up like the pool's.
// One copy's readback: 1 read, 0 not ready yet, -1 given up.
int ledgerRead(ID3D11DeviceContext* ctx, LedgerCopy& c, std::vector<uint8_t>* into) {
    if (!c.inUse || g_frame - c.frame < kReadAfter) return 0;
    D3D11_MAPPED_SUBRESOURCE m{};
    const HRESULT hr = ctx->Map(c.staging, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
    if (hr == DXGI_ERROR_WAS_STILL_DRAWING) {
        if (g_frame - c.frame > kDropAfter) { c.inUse = false; ++g_ledgerSkipped; return -1; }
        return 0;
    }
    if (FAILED(hr) || !m.pData) { c.inUse = false; ++g_ledgerSkipped; return -1; }
    if (into) {
        const uint8_t* b = static_cast<const uint8_t*>(m.pData);
        into->assign(b, b + c.bytes);
    }
    ctx->Unmap(c.staging, 0);
    c.inUse = false;
    return 1;
}
void ledgerPoll(ID3D11DeviceContext* ctx) {
    for (int what = 0; what < 1 + kLedgerPalettes; ++what) {
        for (LedgerCopy& c : g_ledgerRing[what]) {
            const bool kept = c.inUse && c.frame >= g_ledgerFrame0 && c.frame <= g_ledgerLastFrame;
            std::vector<uint8_t>* into =
                !kept ? nullptr
                      : (what == 0 ? &g_ledgerInst[c.frame - g_ledgerFrame0]
                                   : &g_ledgerPalette[what - 1][c.frame - g_ledgerFrame0]);
            ledgerRead(ctx, c, into);
        }
    }
    for (int s = 0; s < g_auxCount; ++s) {
        AuxSlot& a = g_aux[s];
        for (int what = 0; what < kAuxWhat; ++what) {
            for (LedgerCopy& c : a.ring[what]) {
                if (!c.inUse) continue;
                const bool kept = c.frame >= g_ledgerFrame0 && c.frame <= g_ledgerLastFrame;
                std::vector<uint8_t> got;
                const int r = ledgerRead(ctx, c, kept ? &got : nullptr);
                if (r != 1 || !kept) continue;
                std::vector<AuxFrame>& fr = g_ledgerAux[c.frame - g_ledgerFrame0];
                AuxFrame* e = nullptr;
                for (AuxFrame& f : fr) {
                    if (f.vs == a.vs) { e = &f; break; }
                }
                if (!e) {
                    fr.push_back(AuxFrame{a.vs, a.instances, a.count, {a.stride[0], a.stride[1], a.stride[2], a.stride[3]}, {}});
                    e = &fr.back();
                }
                e->bytes[what].swap(got);
            }
        }
    }
}

bool writeRaw(const wchar_t* dir, const wchar_t* prefix, uint32_t frame, const std::vector<uint8_t>& bytes,
              wchar_t* path, size_t pathN) {
    _snwprintf_s(path, pathN, _TRUNCATE, L"%s\\%s_%s_%u.bin", dir, prefix, g_ledgerStamp, frame);
    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD w = 0;
    const bool ok = WriteFile(h, bytes.data(), static_cast<DWORD>(bytes.size()), &w, nullptr) != 0;
    CloseHandle(h);
    return ok;
}

// Everything to disk once the last frame's readbacks have had their chance:
// the pool copies as pool_<stamp>_<frame>.bin (the pair dumps' format), the
// two buffers raw, the draws as one file -- a header, then per frame the
// frame, a count and the rows. One long frame, after the run's own.
void writeLedger(ID3D11DeviceContext* ctx) {
    const std::wstring dir = Log::get().dir() + L"\\pool";
    if (!g_dumpDirMade) {
        g_dumpDirMade = true;
        CreateDirectoryW(dir.c_str(), nullptr);
    }
    int pools = 0, insts = 0, bones = 0, auxes = 0;
    wchar_t path[MAX_PATH];
    for (int i = 0; i < kLedgerFrames; ++i) {
        const uint32_t frame = g_ledgerFrame0 + static_cast<uint32_t>(i);
        if (!g_ledgerPool[i].empty() &&
            writeDump(g_ledgerPool[i].data(), static_cast<uint32_t>(g_ledgerPool[i].size()), nullptr, 0, frame,
                      path, MAX_PATH, g_ledgerStamp)) {
            ++pools;
        }
        if (!g_ledgerInst[i].empty() && writeRaw(dir.c_str(), L"inst", frame, g_ledgerInst[i], path, MAX_PATH)) ++insts;
        for (int p = 0; p < g_paletteCount; ++p) {
            wchar_t prefix[16];
            _snwprintf_s(prefix, 16, _TRUNCATE, L"bones%d", p);
            if (!g_ledgerPalette[p][i].empty() &&
                writeRaw(dir.c_str(), prefix, frame, g_ledgerPalette[p][i], path, MAX_PATH)) {
                ++bones;
            }
        }
        // The aux file: a header, then per watched shader its identity, the
        // four buffers' sizes and strides, and the bytes.
        if (!g_ledgerAux[i].empty()) {
            _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%s\\aux_%s_%u.bin", dir.c_str(), g_ledgerStamp, frame);
            HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h != INVALID_HANDLE_VALUE) {
                DWORD w = 0;
                struct AuxHeader { char magic[8]; uint32_t version, frame, entries, pad; };
                AuxHeader ah = {{'E', 'D', 'V', 'R', 'L', 'A', 'U', 'X'}, 1u, frame,
                                static_cast<uint32_t>(g_ledgerAux[i].size()), 0u};
                bool ok = WriteFile(h, &ah, sizeof(ah), &w, nullptr) != 0;
                for (const AuxFrame& f : g_ledgerAux[i]) {
                    struct Entry { uint64_t vs; uint32_t instances, count, bytes[kAuxWhat], stride[kAuxWhat]; };
                    Entry e = {f.vs, f.instances, f.count, {}, {}};
                    for (int k = 0; k < kAuxWhat; ++k) {
                        e.bytes[k] = static_cast<uint32_t>(f.bytes[k].size());
                        e.stride[k] = f.stride[k];
                    }
                    ok = ok && WriteFile(h, &e, sizeof(e), &w, nullptr) != 0;
                    for (int k = 0; ok && k < kAuxWhat; ++k) {
                        if (!f.bytes[k].empty()) {
                            ok = WriteFile(h, f.bytes[k].data(), static_cast<DWORD>(f.bytes[k].size()), &w, nullptr) != 0;
                        }
                    }
                }
                CloseHandle(h);
                if (ok) ++auxes;
            }
        }
    }
    struct Header {
        char     magic[8];
        uint32_t version;
        uint32_t frames;
        uint32_t frame0;
        uint32_t instStride;
        uint32_t bonesStride;
        uint32_t poolBytes;
        int32_t  crop[kLedgerCrops];   // crop k's frame, -1 = not taken
    };
    static_assert(sizeof(Header) == 160, "tools/eye_run_ledger.py reads a 160-byte header");
    Header hd = {{'E', 'D', 'V', 'R', 'L', 'D', 'G', 'R'}, 2u, static_cast<uint32_t>(kLedgerFrames), g_ledgerFrame0,
                 g_instStride, 48u, g_poolBytes, {}};   // the palettes are 48-byte rows; version 2 rows carry ps and rt
    memcpy(hd.crop, g_ledgerCropFrame, sizeof(hd.crop));
    _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%s\\draws_%s.bin", dir.c_str(), g_ledgerStamp);
    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    bool drawsOk = h != INVALID_HANDLE_VALUE;
    uint32_t rows = 0;
    if (drawsOk) {
        DWORD w = 0;
        drawsOk = WriteFile(h, &hd, sizeof(hd), &w, nullptr) != 0;
        for (int i = 0; drawsOk && i < kLedgerFrames; ++i) {
            const uint32_t n = static_cast<uint32_t>(g_ledgerDraws[i].size());
            const uint32_t frame = g_ledgerFrame0 + static_cast<uint32_t>(i);
            drawsOk = WriteFile(h, &frame, 4, &w, nullptr) != 0 && WriteFile(h, &n, 4, &w, nullptr) != 0;
            if (drawsOk && n) {
                drawsOk = WriteFile(h, g_ledgerDraws[i].data(), static_cast<DWORD>(n * sizeof(LedgerDraw)), &w,
                                    nullptr) != 0;
            }
            rows += n;
        }
        CloseHandle(h);
    }
    int first = -1, last = -1;
    for (int k = 0; k < kLedgerCrops; ++k) {
        if (g_ledgerCropFrame[k] < 0) continue;
        if (first < 0) first = k;
        last = k;
    }
    char pal[160] = "";
    for (int p = 0; p < g_paletteCount; ++p) {
        char one[40];
        _snprintf_s(one, sizeof(one), _TRUNCATE, "%s%u", p ? ", " : "", g_paletteBytes[p]);
        strncat_s(pal, one, _TRUNCATE);
    }
    char aux[400] = "";
    for (int s = 0; s < g_auxCount; ++s) {
        char one[60];
        _snprintf_s(one, sizeof(one), _TRUNCATE, "%s%016llX x%u", s ? ", " : "",
                    static_cast<unsigned long long>(g_aux[s].vs), g_aux[s].instances);
        strncat_s(aux, one, _TRUNCATE);
    }
    Log::get().note(
        "object probe: the eye run's LEDGER is on disk beside its crops -- %d frames from %u: %d pool copies "
        "(pool_%ls_<frame>.bin), %d instance streams (inst_%ls_<frame>.bin, %u bytes, stride %u), %d palette "
        "copies of %d palettes of [%s] bytes (bones<p>_%ls_<frame>.bin, the first %u bytes of each, copied at the "
        "draw), %d aux files "
        "(aux_%ls_<frame>.bin: cb2, t0 and the first two vertex buffers of the %d shaders drawing %u+ instances "
        "in a draw: [%s]) and %u eye draws in draws_%ls.bin%s; the crops C%02d..C%02d were frames %d..%d. "
        "tools/eye_run_ledger.py reads them. %u copies were skipped.",
        kLedgerFrames, g_ledgerFrame0, pools, g_ledgerStamp, insts, g_ledgerStamp, g_instBytes, g_instStride, bones,
        g_paletteCount, pal, g_ledgerStamp, kLedgerBonesMax, auxes, g_ledgerStamp, g_auxCount, kLedgerAuxMin, aux,
        rows, g_ledgerStamp, drawsOk ? "" : " (the draws file FAILED to write)", first < 0 ? 0 : first,
        last < 0 ? 0 : last, first < 0 ? -1 : g_ledgerCropFrame[first], last < 0 ? -1 : g_ledgerCropFrame[last],
        g_ledgerSkipped);
    for (int i = 0; i < kLedgerFrames; ++i) {
        std::vector<uint8_t>().swap(g_ledgerPool[i]);
        std::vector<uint8_t>().swap(g_ledgerInst[i]);
        for (int p = 0; p < kLedgerPalettes; ++p) std::vector<uint8_t>().swap(g_ledgerPalette[p][i]);
        std::vector<LedgerDraw>().swap(g_ledgerDraws[i]);
        std::vector<AuxFrame>().swap(g_ledgerAux[i]);
    }
    _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%s\\drawstate_%s.bin", dir.c_str(), g_ledgerStamp);
    const bool snapshotOk = g_drawSnapshot.write(ctx, path);
    wchar_t tonePath[MAX_PATH];
    _snwprintf_s(tonePath,MAX_PATH,_TRUNCATE,L"%s\\tonemap_%s.bin",dir.c_str(),g_ledgerStamp);
    const bool toneOk=g_tonemapSnapshot.write(ctx,tonePath,dir.c_str());
    wchar_t panelPath[MAX_PATH];
    _snwprintf_s(panelPath,MAX_PATH,_TRUNCATE,L"%s\\panels_%s.bin",dir.c_str(),g_ledgerStamp);
    const bool panelOk=g_panelSnapshot.write(ctx,panelPath);
    Log::get().note("object probe: panel snapshots %ls: %u draws, actual first matching frame %u, target %llX, %llu reserved bytes, %u declines, %u readback/capture failures, %u other-eye draws, %u skipped/replaced candidates; %s. First matching eye/frame only, 16 draws, 768 MiB payload cap; actual native draw inputs and before/after HDR/depth retained. No rendering changes.",
        panelPath,g_panelSnapshot.count(),g_panelSnapshot.firstFrame(),
        static_cast<unsigned long long>(g_panelSnapshot.target()),
        static_cast<unsigned long long>(g_panelSnapshot.bytes),g_panelSnapshot.declined,
        g_panelSnapshot.failures,g_panelSnapshot.ignoredOtherEye,g_panelSkipped,
        panelOk?"written":"WRITE FAILED");
    // The depth capture's readback rides the ledger's own grace period: the
    // copies were staged the moment each pass ended, so only the Map waits
    // on the GPU, with the snapshot's no-wait rule.
    const uint32_t depthFiles=g_eyeDepthCapture.write(ctx,dir.c_str(),g_ledgerStamp);
    Log::get().note("object probe: eye depth capture %ls: %u files (depth_%ls_f<frame>_<A|B>.bin), "
                    "%llu payload bytes staged, %u non-eye draws skipped (offscreen phases the depth "
                    "probe's scene pair does not name; eye identity comes from that pair, and this "
                    "instrument switches the probe on itself), declines %u format / %u bytes / %u "
                    "frame-cap, %u readback/write failures, %u SEH faults; %s. Each file is one eye "
                    "pass's completed depth as plain R32_FLOAT texels (the R32G8X24 family converted "
                    "through the depth probe's read table) plus that pass's VS b1 float4 registers "
                    "[256,336) from its first pool-carrying draw (file version 2: the view-projection's "
                    "columns at registers 270..273, the eye origin at 275; version 1 files hold registers "
                    "64..147 and no camera); eye A/B is the scene pair's first-bind order. Only with "
                    "advanced.eye_depth_capture on; the first %u frames of the run; no rendering changes.",
                    dir.c_str(),depthFiles,g_ledgerStamp,
                    static_cast<unsigned long long>(g_eyeDepthCapture.bytes()),g_eyeDepthCapture.nonEyeSkips,
                    g_eyeDepthCapture.declinedFormat,g_eyeDepthCapture.declinedBytes,
                    g_eyeDepthCapture.declinedFrameCap,
                    g_eyeDepthCapture.failures,g_eyeDepthCapture.faults,
                    depthFiles||g_eyeDepthCapture.declined()?"written":"nothing captured (instrument off, "
                    "or no scene-pair frames in the run)",
                    static_cast<unsigned>(edvr::EyeDepthCapture::kMaxFrames));
    // The cull gate probe's file, written only for a run it armed with. Its
    // window closed ~30 frames ago, so no worker is still appending.
    if (g_gateRun) {
        if (cullGateProbe.armed()) closeGateProbe();
        wchar_t gatePath[MAX_PATH];
        _snwprintf_s(gatePath, MAX_PATH, _TRUNCATE, L"%s\\gate_%s.bin", dir.c_str(), g_ledgerStamp);
        const bool gateOk = g_gateHooked && cullGateProbe.write(gatePath);
        const CullGateProbe::Counts gc = cullGateProbe.counts();
        const auto& geo = g_eyeMeshSnapshot;
        if (!g_gateHooked || (!gc.gateCalls && !gc.builderCalls)) {
            Log::get().note("cull gate probe: NOTHING captured for eye run %ls -- %s (advanced.cull_gate_capture). "
                            "Pool-draw geometry: %u draws, %u meshes.",
                            g_ledgerStamp,
                            !g_gateHooked ? "the engine hooks refused the probe (status above); no gate file"
                                          : "no gate or builder call landed in its ledger frames (the traversal "
                                            "never ran in the window, or the frame stamp never reached it); the "
                                            "gate file holds zero rows",
                            static_cast<unsigned>(geo.geoDraws.size()), static_cast<unsigned>(geo.geoMeshes.size()));
        } else {
            Log::get().note("cull gate probe: %ls: %u gate calls (%u kept, %u dropped over the cap), %u builder "
                            "calls on %u engine records (%u kept, %u dropped), %u view-array dumps (%u dropped), "
                            "%u entries / %u sub-items dropped, %u faults, %u record/pose mismatches; ledger frames "
                            "%u..%u; builder plane test %s; %s. Pool-draw geometry for frame %u: %u draws mapped, "
                            "%u distinct meshes, %u states, %u declined, %u draws over the cap (drawstate_%ls.eyemesh.bin, "
                            "version 9). tools/cull_gate_probe.py reads them with the run's depth, pool and ledger.",
                            gatePath, gc.gateCalls, gc.gateKept, gc.gateDropped, gc.builderCalls,
                            cullGateProbe.distinctRecords(), gc.builderKept, gc.builderDropped, gc.dumps,
                            gc.dumpsDropped, gc.entriesDropped, gc.subItemsDropped, gc.faults, gc.recordMismatch,
                            cullGateProbe.firstFrame(), cullGateProbe.lastFrame(),
                            g_gateFrustumOk ? "matched" : "MISMATCHED (no builder frustum verdicts)",
                            gateOk ? "written" : "WRITE FAILED",
                            cullGateProbe.firstFrame(), static_cast<unsigned>(geo.geoDraws.size()),
                            static_cast<unsigned>(geo.geoMeshes.size()), static_cast<unsigned>(geo.geoStates.size()),
                            geo.geoDeclined, geo.geoDrawsDropped, g_ledgerStamp);
        }
        // The per-part test (FUN_1442B3FC0): its rows, or why there are none.
        if (g_gateHooked) {
            if (!cullGateProbe.partHooked())
                Log::get().note("cull gate probe: per-part test FUN_1442B3FC0 STOOD DOWN (%s): the gate file's part "
                                "section is empty by construction for eye run %ls.", g_gatePartStatus, g_ledgerStamp);
            else
                Log::get().note("cull gate probe: per-part test FUN_1442B3FC0 for eye run %ls: %u calls in the window "
                                "(%u rows kept, %u dropped over the %u-row cap); of the kept rows %u with the builder's "
                                "frame unverified (verdict kept, no part identity), %u from a caller other than the "
                                "builder's sub-item loop, %u outside a kept builder call; %u distinct LOD tables "
                                "recorded (%u lost; version 3 rows, one per *(entry+8)); verdicts read after the "
                                "forward, never written.%s",
                                g_ledgerStamp, gc.partCalls, gc.partKept, gc.partDropped, CullGateProbe::kPartCap,
                                gc.partUnverified, gc.partForeign, gc.partUnlinked, gc.tablesKept, gc.tablesDropped,
                                gc.partCalls ? "" : " NOTHING captured: no part test ran inside the window.");
        }
        // Which happened to the geometry arm (design doc §10): each outcome
        // reads differently, whether or not the release path ever ran.
        if (g_gateGeoFrame && geo.geoFrame == g_gateGeoFrame)
            Log::get().note("eye mesh snapshot: geometry armed for %u draws of ledger frame %u (%u distinct meshes, "
                            "%u states, %u declined, %u over the cap)%s.",
                            static_cast<unsigned>(geo.geoDraws.size()), g_gateGeoFrame,
                            static_cast<unsigned>(geo.geoMeshes.size()), static_cast<unsigned>(geo.geoStates.size()),
                            geo.geoDeclined, geo.geoDrawsDropped,
                            geo.geoRearms ? "; re-armed after a pool release cleared it before any of its draws was "
                                            "mapped" : "");
        else if (g_gateGeoFrame && geo.geoLostFrame)
            Log::get().note("eye mesh snapshot: geometry for ledger frame %u disarmed by pool release at ledger frame %u "
                            "(%u mapped draws lost): the version 9 section is empty.",
                            geo.geoLostFrame, geo.geoLostAt, geo.geoLostDraws);
        else if (g_gateGeoFrame)
            Log::get().note("eye mesh snapshot: geometry for ledger frame %u disarmed by a reset outside a recorded pool "
                            "release (the arm reads frame %u): the version 9 section is empty.",
                            g_gateGeoFrame, geo.geoFrame);
        g_gateRun = false;
    }
    Log::get().note("object probe: tone-map snapshots %ls: %u draws, actual first matching frame %u, %u reserved bytes, %u declines, %u failed copies/shaders; %s. At most two eye draws; exposure, colour LUT, HDR input and converted output retained for target colour replay. No rendering changes.",tonePath,unsigned(g_tonemapSnapshot.count()),g_tonemapSnapshot.firstFrame(),g_tonemapSnapshot.bytes,g_tonemapSnapshot.declined,g_tonemapSnapshot.failures,toneOk?"written":"WRITE FAILED");
    const uint32_t missingShaders = g_drawSnapshot.writeShaders(dir.c_str());
    Log::get().note("object probe: eye draw snapshots %ls: %u draws, %u holo surfaces, %u capped draws, "
                    "%u failed copies, %u missing shader files; %s. VS b0/b1/b2 and PS b2 are captured at each watched draw; "
                    "surface alpha is from its first draw only.", path,
                    static_cast<uint32_t>(g_drawSnapshot.draws.size()),
                    static_cast<uint32_t>(g_drawSnapshot.surfaces.size()), g_drawSnapshot.dropped,
                    g_drawSnapshot.failures, missingShaders, snapshotOk ? "written" : "WRITE FAILED");
    Log::get().note("object probe: UI/mesh/effect vertex snapshots: %u draws, %u bytes, %u range/budget declines; UI/solar first three watched frames, meshes first source frame, effects throughout run; 256 KiB per stream, 32 MiB total. Draw offsets, bindings and capture ranges are retained.",g_drawSnapshot.vertexDraws,g_drawSnapshot.vertexBytes,g_drawSnapshot.vertexDeclined);
    uint32_t solarDraws=0,solarConstants=0,solarGeometry=0;
    for(const auto& d:g_drawSnapshot.draws)if(EyeDrawSnapshot::solarDraw(d.vs)) {
        ++solarDraws;solarConstants+=d.copied[0]>=128 && d.copied[1]>0;
        solarGeometry+=d.streams[0].copied>0 && !d.layout.empty();
    }
    Log::get().note("object probe: solar snapshots: %u draws, %u with draw-time b0/b1, %u with bounded geometry/layout; VS/PS retained for surface, corona and arcs. Constants throughout run; geometry first three watched frames. No solar rendering changes.",solarDraws,solarConstants,solarGeometry);
    _snwprintf_s(path,MAX_PATH,_TRUNCATE,L"%s\\drawstate_%s.eyemesh.bin",dir.c_str(),g_ledgerStamp);
    const bool eyeMeshOk=g_eyeMeshSnapshot.write(ctx,path);
    // Always emit the report, including a run whose probes saw nothing:
    // silence must not read as success.
    objectClassificationProbe.finish();
    const bool classificationOk=objectClassificationProbe.write(dir.c_str(),g_ledgerStamp);
    const auto writer=objectRecordWriterProbe.summary();
    Log::get().note("object classification: eye run %ls report %s (classification_%ls.json/.bin, schema v3: the record-writer and kinematic eval probes' sections).",
                    g_ledgerStamp,classificationOk?"written":"WRITE FAILED",g_ledgerStamp);
    Log::get().note("object classification: CPU record-writer probe hook %s; observed/stored/completed %llu/%u/%u, retained %llu bytes; record overflow/byte declines/read/context/unwind/completion failures %llu/%llu/%llu/%llu/%llu/%llu; declined management/unknown callers %llu/%llu. Exact 336-byte producer records and opaque caller context are evidence only; inactive/refused hooks and incomplete lookups remain explicit.",
                    objectRecordWriterProbe.hookStatusText(),
                    static_cast<unsigned long long>(writer.observed),
                    writer.stored,writer.completed,
                    static_cast<unsigned long long>(writer.retainedBytes),
                    static_cast<unsigned long long>(writer.recordOverflow),
                    static_cast<unsigned long long>(writer.byteBudgetDeclines),
                    static_cast<unsigned long long>(writer.readFaults),
                    static_cast<unsigned long long>(writer.contextFailures),
                    static_cast<unsigned long long>(writer.unwindFailures),
                    static_cast<unsigned long long>(writer.completionFailures),
                    static_cast<unsigned long long>(writer.declinedManagement),
                    static_cast<unsigned long long>(writer.declinedUnknown));
    const auto ownership=objectRecordWriterProbe.ownershipSummary();
    Log::get().note("object classification: KinematicRig ownership attempted/linked/stored/reused %llu/%llu/%llu/%llu; unsupported/ancestor missing/unwind failed %llu/%llu/%llu; tuple read/mismatch %llu/%llu, registry read/mismatch %llu/%llu, record range/read %llu/%llu. These are capture-local associations, not static-object classifications.",
                    static_cast<unsigned long long>(ownership.attempted),
                    static_cast<unsigned long long>(ownership.linked),
                    static_cast<unsigned long long>(ownership.stored),
                    static_cast<unsigned long long>(ownership.deduplicated),
                    static_cast<unsigned long long>(ownership.unsupportedWriter),
                    static_cast<unsigned long long>(ownership.ancestorMissing),
                    static_cast<unsigned long long>(ownership.unwindFailed),
                    static_cast<unsigned long long>(ownership.tupleReadFault),
                    static_cast<unsigned long long>(ownership.tupleMismatch),
                    static_cast<unsigned long long>(ownership.registryReadFault),
                    static_cast<unsigned long long>(ownership.registryMismatch),
                    static_cast<unsigned long long>(ownership.recordRangeMismatch),
                    static_cast<unsigned long long>(ownership.recordReadFault));
    Log::get().note("object classification: KinematicRig ownership opcode rejects/conflicts/cap/byte declines/read faults %llu/%llu/%llu/%llu/%llu; missing-ancestor traces stored/declined %llu/%llu. Zero linked records is unavailable ownership evidence, not a successful empty scene.",
                    static_cast<unsigned long long>(ownership.opcodeMismatch),
                    static_cast<unsigned long long>(ownership.cacheConflicts),
                    static_cast<unsigned long long>(ownership.recordOverflow),
                    static_cast<unsigned long long>(ownership.byteBudgetDeclines),
                    static_cast<unsigned long long>(ownership.readFaults),
                    static_cast<unsigned long long>(ownership.ancestorTraces),
                    static_cast<unsigned long long>(ownership.ancestorTraceOverflow));
    const uint32_t eyeMeshMissing=g_eyeMeshSnapshot.writeShaders(dir.c_str());
    Log::get().note("object probe: eye mesh snapshots %ls: %u draws, %u frame/target-local buffers, %u bytes, "
                    "%u buffer declines, %u capped draws, %u failed copies, %u missing shaders; %s. "
                    "First three matching eye frames; original draw ordinal, cameras, input layout, full t33/t38 and VB0 "
                    "at first use per frame/target. Geometry first frame, %u vertex bytes, %u vertex declines. "
                    "Separate 4096-draw/256-MiB pool/32-MiB vertex caps; no rendering changes.",
                    path,g_eyeMeshSnapshot.meshDraws,unsigned(g_eyeMeshSnapshot.meshBuffers.size()),g_eyeMeshSnapshot.meshBytes,
                    g_eyeMeshSnapshot.meshDeclined,g_eyeMeshSnapshot.dropped,g_eyeMeshSnapshot.failures,eyeMeshMissing,
                    eyeMeshOk?"written":"WRITE FAILED",g_eyeMeshSnapshot.vertexBytes,g_eyeMeshSnapshot.vertexDeclined);
    uint32_t sourceCameras=0,screenDraws=0,sourceDepths=0,effectDraws=0,effectVertices=0,effectLayouts=0;
    for(const auto& d:g_drawSnapshot.draws)if(d.ordinal==UINT32_MAX-2) {
        ++effectDraws;effectVertices+=d.streams[0].copied || d.streams[1].copied;effectLayouts+=!d.layout.empty();
    }
    for(const auto& d:g_drawSnapshot.draws) {sourceCameras+=d.ordinal==UINT32_MAX;screenDraws+=d.vs==EyeDrawSnapshot::kVscreen;}
    for(const auto& t:g_drawSnapshot.surfaces)sourceDepths+=t.format==20 || t.format==40 || t.format==45 || t.format==55;
    if(screenDraws)Log::get().note("object probe: on-foot source capture: %u camera frames, %u screen draws, %u completed depth surfaces; colour/depth copied at first composite, no temporal changes.",sourceCameras,screenDraws,sourceDepths);
    if(screenDraws)Log::get().note("object probe: source effect snapshots: %u draws, %u with vertex payloads, %u with original input layouts. Drawstate v7 ordinal UINT32_MAX-2; constants and bounded VB0/VB1/IB across the run; shared 32 MiB vertex budget, no normal-play copies or effect changes.",effectDraws,effectVertices,effectLayouts);
    if(screenDraws)Log::get().note("object probe: effect colour crops: %u images, %u bytes, %u format/range/budget declines; first source frame, before/after each light/beam/streak/particle draw. Native lower-right 1024-square crops, original HDR format and crop origin in drawstate v7; 64 MiB cap shared with night inputs.",unsigned(g_drawSnapshot.effectImages.size()),g_drawSnapshot.effectImageBytes,g_drawSnapshot.effectImageDeclined);
    uint32_t nightDraws=0,nightConstants=0,nightImages=0,nightInputs=0,nightGeometry=0,nightSamplers=0;
    for(const auto& d:g_drawSnapshot.draws)if(d.vs==EyeDrawSnapshot::kNight && d.ps==EyeDrawSnapshot::kNightPs) {
        ++nightDraws;nightConstants+=d.copied[1]>=333*16 && d.copied[3]>=12*16;
        nightGeometry+=d.streams[0].copied && d.streams[2].copied && !d.layout.empty();
    }
    for(const auto& e:g_drawSnapshot.effectImages)if(e.draw<g_drawSnapshot.draws.size() && g_drawSnapshot.draws[e.draw].vs==EyeDrawSnapshot::kNight) {
        if(e.after<=1)++nightImages;else ++nightInputs;
    }
    for(const auto& n:g_drawSnapshot.nightSampling)nightSamplers+=n.mask==3 && n.viewportCount==1;
    Log::get().note("object probe: night-vision snapshots: %u draws, %u with PS camera/settings, %u before/after images; drawstate slot 1 is PS b1 for FCF7BD2896751D96/F786D34B5E118D5E, slot 3 is PS b2. First-frame native terrain crops retain HDR and origin; effect image declines %u. No night-vision rendering changes.",nightDraws,nightConstants,nightImages,g_drawSnapshot.effectImageDeclined);
    Log::get().note("object probe: night-vision sampling: %u input images (PS t0..t4), %u draws with both samplers and one viewport, %u with geometry/layout. Drawstate v7 phases 2..6 retain typed native crops, BC4 blocks and draw-time contents for each eye; sampler masks expose absent states. Shared 64 MiB image and 32 MiB vertex caps; only while a dump is armed.",nightInputs,nightSamplers,nightGeometry);
    uint32_t spritePairs=0,spriteConstants=0,spriteLayouts=0,spriteDepth=0,spriteMeshes=0;
    for(const auto& d:g_drawSnapshot.spriteDiagnostics) {
        spritePairs+=d.before && d.after && !d.postPending ? 1u : 0u;
        spriteConstants+=d.psB1 && d.psB1Bytes>=91*16 ? 1u : 0u;
        spriteLayouts+=!d.layout.empty() ? 1u : 0u;
        spriteDepth+=d.depthImage ? 1u : 0u;
        spriteMeshes+=d.mesh[0]!=UINT32_MAX && d.mesh[1]!=UINT32_MAX ? 1u : 0u;
    }
    Log::get().note("object probe: target colour snapshots: %u exact sprite draws, %u completed before/after pairs, %u with PS b1, %u with input layout, %u with depth/stencil, %u with t33/t38; %u reserved colour bytes, %u depth bytes, %u declines. First requested frame only; native central crops and original draw state in drawstate v8. Pair counts describe GPU copies; the snapshot failure count above includes unavailable readbacks. No target rendering changes.",unsigned(g_drawSnapshot.spriteDiagnostics.size()),spritePairs,spriteConstants,spriteLayouts,spriteDepth,spriteMeshes,g_drawSnapshot.spriteImageBytes,g_drawSnapshot.spriteDepthBytes,g_drawSnapshot.spriteImageDeclined);
    if(screenDraws)Log::get().note("object probe: source mesh snapshots: %u draws, %u frame-local buffers, %u bytes, %u range/format/budget declines. Drawstate v4 records original draw cameras, full t33/t38 and VB0 at first use per resource per frame; firstDraw identifies that copy. No render or pacing changes.",g_drawSnapshot.meshDraws,unsigned(g_drawSnapshot.meshBuffers.size()),g_drawSnapshot.meshBytes,g_drawSnapshot.meshDeclined);
    _snwprintf_s(path,MAX_PATH,_TRUNCATE,L"%s\\gui_%s.bin",dir.c_str(),g_ledgerStamp);
    const bool guiOk=g_guiSnapshot.write(ctx,path,dir.c_str());
    Log::get().note("object probe: GUI source snapshot %ls: %u draws, %u range/budget/format declines, %u failed copies/shaders, %u missing layouts; %s. First matching source frame, square/wide GUI targets up to 2048, 96 MiB cap; original geometry, atlases, transforms and render state.",path,unsigned(g_guiSnapshot.count()),g_guiSnapshot.declined,g_guiSnapshot.failures,g_guiSnapshot.missingLayouts,guiOk?"written":"WRITE FAILED");
    detail::g_objectProbeLedgerOn = false;
    ledgerRelease();
    objectClassificationProbe.reset();
}

void poll(ID3D11DeviceContext* ctx) {
    // In frame order (matters only for which copy a full ring drops first).
    for (int pass = 0; pass < kRing; ++pass) {
        Slot* s = nullptr;
        for (Slot& c : g_ring) {
            if (!c.inUse) continue;
            if (g_frame - c.frame < kReadAfter) continue;
            if (!s || c.frame < s->frame) s = &c;
        }
        if (!s) return;
        D3D11_MAPPED_SUBRESOURCE m{};
        const HRESULT hr = ctx->Map(s->staging, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
        if (hr == DXGI_ERROR_WAS_STILL_DRAWING) {
            if (g_frame - s->frame > kDropAfter) { s->inUse = false; ++g_ledgerSkipped; continue; }
            return;   // the older copies are not ready either
        }
        if (FAILED(hr) || !m.pData) { s->inUse = false; ++g_ledgerSkipped; continue; }
        const uint8_t* bytes = static_cast<const uint8_t*>(m.pData);
        if (detail::g_objectProbeLedgerOn && s->frame >= g_ledgerFrame0 && s->frame <= g_ledgerLastFrame) {
            g_ledgerPool[s->frame - g_ledgerFrame0].assign(bytes, bytes + s->bytes);   // the ledger's frame
        }
        ctx->Unmap(s->staging, 0);
        s->inUse = false;
    }
}

}  // namespace

void objectProbeConfigure(Config& cfg) {
    // The pool's recognition runs with fix.temporal_aa (as it always has, so
    // an eye run's ledger finds the pool already known on its first frame)
    // or with advanced.object_probe, which also prints the absent-pool note.
    // The per-object motion estimate it once also fed retired 2026-09-23
    // (see the header comment); nothing is copied outside an armed run.
    g_verbose = cfg.getBool("advanced.object_probe", false);
    g_eyeDepthCapture.configure(cfg.getBool("advanced.eye_depth_capture", false));
    cullGateProbe.configure(cfg.getBool("advanced.cull_gate_capture", false));
    detail::g_objectProbeOn = g_verbose || temporalModeEnabled(cfg.getString("fix.temporal_aa", "off"));
}

void objectProbeNoteSourceDraw(ID3D11DeviceContext* ctx,char kind,uint32_t count,uint32_t instances,
                              uint32_t startInstance,uint32_t start,int32_t base) {
    if(!detail::g_objectProbeLedgerOn || !ctx || g_frame+1<g_ledgerFrame0 || g_frame+1>g_ledgerLastFrame)return;
    g_drawSnapshot.captureSource(ctx,g_frame+1,bindingShaderHash(BindSlot::Vs),bindingShaderHash(BindSlot::Ps),
                                 kind,count,instances,startInstance,start,base);
    g_drawSnapshot.captureSourceMesh(ctx,g_frame+1,bindingShaderHash(BindSlot::Vs),bindingShaderHash(BindSlot::Ps),
                                     kind,count,instances,startInstance,start,base);
}

void objectProbeSourceDrawEnd(ID3D11DeviceContext* ctx) {
    // An early-return verdict swallowed the original draw. It has no valid
    // native before/after pair; do not carry its arguments into another draw.
    if (g_panelCaptureArgs.ctx == ctx) {
        ++g_panelSkipped;
        g_panelCaptureArgs = {};
    }
    if(!detail::g_objectProbeLedgerOn || !ctx || g_frame+1<g_ledgerFrame0 || g_frame+1>g_ledgerLastFrame)return;
    g_drawSnapshot.captureEffectEnd(ctx);
    g_tonemapSnapshot.end(ctx);
}

void objectProbePanelDrawBegin(ID3D11DeviceContext* ctx) {
    if (!detail::g_objectProbeLedgerOn || !ctx || g_panelCaptureArgs.ctx != ctx) return;
    const PanelCaptureArgs args = g_panelCaptureArgs;
    g_panelCaptureArgs = {};
    if (args.frame != g_frame+1) {++g_panelSkipped;return;}
    g_panelSnapshot.captureRequested(ctx,g_ledgerFrame0,g_ledgerLastFrame,
        args.frame,args.ordinal,EyePanelSnapshot::kVs,EyePanelSnapshot::kPs,
        args.kind,args.count,args.instances,args.startInstance,args.start,args.base);
}

void objectProbePanelDrawEnd(ID3D11DeviceContext* ctx) {
    if (detail::g_objectProbeLedgerOn && ctx) g_panelSnapshot.end(ctx);
}

void objectProbeNoteGuiSourceDraw(ID3D11DeviceContext* ctx,char kind,uint32_t count,uint32_t instances,
                                 uint32_t startInstance,uint32_t start,int32_t base) {
    if(!detail::g_objectProbeLedgerOn || !ctx || g_frame+1<g_ledgerFrame0 || g_frame+1>g_ledgerLastFrame)return;
    const uint64_t vs=bindingShaderHash(BindSlot::Vs);if(!GuiDrawSnapshot::gui(vs))return;
    g_guiSnapshot.capture(ctx,g_frame+1,vs,bindingShaderHash(BindSlot::Ps),kind,count,instances,start,base,startInstance);
}

namespace {

// NOINLINE: buf->GetDesc writes into a local D3D11_BUFFER_DESC, which is
// what earns objectProbeOnEyeDraw its /GS stack cookie despite this branch
// running only when the pool buffer's identity first changes, not once per
// draw. Lifted out verbatim (the precedent: vscreen.cpp's forwardQuadSkip,
// device_hook.cpp's noteDeviceCreateFailure) so the per-draw function is
// relieved of it.
__declspec(noinline) void objectProbeNotePoolFirstSeen(ID3D11Buffer* buf) {
    // The usage decides what an unwritten slot holds: a
    // dynamic buffer is renamed on every discarding map,
    // so a slot the game did not write this frame carries
    // whatever the allocation held last time round -- a
    // stale record, counted live (the 11:36 flight: 175-206
    // "allocated" a pair against 3-20 freed, on the pad).
    D3D11_BUFFER_DESC bd{};
    buf->GetDesc(&bd);
    const char* usage = bd.Usage == D3D11_USAGE_DEFAULT     ? "default"
                        : bd.Usage == D3D11_USAGE_IMMUTABLE ? "immutable"
                        : bd.Usage == D3D11_USAGE_DYNAMIC
                            ? "dynamic (renamed on every discarding map: a slot the "
                              "game did not write this frame may hold a stale record)"
                            : "staging";
    Log::get().note(
        "object probe: the instanced-mesh pool is at VS t33 on the scene's draws -- "
        "a structured buffer of %u bytes, %u records of %u (%.1f MB), object %p, "
        "usage %s, cpu access 0x%X, bind 0x%X, misc 0x%X. Nothing on the draw path "
        "but one shader-resource read a second; copied a frame only while the eye "
        "run's ledger is armed (tools/eye_run_ledger.py). The per-object motion "
        "estimate this line used to describe (a two-frame diff every eighth frame) "
        "retired 2026-09-23; see the file header.",
        g_poolBytes, g_records, kRecordBytes,
        static_cast<double>(g_poolBytes) / 1048576.0,
        static_cast<void*>(g_pool), usage, static_cast<unsigned>(bd.CPUAccessFlags),
        static_cast<unsigned>(bd.BindFlags), static_cast<unsigned>(bd.MiscFlags));
}

}  // namespace

void objectProbeOnEyeDraw(ID3D11DeviceContext* ctx, char kind, uint32_t count, uint32_t instances,
                          uint32_t startInstance,uint32_t start,int32_t base) {
    if ((!detail::g_objectProbeOn && !detail::g_objectProbeLedgerOn) || !ctx) return;
    if (detail::g_objectProbeLedgerOn) ledgerNoteDraw(ctx, kind, count, instances, startInstance,start,base);
    if (g_checksLeft == 0) return;
    // The record-carrying families are instanced (question 5: every carrier
    // declares INSTANCEANDMODELDATAINDEX); a plain draw is not asked.
    if ((kind != 'X' && kind != 'N') || instances == 0) return;
    --g_checksLeft;
    ID3D11ShaderResourceView* srv = nullptr;
    bool got = false;
    guardedBudget(g_budget, [&] {
        ctx->VSGetShaderResources(kPoolSlot, 1, &srv);
        got = true;
    });
    if (!got || !srv) return;
    ResourceInfo info;
    if (bindingResolve(srv, &info) && info.isBuffer && info.b == kRecordBytes &&
        info.a >= kRecordBytes) {
        ID3D11Resource* res = nullptr;
        srv->GetResource(&res);
        ID3D11Buffer* buf = nullptr;
        if (res) {
            res->QueryInterface(__uuidof(ID3D11Buffer), reinterpret_cast<void**>(&buf));
            res->Release();
        }
        if (buf) {
            if (buf != g_pool) {
                releasePool();
                g_pool = buf;   // the QueryInterface reference is the one held
                g_poolBytes = info.a;
                g_records = info.a / kRecordBytes;
                if (!g_noted) {
                    g_noted = true;
                    objectProbeNotePoolFirstSeen(buf);
                }
            } else {
                buf->Release();
            }
            g_checksLeft = 0;
            g_checkMs = stampMs();
        }
    }
    srv->Release();
}

void objectProbeFrameBoundary(ID3D11DeviceContext* ctx) {
    if (!detail::g_objectProbeOn && !detail::g_objectProbeLedgerOn) {
        if (cullGateProbe.armed()) closeGateProbe();   // switched off mid-window: release the relays
        if (g_wasOn) {
            // Switched off live: the copies and the reference go.
            releaseRing();
            releasePool();
            detail::g_objectProbeLedgerOn = false;
            g_wasOn = false;
            g_noted = false;
        }
        return;
    }
    g_wasOn = true;
    ++g_frame;
    // The cull gate probe stamps the jobs it observes with the ledger frame
    // the draws submitted from now on belong to (ledgerNoteDraw's g_frame+1),
    // and lets go of the relays one frame after its window.
    cullGateProbe.setFrame(g_frame + 1);
    if (cullGateProbe.armed() && g_frame + 1 > cullGateProbe.lastFrame() + 1) closeGateProbe();
    g_checksLeft = (g_pool && !dueMs(g_checkMs, kRecheckMs)) ? 0 : kChecksPerFrame;
    if (!ctx) return;
    guardedBudget(g_budget, [&] {
        if (g_pool) {
            g_framesWithoutPool = 0;
            // Outside an armed run no copy is issued at all any more (it only
            // ever fed the retired diff); during one, the ledger wants its
            // pool copy every frame.
            if (ledgerCopyWanted()) issueCopy(ctx);
            poll(ctx);
        } else if (++g_framesWithoutPool == kAbsentFrames && !g_absentNoted && g_verbose) {
            g_absentNoted = true;
            Log::get().note(
                "object probe: on, but no 336-byte structured buffer has been found at VS t33 on "
                "the first instanced eye draws of %u frames -- not a rendered scene yet (a menu, "
                "a loading screen), or the pool has moved from where the 2026-09-06 dump put it. "
                "It keeps looking.",
                kAbsentFrames);
        }
        // A non-pool celestial/UI capture must still finish and report its
        // draw snapshots, even if no scene instance pool was discovered.
        if (detail::g_objectProbeLedgerOn) {
            if (g_frame <= g_ledgerLastFrame) ledgerIssue(ctx);
            ledgerPoll(ctx);
            for (AuxSlot& a : g_aux) a.seenThisFrame = false;
            for (bool& seen : g_paletteSeen) seen = false;
            if (g_frame > g_ledgerLastFrame + kReadAfter + kDropAfter) writeLedger(ctx);
        }
    });
}

void objectProbeNoteEarlyDraw(ID3D11DeviceContext* ctx, char kind, uint32_t count,
                             uint32_t instances, uint32_t startInstance,uint32_t start,int32_t base) {
    if (objectProbeLedgerActive() && ctx) ledgerNoteDraw(ctx, kind, count, instances, startInstance,start,base);
}

void objectProbeArmLedger(const wchar_t* stamp) {
    if (detail::g_objectProbeLedgerOn) return;
    wcsncpy_s(g_ledgerStamp, 16, stamp ? stamp : L"000000", _TRUNCATE);
    g_ledgerFrame0 = g_frame + 1;   // this frame's draws get their copy at the next boundary
    g_ledgerLastFrame = g_ledgerFrame0 + static_cast<uint32_t>(kLedgerFrames) - 1u;
    for (int k = 0; k < kLedgerCrops; ++k) g_ledgerCropFrame[k] = -1;
    for (int i = 0; i < kLedgerFrames; ++i) {
        g_ledgerPool[i].clear();
        g_ledgerInst[i].clear();
        for (int p = 0; p < kLedgerPalettes; ++p) g_ledgerPalette[p][i].clear();
        g_ledgerDraws[i].clear();
        g_ledgerAux[i].clear();
    }
    g_ledgerSkipped = 0;
    ledgerRelease();
    detail::g_objectProbeLedgerOn = true;
    g_gateRun = false;
    g_eyeMeshSnapshot.armGeometry(0);
    if (cullGateProbe.enabled()) armGateProbe();   // advanced.cull_gate_capture rides the eye run
    // The classification's own mesh records came from the retired mesh
    // capture (2026-09-23); arming it still arms the record-writer and
    // kinematic eval probes, whose own hooks fill the run's classification
    // JSON, as they always did with that capture off (the default). Clock 0:
    // that capture's frame count, which never advanced with it off.
    objectClassificationProbe.arm(0u);
    Log::get().note("object classification: armed with eye run %ls (the record-writer and kinematic eval probes; "
                    "its mesh-record discovery retired with mesh motion on 2026-09-23). No rendering changes.",
                    g_ledgerStamp);
    // The eye run is an explicit diagnostic capture. Pair its ledger with
    // the full draw census so offscreen effects and surviving billboard
    // particles have their PS/resources recorded without another keypress.
    if (!drawCensusArmed()) {
        drawCensusAutoRequest();
        Log::get().note("object probe: eye run %ls also armed the full draw census; "
                        "visible substituted particles are included before replacement.", g_ledgerStamp);
    }
}

void objectProbeLedgerMark(int k) {
    if (!detail::g_objectProbeLedgerOn || k < 0 || k >= kLedgerCrops) return;
    g_ledgerCropFrame[k] = static_cast<int>(g_frame + 1);   // as ledgerNoteDraw counts this frame
}

void objectProbeShutdown() {
    if (cullGateProbe.armed()) closeGateProbe();
    g_gateRun = false;   // no run survives shutdown: its geometry arm is released, not kept
    objectClassificationProbe.reset();
    releaseRing();
    releasePool();
    detail::g_objectProbeLedgerOn = false;
    detail::g_objectProbeOn = false;
    g_wasOn = false;
}

}  // namespace edvr
