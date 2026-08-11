#include "vscreen.h"

#include <windows.h>

#include <d3d11.h>

#include <cmath>
#include <cstring>
#include <unordered_map>

#include "../common/config.h"
#include "../common/guard.h"
#include "../common/log.h"
#include "../common/vtable_hook.h"
#include "glitch_frame.h"

namespace edvr {
namespace {

// ID3D11DeviceContext vtable indices.
//
// A frozen COM ABI: IUnknown occupies 0-2, ID3D11DeviceChild 3-6, and the
// ID3D11DeviceContext methods follow in d3d11.h declaration order. None of
// these collide with the exposure fix's slots, so the two hooks coexist.
constexpr size_t kSlotVSSetConstantBuffers  = 7;
constexpr size_t kSlotDrawIndexed           = 12;
constexpr size_t kSlotDraw                  = 13;
constexpr size_t kSlotMap                   = 14;
constexpr size_t kSlotUnmap                 = 15;
constexpr size_t kSlotDrawIndexedInstanced  = 20;
constexpr size_t kSlotDrawInstanced         = 21;
constexpr size_t kSlotOMSetRenderTargets    = 33;
constexpr size_t kSlotClearRenderTargetView = 50;
constexpr size_t kHighestSlotUsed           = 50;

typedef void(STDMETHODCALLTYPE* PFN_SetConstantBuffers)(ID3D11DeviceContext*, UINT, UINT,
                                                        ID3D11Buffer* const*);
typedef void(STDMETHODCALLTYPE* PFN_Draw)(ID3D11DeviceContext*, UINT, UINT);
typedef void(STDMETHODCALLTYPE* PFN_DrawIndexed)(ID3D11DeviceContext*, UINT, UINT, INT);
typedef void(STDMETHODCALLTYPE* PFN_DrawInstanced)(ID3D11DeviceContext*, UINT, UINT, UINT,
                                                   UINT);
typedef void(STDMETHODCALLTYPE* PFN_DrawIndexedInstanced)(ID3D11DeviceContext*, UINT, UINT,
                                                          UINT, INT, UINT);
typedef HRESULT(STDMETHODCALLTYPE* PFN_Map)(ID3D11DeviceContext*, ID3D11Resource*, UINT,
                                            D3D11_MAP, UINT, D3D11_MAPPED_SUBRESOURCE*);
typedef void(STDMETHODCALLTYPE* PFN_Unmap)(ID3D11DeviceContext*, ID3D11Resource*, UINT);
typedef void(STDMETHODCALLTYPE* PFN_OMSetRenderTargets)(ID3D11DeviceContext*, UINT,
                                                        ID3D11RenderTargetView* const*,
                                                        ID3D11DepthStencilView*);
typedef void(STDMETHODCALLTYPE* PFN_ClearRtv)(ID3D11DeviceContext*,
                                              ID3D11RenderTargetView*, const FLOAT[4]);

// Most draws into eye-sized targets a frame may hold and still be the on-foot
// panel. On foot it is exactly 2, one per eye. In the cockpit the scene is
// drawn per eye and it is in the hundreds -- which is why this bound exists.
constexpr uint32_t kMaxPanelDraws = 4;

struct State {
    VTableHook hook;

    PFN_SetConstantBuffers   realVSSetConstantBuffers = nullptr;
    PFN_Draw                 realDraw = nullptr;
    PFN_DrawIndexed          realDrawIndexed = nullptr;
    PFN_DrawInstanced        realDrawInstanced = nullptr;
    PFN_DrawIndexedInstanced realDrawIndexedInstanced = nullptr;
    PFN_Map                  realMap = nullptr;
    PFN_Unmap                realUnmap = nullptr;
    PFN_OMSetRenderTargets   realOMSetRenderTargets = nullptr;
    PFN_ClearRtv             realClearRtv = nullptr;

    bool  blackVoid = true;
    bool  distanceEnabled = false;
    float distanceScale = 1.0f;
    uint32_t distanceIndex = 47;

    void* curRtv0 = nullptr;
    void* curVsCb0 = nullptr;

    // The panel's transform, as the game last wrote it. Captured from the Unmap
    // the game wrote it through, so reading it costs nothing.
    void*    compositeCb = nullptr;
    uint8_t  shadow[512] = {};
    uint32_t shadowBytes = 0;
    ID3D11Buffer* ourCb = nullptr;
    uint32_t ourCbBytes = 0;

    void*    mappedResource = nullptr;
    void*    mappedData = nullptr;
    uint32_t mappedBytes = 0;

    // A second mapped buffer, tracked only so the transition-flash detector can
    // read the camera the game wrote. Separate from the pair above because the
    // panel transform and the scene camera live in different buffers and can be
    // mapped at the same time -- sharing one slot let whichever unmapped second
    // overwrite the other's record.
    void*    camResource = nullptr;
    void*    camData = nullptr;
    uint32_t camBytes = 0;

    uint32_t eyeDrawsThisFrame = 0;
    uint32_t eyeDrawsLastFrame = 0;
    uint64_t voidClears = 0;
    uint64_t panelOverrides = 0;

    std::unordered_map<void*, bool> eyeSizedCache;
};

State* g_state = nullptr;
FaultBudget g_budget("vScreen", 5);

// Is this render target one of the two textures sent to the headset?
//
// By size. The graphics layer never sees what is submitted to the headset, and
// nothing else in an Elite frame is drawn into at 2048x2048 or larger. Cached
// per view, since it costs a resource lookup.
bool targetIsEyeSized(void* rtv) {
    State* s = g_state;
    if (!rtv) return false;
    auto it = s->eyeSizedCache.find(rtv);
    if (it != s->eyeSizedCache.end()) return it->second;

    bool out = false;
    ID3D11Resource* res = nullptr;
    static_cast<ID3D11View*>(rtv)->GetResource(&res);
    if (res) {
        D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
        res->GetType(&dim);
        if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
            D3D11_TEXTURE2D_DESC d{};
            static_cast<ID3D11Texture2D*>(res)->GetDesc(&d);
            out = d.Width >= 2048 && d.Height >= 2048;
        }
        res->Release();
    }
    // View addresses get reused, so this cannot be trusted forever. Dropping it
    // wholesale is cheap and correct.
    if (s->eyeSizedCache.size() > 512) s->eyeSizedCache.clear();
    s->eyeSizedCache[rtv] = out;
    return out;
}

// A flat, non-black grey -- what the void around the panel is cleared to.
// Matched by shape rather than by the exact value, so a game update that picks
// a different grey still matches, and one that already clears to black needs no
// help.
bool isFlatGrey(const FLOAT c[4]) {
    if (c[0] <= 0.0f || c[0] >= 0.5f) return false;
    return fabsf(c[0] - c[1]) < 1e-4f && fabsf(c[1] - c[2]) < 1e-4f;
}

// Swaps in a modified copy of the panel's transform for one draw.
//
// Nothing of the game's is written to. Its own values are copied into a buffer
// of ours with one number changed, ours is used for the draw, and the original
// is put back immediately after.
bool beginPanelOverride(ID3D11DeviceContext* self) {
    State* s = g_state;
    if (!s->distanceEnabled) return false;
    if (!targetIsEyeSized(s->curRtv0)) return false;
    ++s->eyeDrawsThisFrame;

    // An eye-sized target is not enough on its own. In the cockpit hundreds of
    // draws land in those textures and rebinding on all of them would corrupt
    // the view, so the previous frame has to have looked like a panel frame.
    if (s->eyeDrawsLastFrame == 0 || s->eyeDrawsLastFrame > kMaxPanelDraws) return false;

    void* cb = s->curVsCb0;
    if (!cb) return false;
    if (!s->compositeCb) {
        // Learn it now; its contents arrive with the next write, so the override
        // starts a frame later rather than acting on data we do not have.
        s->compositeCb = cb;
        return false;
    }
    if (cb != s->compositeCb || s->shadowBytes == 0) return false;
    if (s->distanceIndex * 4u + 4u > s->shadowBytes) return false;

    const uint32_t bytes = s->shadowBytes;
    if (!s->ourCb || s->ourCbBytes != bytes) {
        ID3D11Device* dev = nullptr;
        self->GetDevice(&dev);
        if (!dev) return false;
        if (s->ourCb) { s->ourCb->Release(); s->ourCb = nullptr; }
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = bytes;
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        const HRESULT hr = dev->CreateBuffer(&bd, nullptr, &s->ourCb);
        dev->Release();
        if (FAILED(hr) || !s->ourCb) { s->ourCb = nullptr; return false; }
        s->ourCbBytes = bytes;
    }

    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(s->realMap(self, s->ourCb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)) || !m.pData) {
        return false;
    }
    memcpy(m.pData, s->shadow, bytes);
    static_cast<float*>(m.pData)[s->distanceIndex] *= s->distanceScale;
    s->realUnmap(self, s->ourCb, 0);

    ID3D11Buffer* ours = s->ourCb;
    s->realVSSetConstantBuffers(self, 0, 1, &ours);
    if (++s->panelOverrides == 1) {
        Log::get().note("vScreen: panel distance x%.3f applied", s->distanceScale);
    }
    return true;
}

void endPanelOverride(ID3D11DeviceContext* self) {
    State* s = g_state;
    ID3D11Buffer* orig = static_cast<ID3D11Buffer*>(s->compositeCb);
    s->realVSSetConstantBuffers(self, 0, 1, &orig);
}

// --- hooks ------------------------------------------------------------------

void STDMETHODCALLTYPE hookedClearRtv(ID3D11DeviceContext* self,
                                      ID3D11RenderTargetView* rtv, const FLOAT c[4]) {
    State* s = g_state;
    // Cheap test first: four float compares, and only a match pays to resolve
    // the target.
    if (s->blackVoid && rtv && c && isFlatGrey(c) && targetIsEyeSized(rtv)) {
        const FLOAT black[4] = {0.0f, 0.0f, 0.0f, c[3]};
        if (++s->voidClears == 1) {
            Log::get().note("vScreen: void %.3f,%.3f,%.3f forced to black",
                            c[0], c[1], c[2]);
        }
        s->realClearRtv(self, rtv, black);
        return;
    }
    s->realClearRtv(self, rtv, c);
}

void STDMETHODCALLTYPE hookedOMSetRenderTargets(ID3D11DeviceContext* self, UINT n,
                                                ID3D11RenderTargetView* const* rtvs,
                                                ID3D11DepthStencilView* dsv) {
    g_state->curRtv0 = (n && rtvs) ? rtvs[0] : nullptr;
    g_state->realOMSetRenderTargets(self, n, rtvs, dsv);
}

void STDMETHODCALLTYPE hookedVSSetConstantBuffers(ID3D11DeviceContext* self, UINT start,
                                                  UINT n, ID3D11Buffer* const* bufs) {
    if (start == 0 && n && bufs) g_state->curVsCb0 = bufs[0];
    g_state->realVSSetConstantBuffers(self, start, n, bufs);
}

HRESULT STDMETHODCALLTYPE hookedMap(ID3D11DeviceContext* self, ID3D11Resource* res,
                                    UINT sub, D3D11_MAP type, UINT flags,
                                    D3D11_MAPPED_SUBRESOURCE* mapped) {
    State* s = g_state;
    const HRESULT hr = s->realMap(self, res, sub, type, flags, mapped);
    // Only the one buffer we care about, so this is a pointer compare on a very
    // hot path and nothing more.
    if (SUCCEEDED(hr) && mapped && sub == 0 && res == s->compositeCb) {
        D3D11_BUFFER_DESC d{};
        static_cast<ID3D11Buffer*>(res)->GetDesc(&d);
        if (d.ByteWidth <= sizeof(s->shadow)) {
            s->mappedResource = res;
            s->mappedData = mapped->pData;
            s->mappedBytes = d.ByteWidth;
        }
    } else if (SUCCEEDED(hr) && mapped && sub == 0 && res) {
        // The scene camera buffer, for the transition-flash detector, recognised
        // by size.
        //
        // GetType FIRST. Map is called on textures as well as buffers, and
        // ID3D11Buffer::GetDesc and ID3D11Texture2D::GetDesc occupy the same
        // vtable slot on their respective interfaces -- so calling the buffer
        // one on a texture writes a 44-byte texture description into the
        // 20-byte buffer description below. That is a stack smash, and it
        // brought the whole process down on the first frame.
        D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
        res->GetType(&dim);
        if (dim == D3D11_RESOURCE_DIMENSION_BUFFER) {
            D3D11_BUFFER_DESC d{};
            static_cast<ID3D11Buffer*>(res)->GetDesc(&d);
            if (glitchFrameWantsBuffer(d.ByteWidth)) {
                s->camResource = res;
                s->camData = mapped->pData;
                s->camBytes = d.ByteWidth;
            }
        }
    }
    return hr;
}

void STDMETHODCALLTYPE hookedUnmap(ID3D11DeviceContext* self, ID3D11Resource* res,
                                   UINT sub) {
    State* s = g_state;
    // Read before forwarding: after the real Unmap the memory is no longer ours
    // to look at.
    if (res == s->mappedResource && s->mappedData) {
        guardedBudget(g_budget, [&] {
            memcpy(s->shadow, s->mappedData, s->mappedBytes);
            s->shadowBytes = s->mappedBytes;
        });
        s->mappedResource = nullptr;
        s->mappedData = nullptr;
        s->mappedBytes = 0;
    }
    if (res == s->camResource && s->camData) {
        // Same rule as above: read before forwarding, because after the real
        // Unmap the memory is no longer ours to look at.
        guardedBudget(g_budget, [&] { glitchFrameObserve(s->camData, s->camBytes); });
        s->camResource = nullptr;
        s->camData = nullptr;
        s->camBytes = 0;
    }
    s->realUnmap(self, res, sub);
}

void STDMETHODCALLTYPE hookedDraw(ID3D11DeviceContext* self, UINT count, UINT start) {
    const bool on = beginPanelOverride(self);
    g_state->realDraw(self, count, start);
    if (on) endPanelOverride(self);
}
void STDMETHODCALLTYPE hookedDrawIndexed(ID3D11DeviceContext* self, UINT count,
                                         UINT startIndex, INT baseVertex) {
    const bool on = beginPanelOverride(self);
    g_state->realDrawIndexed(self, count, startIndex, baseVertex);
    if (on) endPanelOverride(self);
}
void STDMETHODCALLTYPE hookedDrawInstanced(ID3D11DeviceContext* self, UINT perInstance,
                                           UINT instances, UINT startVertex,
                                           UINT startInstance) {
    const bool on = beginPanelOverride(self);
    g_state->realDrawInstanced(self, perInstance, instances, startVertex, startInstance);
    if (on) endPanelOverride(self);
}
void STDMETHODCALLTYPE hookedDrawIndexedInstanced(ID3D11DeviceContext* self,
                                                  UINT perInstance, UINT instances,
                                                  UINT startIndex, INT baseVertex,
                                                  UINT startInstance) {
    const bool on = beginPanelOverride(self);
    g_state->realDrawIndexedInstanced(self, perInstance, instances, startIndex, baseVertex,
                                      startInstance);
    if (on) endPanelOverride(self);
}

}  // namespace

void vScreenFrameBoundary() {
    State* s = g_state;
    if (!s) return;
    // Carried to the next frame: a draw cannot know how many more will follow
    // it. The render mode rarely changes between consecutive frames, and when it
    // does the override skips one.
    s->eyeDrawsLastFrame = s->eyeDrawsThisFrame;
    // The flash detector needs the count for the frame that just ended, to tell
    // a rendered scene from a menu. It has to be told before the counter resets.
    glitchFrameBoundary(s->eyeDrawsLastFrame);
    s->eyeDrawsThisFrame = 0;
}

void installVScreenFixes(ID3D11Device* device) {
    if (!device || g_state) return;

    Config& cfg = Config::get();
    const bool wantVoid = cfg.getBool("fix.black_void", true);
    const float scale = cfg.getFloat("fix.panel_distance", 1.0f);
    if (!wantVoid && scale == 1.0f) return;   // nothing asked for

    ID3D11DeviceContext* ctx = nullptr;
    device->GetImmediateContext(&ctx);
    if (!ctx) return;

    g_state = new State();
    g_state->blackVoid = wantVoid;
    g_state->distanceScale = scale;
    g_state->distanceEnabled = scale != 1.0f;
    g_state->distanceIndex =
        static_cast<uint32_t>(cfg.getInt("advanced.panel_distance_index", 47));

    State& s = *g_state;
    if (!s.hook.attach(ctx) || s.hook.executablePrefix() <= kHighestSlotUsed) {
        Log::get().note("vScreen: context vtable unusable; not installing");
        s.hook.uninstall();
        ctx->Release();
        return;
    }

    s.hook.replace(kSlotClearRenderTargetView, &hookedClearRtv,
                   reinterpret_cast<void**>(&s.realClearRtv));
    s.hook.replace(kSlotOMSetRenderTargets, &hookedOMSetRenderTargets,
                   reinterpret_cast<void**>(&s.realOMSetRenderTargets));
    s.hook.replace(kSlotVSSetConstantBuffers, &hookedVSSetConstantBuffers,
                   reinterpret_cast<void**>(&s.realVSSetConstantBuffers));
    s.hook.replace(kSlotMap, &hookedMap, reinterpret_cast<void**>(&s.realMap));
    s.hook.replace(kSlotUnmap, &hookedUnmap, reinterpret_cast<void**>(&s.realUnmap));
    s.hook.replace(kSlotDraw, &hookedDraw, reinterpret_cast<void**>(&s.realDraw));
    s.hook.replace(kSlotDrawIndexed, &hookedDrawIndexed,
                   reinterpret_cast<void**>(&s.realDrawIndexed));
    s.hook.replace(kSlotDrawInstanced, &hookedDrawInstanced,
                   reinterpret_cast<void**>(&s.realDrawInstanced));
    s.hook.replace(kSlotDrawIndexedInstanced, &hookedDrawIndexedInstanced,
                   reinterpret_cast<void**>(&s.realDrawIndexedInstanced));

    if (!s.hook.commit()) {
        Log::get().note("vScreen: vtable commit failed; not installing");
        s.hook.uninstall();
        ctx->Release();
        return;
    }

    Log::get().note("vScreen fixes installed: black void %s, panel distance %s",
                    s.blackVoid ? "on" : "off",
                    s.distanceEnabled ? "on" : "off (1.0)");
    ctx->Release();
}

void shutdownVScreenFixes() {
    if (!g_state) return;
    g_state->distanceEnabled = false;
    if (g_state->ourCb) {
        g_state->ourCb->Release();
        g_state->ourCb = nullptr;
    }
    g_state->hook.uninstall();
}

}  // namespace edvr
