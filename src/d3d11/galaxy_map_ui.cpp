#include "temporal_shader_bytecode.h"   // kMapIconFacingVsBytecode and the rest: the build's compiles of galaxy_map_vs.h
#include "galaxy_map_ui.h"

#include <windows.h>

#include <d3d11.h>
#include <wrl/client.h>

#include <cstdio>
#include <cstring>
#include <new>

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

struct State {
    ID3D11Device* device = nullptr;   // identity only
    ComPtr<ID3D11VertexShader> swapVs[kSwapCount];
    bool swapTried[kSwapCount] = {};
    ComPtr<ID3D11VertexShader> probeVs;
    ComPtr<ID3D11PixelShader> probePs;
    bool probeTried = false;
    ComPtr<ID3D11PixelShader> depthPs[2];   // the depth issue's stand-ins for kPsSprite and kPsCanvas
    bool depthPsTried = false;
    ComPtr<ID3D11VertexShader> savedVs;
    bool bound = false;
    EyeRing eye[2];
    bool failed = false;
    const char* said[4] = {};
    unsigned flips = 0;
    ComPtr<ID3D11Buffer> camBuf;      // MapCam at vertex slot 10: the map camera's up (stage 0 rows[1]), its position and the two eyes' offsets
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

const unsigned char* const kBytecode[kSwapCount] = {kMapIconFacingVsBytecode, kMapIconUvFacingVsBytecode, kMapArrowDepthVsBytecode, kMapRingVsBytecode, kMapStemVsBytecode};
const size_t kBytecodeLen[kSwapCount] = {sizeof(kMapIconFacingVsBytecode), sizeof(kMapIconUvFacingVsBytecode), sizeof(kMapArrowDepthVsBytecode), sizeof(kMapRingVsBytecode), sizeof(kMapStemVsBytecode)};
const char* const kSwapName[kSwapCount] = {"map_icon_facing_vs", "map_icon_uv_facing_vs", "map_arrow_depth_vs", "map_ring_vs", "map_stem_vs"};
const char* const kSwapLabel[kSwapCount] = {"C31238331D8AC3D4 (icons)", "12382D2EA45E9632 (labels)", "4EF6DDB075A927FA (arrow)", "94EA991C3A03DBD9 (rings)", "3028E7C6E0E097F9 (stems)"};

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
    s.probeTried = false;
    for (auto& p : s.depthPs) p.Reset();
    s.depthPsTried = false;
    s.savedVs.Reset();
    s.bound = false;
    s.camBuf.Reset();
    s.camWritten = false;
    s.eyesWritten = false;
    for (EyeRing& r : s.eye) r = EyeRing{};
    s.device = dev;
    if (dev) dev->Release();   // identity only
}

// The map camera's constants in the MapCam buffer, bound at slot 10 (galaxy_map_vs.h): mapUp, mapPos (stage 0's position, w 1 when the eyes are valid) and the two eyes' offsets as vectors
// in the map frame. The buffer is written when mapCameraChanges() has moved since the last write (the first write included), or the eyes' pair has changed or become valid or invalid,
// and slot 10 is read and set only when it does not hold ours: there can be 500 such draws a frame, and the read catches a game call that cleared the slots. False until the pose has
// answered once; after that the last answer stands when the camera's reader has none. Without the eyes mapPos.w is 0 and the shaders use the game's own eye.
bool bindMapCam(State& s, ID3D11DeviceContext* ctx) {
    const uint64_t changes = mapCameraChanges();
    float eyes[2][3] = {};
    uint64_t eyeGen = 0;
    const bool eyesOk = mapCameraEyes(eyes, &eyeGen);
    float cam[16] = {0.0f, 1.0f, 0.0f, 0.0f};
    const bool eyesMoved = eyesOk ? (!s.eyesWritten || eyeGen != s.eyeGen) : s.eyesWritten;
    if (!s.camBuf || !s.camWritten || changes != s.camChanges || eyesMoved) {
        MapCameraPose pose;
        if (mapCameraPose(pose)) {
            cam[0] = pose.rows[1][0];
            cam[1] = pose.rows[1][1];
            cam[2] = pose.rows[1][2];
            cam[4] = pose.position[0];
            cam[5] = pose.position[1];
            cam[6] = pose.position[2];
            cam[7] = eyesOk ? 1.0f : 0.0f;
            if (eyesOk) {
                for (int e = 0; e < 2; ++e)
                    for (int k = 0; k < 3; ++k)
                        cam[8 + 4 * e + k] = eyes[e][0] * pose.rows[0][k] + eyes[e][1] * pose.rows[1][k] + eyes[e][2] * pose.rows[2][k];
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
                                    cam[8], cam[9], cam[10], cam[12], cam[13], cam[14]);
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
            Log::get().note("galaxy map: the map camera's constants are bound at vertex constant slot %u for the icons, labels, rings and stems (slot held %s before; first up %.3f %.3f %.3f, eyes %s)", kCamSlot,
                            held ? "another buffer" : "nothing", cam[0], cam[1], cam[2], eyesOk ? "valid" : "not yet published: the game's eye");
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

void setGates(bool vr, bool pointer) {
    const bool wasPointer = g_pointerOpen.exchange(pointer, std::memory_order_relaxed);
    g_vrOpen.store(vr, std::memory_order_relaxed);
    if (wasPointer && !pointer) {
        g_over.store(false, std::memory_order_relaxed);
        if (g_st) g_st->resetPending.store(true, std::memory_order_release);   // the render thread lets the queries go at its next probe
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
    if (vs == kVsIconA) which = kSwapIconA;
    else if (vs == kVsIconB) which = kSwapIconB;
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
    } else if (!vrOpen()) {
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
    if (which != kSwapArrow && !bindMapCam(s, ctx)) {
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
        }
        if (!s.probeVs || !s.probePs) {
            noteUnavailable("the arrow probe's shaders");
            return;
        }
    }
    drain(s, ctx, eye, seq);
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
        vScreenPSSetShaderRaw(ctx, s.probePs.Get(), nullptr, 0);
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
}

} // namespace mapui
} // namespace edvr
