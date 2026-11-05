#include "temporal_shader_bytecode.h"   // kMapIconFacingVsBytecode and the rest: the build's compiles of galaxy_map_vs.h
#include "galaxy_map_ui.h"
#include "galaxy_map_axes.h"        // mapCamAxes: the one pair of axes the world quads are spanned on
#include "galaxy_map_vs.h"          // kMapBodies: the marker shader's table
#include "../common/temporal_mode.h"  // temporalMapCloseView

#include <windows.h>

#include <d3d11.h>
#include <wrl/client.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <new>

#include "../common/config.h"
#include "../common/guard.h"
#include "../common/log.h"
#include "flat_compute_readback.h"   // FlatComputeInternalScope: EDVR's own calls do not reach the game's tees
#include "flat_stationary_camera_capture.h"   // mapCameraPose, mapCameraChanges: the head-free up the icons, labels and rings are fixed to
#include "shader_swap.h"
#include "vscreen.h"                 // the raw VS/PS/SRV setters: the binding shadow keeps naming the game's

namespace edvr {
namespace mapui {

Counters g_counters;

namespace {

using Microsoft::WRL::ComPtr;

constexpr uint32_t kRing = 4;       // eye-frames in flight per eye
constexpr uint32_t kPerFrame = 4;   // probes per eye-frame (the arrow's shader draws other sprites some frames; each is a probe and only the arrow's can count)
constexpr unsigned kMaxFlipNotes = 16;
constexpr UINT kCamSlot = 10;      // vertex constant slot of MapCam (galaxy_map_vs.h): the game's vertex shaders declare only 0, 1, 2, 3, 12 and 13

struct Group {
    uint64_t seq = 0;
    uint32_t n = 0;
    ComPtr<ID3D11Query> q[kPerFrame];
};
struct EyeRing {
    Group g[kRing];
    uint32_t head = 0, count = 0;
    bool over = false;
    uint64_t probedSeq = 0;   // the last eye-frame a probe was issued for
};

// A mask for the target the game draws the panels into, when the layer is not taking them (galaxy_map_ui.h, THE MASK WITHOUT THE LAYER).
struct DirectMask {
    ID3D11Resource* target = nullptr;   // identity only
    ComPtr<ID3D11Texture2D> tex;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11ShaderResourceView> srv;
    UINT w = 0, h = 0;
    uint64_t seq = 0;                   // the frame it was last cleared for
};

struct State {
    ID3D11Device* device = nullptr;   // identity only
    DirectMask direct[2];
    ComPtr<ID3D11BlendState> directBlend[2];   // maskBlendDesc, probeBlendDesc
    uint64_t directFrame = 1;                  // stepped by the frame tick (setGates)
    bool directSaid = false;
    ComPtr<ID3D11Buffer> bodyBuf;              // MapBodies at vertex slot 11
    uint64_t bodyWritten = 0;                  // the table's generation in the buffer
    bool markerAtlasSaid = false;
    ComPtr<ID3D11VertexShader> swapVs[kSwapCount];
    bool swapTried[kSwapCount] = {};
    ComPtr<ID3D11VertexShader> probeVs;
    ComPtr<ID3D11PixelShader> probePs, probeStickyPs;
    bool probeTried = false;
    ComPtr<ID3D11PixelShader> depthPs[2];   // the depth issue's stand-ins for kPsSprite and kPsCanvas
    bool depthPsTried = false;
    ComPtr<ID3D11VertexShader> savedVs;
    bool bound = false;
    EyeRing eye[2];
    bool failed = false;
    const char* said[4] = {};
    unsigned flips = 0;
    ComPtr<ID3D11Buffer> camBuf;      // MapCam at vertex slot 10: the world quads' axes (R, U, F), the map camera's position and the two eyes' offsets
    bool camWritten = false;
    uint64_t camChanges = 0;          // mapCameraChanges() at the last write
    uint64_t eyeGen = 0;              // mapCameraEyes' generation at the last write that carried the eyes
    bool eyesWritten = false;         // the last write carried the eyes (mapPos.w 1)
    bool camBoundSaid = false, camFailedSaid = false, eyesSaid = false;
    std::atomic<bool> resetPending{false};
};

State* g_st = nullptr;

State& st() {
    if (!g_st) g_st = new State;   // never freed: D3D objects released at process exit would run in a dead device's shadow
    return *g_st;
}

const unsigned char* const kBytecode[kSwapCount] = {kMapIconFacingVsBytecode, kMapIconUvFacingVsBytecode, kMapArrowDepthVsBytecode, kMapRingVsBytecode, kMapStemVsBytecode, kMapMarkerVsBytecode, kMapReticuleVsBytecode};
const size_t kBytecodeLen[kSwapCount] = {sizeof(kMapIconFacingVsBytecode), sizeof(kMapIconUvFacingVsBytecode), sizeof(kMapArrowDepthVsBytecode), sizeof(kMapRingVsBytecode), sizeof(kMapStemVsBytecode), sizeof(kMapMarkerVsBytecode), sizeof(kMapReticuleVsBytecode)};
const char* const kSwapName[kSwapCount] = {"map_icon_facing_vs", "map_icon_uv_facing_vs", "map_arrow_depth_vs", "map_ring_vs", "map_stem_vs", "map_marker_vs", "map_reticule_vs"};
const char* const kSwapLabel[kSwapCount] = {"C31238331D8AC3D4 (icons)", "12382D2EA45E9632 (labels)", "4EF6DDB075A927FA (arrow)", "94EA991C3A03DBD9 (rings)", "3028E7C6E0E097F9 (stems)", "12382D2EA45E9632 (the System Map's markers)", "C31238331D8AC3D4 (the System Map's reticule)"};

// The bodies' table (galaxy_map_ui.h, THE BODIES; its rows, galaxy_map_vs.h kMapBodyRows): xyz the centre, w the marker's scale, a zero w ends it. Written behind a try-lock from whichever thread unmaps the constants; a busy writer drops that one row, and the game writes it again next frame.
constexpr uint32_t kBodySlot = 11;
struct BodyTable {
    std::atomic<bool> busy{false};
    float rows[kMapBodyRows][4] = {};
    uint32_t n = 0;
    uint32_t oldest = 0;   // full: the next row to give up. A system can have more bodies than rows; those in view write their constants each frame, before their sprites
    float depth = 0.0f;
    std::atomic<uint64_t> generation{1};
};
BodyTable g_bodies;
thread_local ID3D11Resource* t_bodyRes = nullptr;
thread_local const float* t_bodyData = nullptr;

// The table in the MapBodies buffer, bound at slot 11 when it does not already hold it. False when the buffer cannot be made: the game's own marker sizes and no replacement shader.
bool bindBodies(State& s, ID3D11DeviceContext* ctx) {
    const uint64_t generation = g_bodies.generation.load(std::memory_order_acquire);
    if (!s.bodyBuf || s.bodyWritten != generation) {
        float copy[kMapBodyRows][4];
        while (g_bodies.busy.exchange(true, std::memory_order_acquire)) {}
        std::memcpy(copy, g_bodies.rows, sizeof(copy));
        g_bodies.busy.store(false, std::memory_order_release);
        if (!s.bodyBuf) {
            ComPtr<ID3D11Device> dev;
            ctx->GetDevice(&dev);
            D3D11_BUFFER_DESC bd{};
            bd.ByteWidth = sizeof(copy);
            bd.Usage = D3D11_USAGE_DEFAULT;
            bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            D3D11_SUBRESOURCE_DATA init{copy, 0, 0};
            if (!dev || FAILED(dev->CreateBuffer(&bd, &init, &s.bodyBuf))) {
                s.bodyBuf.Reset();
                return false;
            }
        } else {
            ctx->UpdateSubresource(s.bodyBuf.Get(), 0, nullptr, copy, 0, 0);
        }
        s.bodyWritten = generation;
    }
    ID3D11Buffer* bound = nullptr;
    ctx->VSGetConstantBuffers(kBodySlot, 1, &bound);
    const bool same = bound == s.bodyBuf.Get();
    if (bound) bound->Release();
    if (!same) {
        ID3D11Buffer* mine = s.bodyBuf.Get();
        ctx->VSSetConstantBuffers(kBodySlot, 1, &mine);
    }
    return true;
}

// A new device drops everything made for the old one.
void bindDevice(State& s, ID3D11DeviceContext* ctx) {
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    if (dev == s.device) {
        if (dev) dev->Release();
        return;
    }
    for (auto& v : s.swapVs) v.Reset();
    for (bool& t : s.swapTried) t = false;
    s.probeVs.Reset();
    s.probePs.Reset();
    s.probeStickyPs.Reset();
    s.probeTried = false;
    for (auto& p : s.depthPs) p.Reset();
    s.depthPsTried = false;
    for (DirectMask& m : s.direct) m = DirectMask{};
    for (auto& b : s.directBlend) b.Reset();
    s.bodyBuf.Reset();
    s.bodyWritten = 0;
    s.savedVs.Reset();
    s.bound = false;
    s.camBuf.Reset();
    s.camWritten = false;
    s.eyesWritten = false;
    for (EyeRing& r : s.eye) r = EyeRing{};
    s.device = dev;
    if (dev) dev->Release();   // identity only
}

// The map camera's constants in the MapCam buffer, bound at slot 10 (galaxy_map_vs.h): the one pair of axes the world quads are spanned on (galaxy_map_axes.h) and its forward, mapPos (stage 0's position, w 1 when the eyes are valid) and the two eyes' offsets as vectors
// in the map frame. The buffer is written when mapCameraChanges() has moved since the last write (the first write included), or the eyes' pair has changed or become valid or invalid, or the temporary key has changed the choice of up,
// and slot 10 is read and set only when it does not hold ours: there can be 500 such draws a frame, and the read catches a game call that cleared the slots. False until the pose has
// answered once; after that the last answer stands when the camera's reader has none. Without the eyes mapPos.w is 0 and the shaders use the game's own eye.
bool bindMapCam(State& s, ID3D11DeviceContext* ctx) {
    const uint64_t changes = mapCameraChanges();
    float eyes[2][3] = {};
    uint64_t eyeGen = 0;
    const bool eyesOk = mapCameraEyes(eyes, &eyeGen);
    // R, U, F at floats 0..11 (w 0), the position at 12..15 (w 1 when the eyes are valid), the two eyes' offsets at 16..19 and 20..23. Before the pose answers the camera's own default axes.
    float cam[24] = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f};
    const bool eyesMoved = eyesOk ? (!s.eyesWritten || eyeGen != s.eyeGen) : s.eyesWritten;
    if (!s.camBuf || !s.camWritten || changes != s.camChanges || eyesMoved) {
        MapCameraPose pose;
        if (mapCameraPose(pose)) {
            const MapAxes axes = mapCamAxes(pose.rows);
            for (int k = 0; k < 3; ++k) {
                cam[k] = axes.R[k];
                cam[4 + k] = axes.U[k];
                cam[8 + k] = axes.F[k];
                cam[12 + k] = pose.position[k];
            }
            cam[15] = eyesOk ? 1.0f : 0.0f;
            if (eyesOk) {
                for (int e = 0; e < 2; ++e)
                    for (int k = 0; k < 3; ++k)
                        cam[16 + 4 * e + k] = eyes[e][0] * pose.rows[0][k] + eyes[e][1] * pose.rows[1][k] + eyes[e][2] * pose.rows[2][k];
            }
            if (!s.camBuf) {
                ComPtr<ID3D11Device> dev;
                ctx->GetDevice(&dev);
                D3D11_BUFFER_DESC bd{};
                bd.ByteWidth = sizeof(cam);
                bd.Usage = D3D11_USAGE_DEFAULT;
                bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
                D3D11_SUBRESOURCE_DATA init{cam, 0, 0};
                if (!dev || FAILED(dev->CreateBuffer(&bd, &init, &s.camBuf))) {
                    s.camBuf.Reset();
                    if (!s.camFailedSaid) {
                        s.camFailedSaid = true;
                        Log::get().note("galaxy map: the map camera's constant buffer could not be made: the icons, labels, rings and stems are drawn as the game made them");
                    }
                    return false;
                }
            } else {
                ctx->UpdateSubresource(s.camBuf.Get(), 0, nullptr, cam, 0, 0);
            }
            s.camWritten = true;
            s.camChanges = changes;
            if (eyesOk) {
                g_counters.eyeGenSteps.fetch_add(s.eyesWritten && eyeGen > s.eyeGen ? eyeGen - s.eyeGen : 1, std::memory_order_relaxed);
                g_counters.eyesValid.fetch_add(1, std::memory_order_relaxed);
                if (!s.eyesSaid) {
                    s.eyesSaid = true;
                    Log::get().note("galaxy map: the two eyes' offsets from the map camera's position are published (eye 0 %.4f %.4f %.4f, eye 1 %.4f %.4f %.4f in the map frame): the overlays are held against the game's rounded eye",
                                    cam[16], cam[17], cam[18], cam[20], cam[21], cam[22]);
                }
            }
            s.eyesWritten = eyesOk;
            s.eyeGen = eyesOk ? eyeGen : 0;
            g_counters.camWrites.fetch_add(1, std::memory_order_relaxed);
        } else if (!s.camWritten) {
            return false;
        }
    }
    if (!s.camBuf) return false;
    ID3D11Buffer* held = nullptr;
    ctx->VSGetConstantBuffers(kCamSlot, 1, &held);
    if (held != s.camBuf.Get()) {
        ID3D11Buffer* ours = s.camBuf.Get();
        ctx->VSSetConstantBuffers(kCamSlot, 1, &ours);
        if (!s.camBoundSaid) {
            s.camBoundSaid = true;
            Log::get().note("galaxy map: the map camera's constants are bound at vertex constant slot %u for the icons, labels, rings and stems (slot held %s before; first up axis %.3f %.3f %.3f, eyes %s)", kCamSlot,
                            held ? "another buffer" : "nothing", cam[4], cam[5], cam[6], eyesOk ? "valid" : "not yet published: the game's eye");
        }
    }
    if (held) held->Release();
    return true;
}

void refreshOver(State& s) {
    const bool now = s.failed || s.eye[0].over || s.eye[1].over;
    if (now == g_over.load(std::memory_order_relaxed)) return;
    g_over.store(now, std::memory_order_relaxed);
    if (s.flips < kMaxFlipNotes) {
        ++s.flips;
        Log::get().note("galaxy map: the arrow's tip is %s a panel%s (flip %u of the first %u said; later ones are in the 30 s line)", now ? "on" : "off",
                        s.failed ? " (no mask or no query: the game's arrow)" : "", s.flips, kMaxFlipNotes);
    }
}

void resetRings(State& s) {
    for (EyeRing& r : s.eye) r = EyeRing{};
    s.failed = false;
    g_over.store(false, std::memory_order_relaxed);
}

// The oldest eye-frames whose queries have all come back, never waited for: the frame in progress (seq) is left alone.
void drain(State& s, ID3D11DeviceContext* ctx, int eye, uint64_t seq) {
    EyeRing& r = s.eye[eye];
    while (r.count) {
        Group& g = r.g[r.head];
        if (g.seq == seq) return;
        uint64_t sum = 0;
        bool broken = false;
        for (uint32_t k = 0; k < g.n; ++k) {
            UINT64 samples = 0;
            const HRESULT h = ctx->GetData(g.q[k].Get(), &samples, sizeof(samples), D3D11_ASYNC_GETDATA_DONOTFLUSH);
            if (h == S_FALSE) return;   // not yet: the next probe asks again
            if (h != S_OK) { broken = true; break; }
            sum += samples;
        }
        if (broken) {
            for (uint32_t k = 0; k < g.n; ++k) g.q[k].Reset();
            g.n = 0;
            r.head = (r.head + 1) % kRing;
            --r.count;
            g_counters.probeFaults.fetch_add(1, std::memory_order_relaxed);
            noteUnavailable("occlusion query readback");
            continue;
        }
        g_counters.answered.fetch_add(1, std::memory_order_relaxed);
        if (sum > 0) g_counters.framesOver.fetch_add(1, std::memory_order_relaxed);
        else g_counters.framesClear.fetch_add(1, std::memory_order_relaxed);
        r.over = sum > 0;
        g.n = 0;
        r.head = (r.head + 1) % kRing;
        --r.count;
        refreshOver(s);
    }
}

} // namespace

void bodyConstantsMapped(ID3D11Resource* res, const void* data) {
    t_bodyRes = nullptr;
    D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
    res->GetType(&dim);
    if (dim != D3D11_RESOURCE_DIMENSION_BUFFER) return;
    D3D11_BUFFER_DESC d{};
    static_cast<ID3D11Buffer*>(res)->GetDesc(&d);
    if (d.ByteWidth != kBodyConstantBytes || !(d.BindFlags & D3D11_BIND_CONSTANT_BUFFER)) return;
    t_bodyRes = res;
    t_bodyData = static_cast<const float*>(data);
}

void bodyConstantsUnmapped(ID3D11Resource* res) {
    if (res != t_bodyRes || !t_bodyData) return;
    t_bodyRes = nullptr;
    const float* centre = t_bodyData + 125 * 4;
    const float* radius = t_bodyData + 126 * 4;
    if (!(centre[0] == 1.0f) || !(radius[0] > 0.0f)) return;
    const float k = markerScale(radius[0]);
    if (g_bodies.busy.exchange(true, std::memory_order_acquire)) return;
    uint32_t at = g_bodies.n;
    for (uint32_t i = 0; i < g_bodies.n; ++i) {
        const float dx = g_bodies.rows[i][0] - centre[1], dy = g_bodies.rows[i][1] - centre[2], dz = g_bodies.rows[i][2] - centre[3];
        if (dx * dx + dy * dy + dz * dz < 0.0025f) { at = i; break; }
    }
    const bool isNew = at == g_bodies.n;
    if (isNew && at == kMapBodies) {
        at = g_bodies.oldest;
        g_bodies.oldest = (g_bodies.oldest + 1) % kMapBodies;
    }
    float& radiusSlot = g_bodies.rows[kMapRadiusRow + at / 4][at % 4];
    bool changed = false;
    if (isNew || g_bodies.rows[at][3] != k || radiusSlot != radius[0]) {
        g_bodies.rows[at][0] = centre[1];
        g_bodies.rows[at][1] = centre[2];
        g_bodies.rows[at][2] = centre[3];
        g_bodies.rows[at][3] = k;
        radiusSlot = radius[0];
        if (isNew && g_bodies.n < kMapBodies) ++g_bodies.n;
        changed = true;
    }
    g_bodies.busy.store(false, std::memory_order_release);
    if (changed) g_bodies.generation.fetch_add(1, std::memory_order_release);
}

uint32_t bodiesKnown() { return g_bodies.n; }
float arrowDepth() { return g_bodies.depth; }

void setSystemOpen(bool system) {
    const bool was = g_systemOpen.exchange(system, std::memory_order_relaxed);
    if (system != was) {
        // Open: a new system's bodies start a new table. Shut: the arrow's depth goes back to infinity for the Galaxy Map.
        while (g_bodies.busy.exchange(true, std::memory_order_acquire)) {}
        std::memset(g_bodies.rows, 0, sizeof(g_bodies.rows));
        g_bodies.n = 0;
        g_bodies.oldest = 0;
        g_bodies.depth = 0.0f;
        g_bodies.busy.store(false, std::memory_order_release);
        g_bodies.generation.fetch_add(1, std::memory_order_release);
        temporalMapCloseView().store(false, std::memory_order_relaxed);
    }
}

void updateArrowDepth(const float cameraPosition[3], const float ray[3], const float forward[3]) {
    if (g_bodies.busy.exchange(true, std::memory_order_acquire)) return;   // a writer is in: next frame's will do
    const float distance = rayBodyDistance(cameraPosition, ray, g_bodies.rows, g_bodies.n);
    float* arrowRow = g_bodies.rows[kMapArrowRow];
    float* cameraRow = g_bodies.rows[kMapCameraRow];
    // No body in front of the camera has no depth to give: the arrow keeps the last one.
    const float inverse = distance > 0.0f ? arrowDepthStep(arrowRow[0], arrowInverseDepth(distance)) : arrowRow[0];
    const bool changed = arrowRow[0] != inverse || cameraRow[0] != cameraPosition[0] || cameraRow[1] != cameraPosition[1] || cameraRow[2] != cameraPosition[2] || cameraRow[3] != 1.0f;
    arrowRow[0] = inverse;
    cameraRow[0] = cameraPosition[0];
    cameraRow[1] = cameraPosition[1];
    cameraRow[2] = cameraPosition[2];
    cameraRow[3] = 1.0f;
    g_bodies.depth = inverse > 0.0f ? 1.0f / inverse : 0.0f;
    const bool closeView = bodyFillsView(cameraPosition, forward, g_bodies.rows, g_bodies.n);
    g_bodies.busy.store(false, std::memory_order_release);
    temporalMapCloseView().store(closeView, std::memory_order_relaxed);
    if (changed) g_bodies.generation.fetch_add(1, std::memory_order_release);
}

void setGates(bool vr, bool pointer) {
    const bool wasPointer = g_pointerOpen.exchange(pointer, std::memory_order_relaxed);
    g_vrOpen.store(vr, std::memory_order_relaxed);
    g_swapOpen.store(vr || pointer || g_systemOpen.load(std::memory_order_relaxed), std::memory_order_relaxed);
    if (g_st) ++g_st->directFrame;
    if (wasPointer && !pointer) {
        g_over.store(false, std::memory_order_relaxed);
        if (g_st) {
            g_st->resetPending.store(true, std::memory_order_release);   // the render thread lets the queries go at its next probe
            for (DirectMask& m : g_st->direct) m = DirectMask{};         // the frame tick is the render thread: the masks made without the layer go with the map
        }
    }
}

void noteUnavailable(const char* what) {
    State& s = st();
    g_counters.maskUnavailable.fetch_add(1, std::memory_order_relaxed);
    s.failed = true;
    bool said = false;
    for (const char*& slot : s.said) {
        if (slot && std::strcmp(slot, what) == 0) { said = true; break; }
        if (!slot) { slot = what; break; }
    }
    if (!said) Log::get().note("galaxy map: %s unavailable: the arrow stays the game's, at the panels' depth", what);
    refreshOver(s);
}

bool swapBegin(ID3D11DeviceContext* ctx, uint64_t vs) {
    uint32_t which;
    if (vs == kVsIconA) which = systemOpen() ? kSwapReticule : kSwapIconA;
    else if (vs == kVsIconB) which = systemOpen() ? kSwapMarker : kSwapIconB;
    else if (vs == kVsArrow) which = kSwapArrow;
    else if (vs == kVsRing) which = kSwapRing;
    else if (vs == kVsStem) which = kSwapStem;
    else return false;
    if (!ctx) return false;
    State& s = st();
    if (s.bound) return false;
    if (which == kSwapArrow) {
        if (!pointerOpen()) return false;
        if (over()) {
            g_counters.arrowKept.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
    } else if (which != kSwapMarker && which != kSwapReticule && !vrOpen()) {
        return false;
    }
    FlatComputeInternalScope internal;
    bindDevice(s, ctx);
    if (!s.swapVs[which] && !s.swapTried[which]) {
        s.swapTried[which] = true;
        s.swapVs[which].Attach(shaderSwapCreateVs(ctx, kBytecode[which], kBytecodeLen[which], kSwapName[which], "galaxy map"));
        if (!s.swapVs[which])
            Log::get().note("galaxy map: the replacement for vertex shader %s could not be made: it is drawn as the game made it", kSwapLabel[which]);
        else
            Log::get().note("galaxy map: the replacement for vertex shader %s is made (%u bytes of bytecode)", kSwapLabel[which], static_cast<unsigned>(kBytecodeLen[which]));
    }
    ID3D11VertexShader* ours = s.swapVs[which].Get();
    if (!ours) return false;
    if (which == kSwapMarker || which == kSwapArrow || which == kSwapReticule) {
        if (!bindBodies(s, ctx)) return false;
        if (which == kSwapMarker && !s.markerAtlasSaid) {
            s.markerAtlasSaid = true;
            // Which kind of minification the small settlement icons have: a mip chain to fall on, or none.
            ID3D11ShaderResourceView* srv = nullptr;
            ID3D11SamplerState* smp = nullptr;
            ctx->PSGetShaderResources(0, 1, &srv);
            ctx->PSGetSamplers(0, 1, &smp);
            D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
            D3D11_SAMPLER_DESC pd{};
            if (srv) srv->GetDesc(&sd);
            if (smp) smp->GetDesc(&pd);
            Log::get().note("galaxy map: the System Map's first marker: its atlas view has %u mip level(s), its sampler filter 0x%X, bias %.2f, max LOD %.1f",
                            sd.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2D ? sd.Texture2D.MipLevels : 0u, static_cast<unsigned>(pd.Filter), pd.MipLODBias, pd.MaxLOD);
            if (srv) srv->Release();
            if (smp) smp->Release();
        }
    } else if (!bindMapCam(s, ctx)) {
        g_counters.noPose.fetch_add(1, std::memory_order_relaxed);   // the pose never answered: the draw is the game's
        return false;
    }
    ctx->VSGetShader(&s.savedVs, nullptr, nullptr);
    vScreenVSSetShaderRaw(ctx, ours, nullptr, 0);
    s.bound = true;
    g_counters.swapped[which].fetch_add(1, std::memory_order_relaxed);
    return true;
}

void swapEnd(ID3D11DeviceContext* ctx) {
    State& s = st();
    if (!s.bound) return;
    s.bound = false;
    if (ctx) {
        FlatComputeInternalScope internal;
        vScreenVSSetShaderRaw(ctx, s.savedVs.Get(), nullptr, 0);
    }
    s.savedVs.Reset();
}

bool depthIssue(ID3D11DeviceContext* ctx, uint64_t ps, const DepthDraw& dd) {
    if (!ctx || !dd.draw || (ps != kPsSprite && ps != kPsCanvas)) return false;
    State& s = st();
    FlatComputeInternalScope internal;
    bindDevice(s, ctx);
    if (!s.depthPsTried) {
        s.depthPsTried = true;
        s.depthPs[0].Attach(shaderSwapCreatePs(ctx, kMapDepthSpritePsBytecode, sizeof(kMapDepthSpritePsBytecode), "map_depth_sprite_ps", "galaxy map"));
        s.depthPs[1].Attach(shaderSwapCreatePs(ctx, kMapDepthCanvasPsBytecode, sizeof(kMapDepthCanvasPsBytecode), "map_depth_canvas_ps", "galaxy map"));
        if (!s.depthPs[0] || !s.depthPs[1]) {
            g_depth.unavailable.fetch_add(1, std::memory_order_relaxed);
            Log::get().note("galaxy map: the sprites' depth pixel shaders could not be made: the depth is not written, the sprites are drawn in the game's order");
        }
    }
    ID3D11PixelShader* ours = s.depthPs[ps == kPsSprite ? 0 : 1].Get();
    if (!ours) return false;
    ID3D11PixelShader* ps0 = nullptr;
    ctx->PSGetShader(&ps0, nullptr, nullptr);
    const bool ran = guarded("galaxy map depth issue", [&] {
        vScreenPSSetShaderRaw(ctx, ours, nullptr, 0);
        dd.draw(dd.user);
    });
    // Whatever happened, the game's pixel shader goes back.
    guarded("galaxy map depth issue restore", [&] { vScreenPSSetShaderRaw(ctx, ps0, nullptr, 0); });
    if (ps0) ps0->Release();
    if (!ran) {
        g_depth.faults.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    g_depth.issued.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void noteMaskIssue(uint64_t vs, int eye, uint64_t seq) {
    const int i = panelIndex(vs);
    if (i >= 0) g_counters.maskIssued[i].fetch_add(1, std::memory_order_relaxed);
    if (eye >= 0 && eye < 2 && g_st && g_st->eye[eye].probedSeq == seq) g_counters.maskLate.fetch_add(1, std::memory_order_relaxed);
}

void probeIssue(ID3D11DeviceContext* ctx, int eye, uint64_t seq, ID3D11ShaderResourceView* mask, const ProbeDraw& pd) {
    if (!ctx || eye < 0 || eye > 1 || !mask || !pd.draw) return;
    State& s = st();
    FlatComputeInternalScope internal;
    bindDevice(s, ctx);
    if (s.resetPending.exchange(false, std::memory_order_acq_rel)) resetRings(s);
    if (!s.probeVs || !s.probePs) {
        if (!s.probeTried) {
            s.probeTried = true;
            s.probeVs.Attach(shaderSwapCreateVs(ctx, kMapProbeVsBytecode, sizeof(kMapProbeVsBytecode), "map_probe_vs", "galaxy map"));
            s.probePs.Attach(shaderSwapCreatePs(ctx, kMapProbePsBytecode, sizeof(kMapProbePsBytecode), "map_probe_ps", "galaxy map"));
            s.probeStickyPs.Attach(shaderSwapCreatePs(ctx, kMapProbeStickyPsBytecode, sizeof(kMapProbeStickyPsBytecode), "map_probe_sticky_ps", "galaxy map"));   // without it the exact test decides both ways
            if (!s.probeStickyPs) Log::get().note("galaxy map: the arrow probe's sticky edge could not be made: the arrow leaves a panel at its exact edge");
        }
        if (!s.probeVs || !s.probePs) {
            noteUnavailable("the arrow probe's shaders");
            return;
        }
    }
    drain(s, ctx, eye, seq);
    const bool sticky = s.probeStickyPs && over();   // kMapProbeStickyPS: getting on is exact, staying on is not
    EyeRing& r = s.eye[eye];
    Group* grp = nullptr;
    bool pushed = false;
    if (r.count && r.g[(r.head + r.count - 1) % kRing].seq == seq) {
        grp = &r.g[(r.head + r.count - 1) % kRing];
    } else {
        if (r.count == kRing) {
            g_counters.ringFull.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        grp = &r.g[(r.head + r.count) % kRing];
        grp->seq = seq;
        grp->n = 0;
        ++r.count;
        pushed = true;
    }
    auto undo = [&] { if (pushed) { --r.count; } };
    if (grp->n >= kPerFrame) {
        g_counters.perFrameFull.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    ComPtr<ID3D11Query>& q = grp->q[grp->n];
    if (!q) {
        ComPtr<ID3D11Device> dev;
        ctx->GetDevice(&dev);
        D3D11_QUERY_DESC qd{D3D11_QUERY_OCCLUSION, 0};
        if (!dev || FAILED(dev->CreateQuery(&qd, &q))) {
            undo();
            noteUnavailable("the occlusion query");
            return;
        }
    }
    r.probedSeq = seq;
    D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    ID3D11VertexShader* vs0 = nullptr;
    ID3D11PixelShader* ps0 = nullptr;
    ID3D11ShaderResourceView* srv0 = nullptr;
    ctx->IAGetPrimitiveTopology(&topology);
    ctx->VSGetShader(&vs0, nullptr, nullptr);
    ctx->PSGetShader(&ps0, nullptr, nullptr);
    ctx->PSGetShaderResources(0, 1, &srv0);
    bool began = false, ended = false;
    const bool ran = guarded("galaxy map probe", [&] {
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
        vScreenVSSetShaderRaw(ctx, s.probeVs.Get(), nullptr, 0);
        vScreenPSSetShaderRaw(ctx, sticky ? s.probeStickyPs.Get() : s.probePs.Get(), nullptr, 0);
        ID3D11ShaderResourceView* m = mask;
        vScreenPSSetShaderResourcesRaw(ctx, 0, 1, &m);
        ctx->Begin(q.Get());
        began = true;
        pd.draw(pd.user);
        ctx->End(q.Get());
        ended = true;
    });
    // Whatever happened, the game's state goes back: topology, shaders, and its t0.
    guarded("galaxy map probe restore", [&] {
        if (began && !ended) ctx->End(q.Get());
        ctx->IASetPrimitiveTopology(topology);
        vScreenVSSetShaderRaw(ctx, vs0, nullptr, 0);
        vScreenPSSetShaderRaw(ctx, ps0, nullptr, 0);
        vScreenPSSetShaderResourcesRaw(ctx, 0, 1, &srv0);
    });
    if (vs0) vs0->Release();
    if (ps0) ps0->Release();
    if (srv0) srv0->Release();
    if (!ran || !ended) {
        q.Reset();
        undo();
        g_counters.probeFaults.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    ++grp->n;
    g_counters.probes.fetch_add(1, std::memory_order_relaxed);
    if (sticky) g_counters.stickyProbes.fetch_add(1, std::memory_order_relaxed);
}

namespace {

// The mask for the target `rtv` names, made or re-made to its size, and cleared at the frame's first use. Null when it cannot be made (the caller says so). `eye` is the mask's slot: the rings are per slot, and over reads both.
DirectMask* directMaskFor(State& s, ID3D11DeviceContext* ctx, ID3D11RenderTargetView* rtv, int* eye) {
    ComPtr<ID3D11Resource> res;
    rtv->GetResource(&res);
    ComPtr<ID3D11Texture2D> target;
    if (!res || FAILED(res.As(&target))) return nullptr;
    D3D11_TEXTURE2D_DESC game{};
    target->GetDesc(&game);
    if (game.SampleDesc.Count != 1) return nullptr;
    int slot = -1;
    for (int i = 0; i < 2 && slot < 0; ++i) if (s.direct[i].target == res.Get()) slot = i;
    for (int i = 0; i < 2 && slot < 0; ++i) if (!s.direct[i].target) slot = i;
    if (slot < 0) {
        // A third target: the eyes' targets were made again. Both masks start over.
        for (DirectMask& m : s.direct) m = DirectMask{};
        slot = 0;
    }
    DirectMask& m = s.direct[slot];
    m.target = res.Get();
    if (!m.tex || m.w != game.Width || m.h != game.Height) {
        m.tex.Reset();
        m.rtv.Reset();
        m.srv.Reset();
        ComPtr<ID3D11Device> dev;
        ctx->GetDevice(&dev);
        D3D11_TEXTURE2D_DESC td{};
        td.Width = game.Width;
        td.Height = game.Height;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if (!dev || FAILED(dev->CreateTexture2D(&td, nullptr, &m.tex)) || FAILED(dev->CreateRenderTargetView(m.tex.Get(), nullptr, &m.rtv)) || FAILED(dev->CreateShaderResourceView(m.tex.Get(), nullptr, &m.srv))) {
            m = DirectMask{};
            return nullptr;
        }
        m.w = game.Width;
        m.h = game.Height;
        m.seq = 0;
        if (!s.directSaid) {
            s.directSaid = true;
            Log::get().note("galaxy map: the UI layer is not taking the panels (Anti-aliasing off, UI quality off or the layer stood down), so the panel mask and the arrow's probe run from the game's own draws, at %ux%u (A8_UNORM, %.1f MB an eye); let go when the map shuts.",
                            game.Width, game.Height, static_cast<double>(game.Width) * game.Height / (1024.0 * 1024.0));
        }
    }
    if (m.seq != s.directFrame) {
        const FLOAT zero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        vScreenClearRenderTargetViewRaw(ctx, m.rtv.Get(), zero);
        m.seq = s.directFrame;
    }
    *eye = slot;
    return &m;
}

ID3D11BlendState* directBlend(State& s, ID3D11DeviceContext* ctx, int which) {
    ComPtr<ID3D11BlendState>& b = s.directBlend[which];
    if (!b) {
        ComPtr<ID3D11Device> dev;
        ctx->GetDevice(&dev);
        const D3D11_BLEND_DESC d = which == 0 ? maskBlendDesc() : probeBlendDesc();
        if (dev) dev->CreateBlendState(&d, &b);
    }
    return b.Get();
}

// The game's targets and blend, held across a direct issue and put back on every way out.
struct DirectBracket {
    ID3D11DeviceContext* ctx;
    ID3D11RenderTargetView* rtv = nullptr;
    ID3D11DepthStencilView* dsv = nullptr;
    ID3D11BlendState* blend = nullptr;
    FLOAT factor[4] = {};
    UINT sampleMask = 0xFFFFFFFFu;
    explicit DirectBracket(ID3D11DeviceContext* c) : ctx(c) {
        ctx->OMGetRenderTargets(1, &rtv, &dsv);
        ctx->OMGetBlendState(&blend, factor, &sampleMask);
    }
    ~DirectBracket() {
        guarded("galaxy map direct restore", [&] {
            vScreenSetRenderTargetsRaw(ctx, 1, &rtv, dsv);
            vScreenOMSetBlendStateRaw(ctx, blend, factor, sampleMask);
        });
        if (rtv) rtv->Release();
        if (dsv) dsv->Release();
        if (blend) blend->Release();
    }
};

} // namespace

void directPanelIssue(ID3D11DeviceContext* ctx, uint64_t vs, const ProbeDraw& draw) {
    if (!ctx || !draw.draw) return;
    State& s = st();
    FlatComputeInternalScope internal;
    bindDevice(s, ctx);
    DirectBracket bracket(ctx);
    if (!bracket.rtv) return;
    int eye = 0;
    DirectMask* m = nullptr;
    ID3D11BlendState* blend = nullptr;
    const bool made = guarded("galaxy map direct mask", [&] {
        m = directMaskFor(s, ctx, bracket.rtv, &eye);
        blend = directBlend(s, ctx, 0);
    });
    if (!made || !m || !blend) {
        noteUnavailable("the panel mask");
        return;
    }
    const uint64_t seq = s.directFrame;
    const bool ran = guarded("galaxy map direct mask issue", [&] {
        ID3D11RenderTargetView* t = m->rtv.Get();
        vScreenSetRenderTargetsRaw(ctx, 1, &t, nullptr);
        vScreenOMSetBlendStateRaw(ctx, blend, nullptr, 0xFFFFFFFFu);
        draw.draw(draw.user);
    });
    if (ran) noteMaskIssue(vs, eye, seq);
}

void directArrowProbe(ID3D11DeviceContext* ctx, const ProbeDraw& point) {
    if (!ctx || !point.draw) return;
    State& s = st();
    FlatComputeInternalScope internal;
    bindDevice(s, ctx);
    DirectBracket bracket(ctx);
    if (!bracket.rtv) return;
    int eye = 0;
    DirectMask* m = nullptr;
    ID3D11BlendState* blend = nullptr;
    const bool made = guarded("galaxy map direct probe", [&] {
        m = directMaskFor(s, ctx, bracket.rtv, &eye);   // a frame with no panel draw: cleared here, so the probe reads clear rather than last frame's
        blend = directBlend(s, ctx, 1);
        // The game's target stays bound at write mask 0, with no depth: nothing is written and nothing tests the point.
        vScreenSetRenderTargetsRaw(ctx, 1, &bracket.rtv, nullptr);
        if (blend) vScreenOMSetBlendStateRaw(ctx, blend, nullptr, 0xFFFFFFFFu);
    });
    if (!made || !m || !blend) {
        noteUnavailable("the panel mask");
        return;
    }
    probeIssue(ctx, eye, s.directFrame, m->srv.Get(), point);
}

} // namespace mapui
} // namespace edvr
