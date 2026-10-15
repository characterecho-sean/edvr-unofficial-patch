#include "remlok_fix.h"

#include <windows.h>

#include <d3d11.h>

#include <cmath>

#include "../common/config.h"
#include "../common/frame_flag.h"
#include "../common/log.h"
#include "binding_shadow.h"
#include "native_on_foot.h"   // F25: nativeOnFootActive for the on-foot visor match
#include "dxbc_container.h"   // F27: the visor variant's checksum (ComputeHashRetail)
#include "ui_depth.h"         // F27: uiDepthEyeOfTargetReadOnly, the bound target's eye
#include <cstring>
#include <string>
#include <vector>

namespace edvr {

// remlokWantsDraws reads this from the header with no call: asked per
// eye draw, and the build has no /GL to fold a cross-TU getter.
// RemlokMode itself is declared in the header, so the inline function
// there can name its kStock value.
namespace detail {
RemlokMode g_remlokMode = RemlokMode::kStock;
}  // namespace detail

namespace {
using Mode = detail::RemlokMode;

// The overlay's shape, exactly as the census measured and the field
// suppression verified it (2026-08-19): a fullscreen triangle, one instance,
// no depth view bound, PS slot 0 sampling the 1024x512 overlay image. The
// size is the discriminating half -- the tonemap chain is made of otherwise
// identical fullscreen triangles that sample eye-sized textures.
constexpr char     kKind = 'N';   // DrawInstanced, as the census letters it
constexpr uint32_t kVertices = 3;
constexpr uint32_t kInstances = 1;
constexpr uint32_t kSrvW = 1024;
constexpr uint32_t kSrvH = 512;
// The shape test itself is remlokOverlayShape (remlok_fix.h), inline so the
// draw path can ask it first; it must say what the three constants say.
static_assert(kKind == 'N' && kVertices == 3 && kInstances == 1,
              "remlokOverlayShape (remlok_fix.h) must match the overlay's measured shape");

float g_keep = 0.55f;
bool  g_swap = false;
// F27: the eye parity's swap with the native eye order (in native on foot the game renders the right eye first).
inline bool remlokSwapNow() { return g_swap; }   // F31: the control only (the native flip is gone; first = left everywhere)
// Scale of the substituted viewport for the overlay draw, centred. Below 1
// the whole helmet frame renders smaller, which pulls the edge lines INWARD
// from the image border -- on a wide-FOV headset the border is the lens rim,
// where the game's own placement is invisible (measured on a Pimax: the
// correctly-clipped temporal line could only be confirmed in the desktop
// mirror). 1.0 keeps the game's placement. The manual fallback; the angle
// below is the real setting.
float g_scale = 1.0f;

// Where the line should SIT, in degrees from straight ahead -- the human
// constant the scale was hiding. Tuned by eye on two headsets, the chosen
// fractions agreed on one angle to within a degree once the asymmetric
// image centre is accounted for: 0.70 of a Quest 3 (outer tangent 1.376)
// and 0.60 of a Pimax (1.529) are both the line at ~46 degrees. The scale
// that puts the line there is derived per headset from the true tangents
// the openvr half publishes; 0 turns the derivation off and g_scale rules.
float g_angleDeg = 46.0f;
bool  g_derivedNoted = false;

// Matched draws this frame: 0 is the left eye, 1 the right. Reset at the
// frame boundary so one odd frame cannot invert the pair for the session.
uint32_t g_matchesThisFrame = 0;
bool     g_pendingRight = false;

// The game's rasterizer state, cloned once with ScissorEnable flipped on,
// re-cloned only if the game ever draws the overlay with a different state.
// The original is compared by pointer and never dereferenced after Release.
ID3D11RasterizerState* g_cloneSource = nullptr;
ID3D11RasterizerState* g_clone = nullptr;
bool g_cloneSourceIsDefault = false;

// What begin set, for end to put back. engaged is the contract between the
// two: end restores exactly what begin says it changed.
bool                   g_engaged = false;
bool                   g_vpEngaged = false;
ID3D11RasterizerState* g_savedRS = nullptr;
UINT                   g_savedRectCount = 0;
D3D11_RECT             g_savedRects[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
UINT                   g_savedVpCount = 0;
D3D11_VIEWPORT         g_savedVps[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};

uint64_t g_applied = 0;
uint64_t g_hidden = 0;
bool     g_cloneFailedNoted = false;

// The scissor-enabled twin of the game's current rasterizer state. Cached
// against the source pointer; a null source means the default state, whose
// description is spelled out because there is no object to ask.
ID3D11RasterizerState* cloneWithScissor(ID3D11DeviceContext* ctx,
                                        ID3D11RasterizerState* source) {
    if (g_clone && source == g_cloneSource &&
        (source || g_cloneSourceIsDefault)) {
        return g_clone;
    }
    D3D11_RASTERIZER_DESC d{};
    if (source) {
        source->GetDesc(&d);
    } else {
        // The documented defaults of the null rasterizer state.
        d.FillMode = D3D11_FILL_SOLID;
        d.CullMode = D3D11_CULL_BACK;
        d.DepthClipEnable = TRUE;
    }
    d.ScissorEnable = TRUE;

    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    if (!dev) return nullptr;
    ID3D11RasterizerState* clone = nullptr;
    const HRESULT hr = dev->CreateRasterizerState(&d, &clone);
    dev->Release();
    if (FAILED(hr) || !clone) {
        if (!g_cloneFailedNoted) {
            g_cloneFailedNoted = true;
            Log::get().note("remlok: could not create the scissor rasterizer "
                            "state (hr=0x%08X); the overlay draws untouched.",
                            static_cast<unsigned>(hr));
        }
        return nullptr;
    }
    if (g_clone) g_clone->Release();
    g_clone = clone;
    g_cloneSource = source;
    g_cloneSourceIsDefault = source == nullptr;
    return g_clone;
}

}  // namespace

void remlokConfigure(Config& cfg) {
    const Mode was = detail::g_remlokMode;
    const std::string m = cfg.getString("fix.remlok_lines", "stock");
    if (m == "stock") {
        detail::g_remlokMode = Mode::kStock;
    } else if (m == "outer") {
        detail::g_remlokMode = Mode::kOuter;
    } else if (m == "hide") {
        detail::g_remlokMode = Mode::kHide;
    } else {
        detail::g_remlokMode = Mode::kStock;
        Log::get().note("remlok_lines \"%s\" is not stock, outer or hide; "
                        "running stock.", m.c_str());
    }

    float keep = cfg.getFloat("advanced.remlok_keep_fraction", 0.55f);
    // Under half and the two eyes' kept regions no longer meet in the
    // middle -- a gap in anything the overlay draws at centre; near 1.0 the
    // nasal line stops being clipped, which is the whole point. Clamp
    // rather than refuse: this is documented as a live-tuned value.
    if (keep < 0.50f) keep = 0.50f;
    if (keep > 0.95f) keep = 0.95f;
    g_keep = keep;
    g_swap = cfg.getBool("advanced.remlok_swap_eyes", false);

    float scale = cfg.getFloat("advanced.remlok_scale", 1.0f);
    // Below half the helmet frame shrinks into a porthole; above 1 would
    // push the lines further into the lens rim, the opposite of the knob's
    // reason to exist.
    if (scale < 0.50f) scale = 0.50f;
    if (scale > 1.00f) scale = 1.00f;
    g_scale = scale;

    float ang = cfg.getFloat("fix.remlok_line_angle", 46.0f);
    // 0 is the documented off switch; anything else is clamped into the band
    // where the line is neither in the middle of the view nor back at the
    // rim this setting exists to escape.
    if (ang != 0.0f) {
        if (ang < 20.0f) ang = 20.0f;
        if (ang > 60.0f) ang = 60.0f;
    }
    if (ang != g_angleDeg) g_derivedNoted = false;   // re-announce the derivation
    g_angleDeg = ang;

    if (was != detail::g_remlokMode) {
        const char* names[] = {"stock", "outer", "hide"};
        Log::get().note("remlok lines: %s. The overlay is recognised by shape "
                        "(3 vertices, 1 instance, no depth, 1024x512 in PS "
                        "slot 0); outer keeps %.0f%% of each eye's width from "
                        "its own temple.",
                        names[static_cast<uint32_t>(detail::g_remlokMode)], g_keep * 100.0f);
    }
}

namespace {

// The viewport scale that puts the overlay's edge line at g_angleDeg for
// THIS headset, from the true tangents the openvr half publishes -- or the
// manual g_scale when the angle is off or nobody has published (openvr half
// absent, or its hook not yet validated).
//
// The drawn image spans tangents [-outer, +inner] with outward negative, so
// its CENTRE sits (outer-inner)/2 outward of straight ahead -- a centred
// viewport scale scales about that point, not about the gaze. The formula
// is validated by the field tuning it replaced: 0.70 on a Quest 3 and 0.60
// on a Pimax both land within a degree of 46 through it.
float effectiveScale() {
    float outer = 0.0f, inner = 0.0f;
    if (g_angleDeg <= 0.0f || !eyeTangents(&outer, &inner)) return g_scale;

    const float halfSpan = (outer + inner) * 0.5f;
    const float centerOut = (outer - inner) * 0.5f;
    if (halfSpan < 1e-3f) return g_scale;

    const float target = tanf(g_angleDeg * 3.1415926535f / 180.0f);
    float s = (target - centerOut) / halfSpan;
    if (s < 0.50f) s = 0.50f;
    if (s > 1.00f) s = 1.00f;
    if (!g_derivedNoted) {
        g_derivedNoted = true;
        Log::get().note("remlok lines: this headset's outer tangent is %.3f "
                        "(%.1f deg), so the %.0f-degree line means overlay "
                        "scale %.2f. remlok_scale is not consulted while "
                        "remlok_line_angle is set.",
                        outer, atanf(outer) * 180.0f / 3.1415926535f,
                        g_angleDeg, s);
    }
    return s;
}

}  // namespace

// F25: the on-foot visor edge. census_skip of vs:B553BB479B7C0B97 removed it on foot (Sean, F24 flight). The draw is a fullscreen triangle, once per
// eye before the tonemap, with a DSV bound, PS 68ABCB9FEF6CA66C, RT 2016x2016 R11G11B10, SRV2 1024x1024 BC4 and SRV3 512x512 BC5.
constexpr uint64_t kOnFootVisorVs = 0xB553BB479B7C0B97ull;
constexpr uint64_t kOnFootVisorPs = 0x68ABCB9FEF6CA66Cull;
constexpr uint32_t kOnFootVisorRt = 2016, kOnFootVisorRtFormat = 26;   // DXGI_FORMAT_R11G11B10_FLOAT
bool onFootVisorMatches() {
    // F26: the VS alone (census_skip proved it unique to the visor edge; the flight's attribute check did not reject it on foot).
    return bindingShaderHash(BindSlot::Vs) == kOnFootVisorVs;
}

// ---- F27: the on-foot visor's per-eye pixel shader ---------------------------------------------------------------------------------------
// PS 68ABCB9FEF6CA66C draws the visor edge lines with a screen-x-keyed weight (`add_sat r2.z, |r2.z|, -0.666667`): 1 at both eye edges, 0 in the
// middle, so the lines show on both sides. The LEFT variant makes the operand -|r2.z| (weight 1 at x = 0, the temple; 0 for x >= 0.5); the RIGHT
// variant drops the modifier (weight 1 at x = 1, the temple; 0 for x <= 0.5). The change is one byte: the extended operand token's modifier at file
// offset 1724 (0x81 abs -> 0x41 neg, or -> 0x01 none). The length is unchanged, and the DXBC checksum (header bytes 4-19) is recomputed. The 20 bytes
// from offset 1720 must match first, or the variant is refused. The variant is bound by the eye identity (the bound target's eye from the eye filing,
// NOT parity); remlok_swap_eyes swaps the two as a control. Only the outer mode uses it; the draw is not scissored any more.
namespace {
constexpr uint64_t kVisorPsHash = 0x68ABCB9FEF6CA66Cull;
constexpr size_t kVisorEyeOffset = 1724;
const uint8_t kVisorEyeSeq[20] = {0x2A, 0x00, 0x10, 0x80, 0x81, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x01, 0x40, 0x00, 0x00, 0xAB, 0xAA, 0x2A, 0xBF};
std::vector<uint8_t> g_visorPsBytes;               // the game's bytecode, kept when the game creates the shader
bool g_visorPsKept = false;
ID3D11PixelShader* g_visorPsVariant[2] = {nullptr, nullptr};   // 0 left, 1 right: created once, kept for the session
bool g_visorPsTried = false;
bool g_visorPending = false;                       // set by remlokOnEyeDraw for the on-foot match in outer mode; consumed by the begin
bool g_visorActive = false;                        // a variant is bound for the draw in flight
ID3D11PixelShader* g_visorSaved = nullptr;         // the game's pixel shader, restored after the draw
uint64_t g_visorUses[2] = {0, 0};                  // per 30 s window
uint64_t g_visorRefusals = 0;
uint64_t g_visorWindowMs = 0;
}  // namespace

// Called from the device's pixel-shader creation hook: keeps the visor shader's bytecode (only that hash, once).
void remlokRememberVisorPs(uint64_t hash, const void* bytecode, size_t bytes) {
    if (hash != kVisorPsHash || g_visorPsKept || !bytecode || bytes < kVisorEyeOffset + sizeof(kVisorEyeSeq)) return;
    g_visorPsBytes.assign(static_cast<const uint8_t*>(bytecode), static_cast<const uint8_t*>(bytecode) + bytes);
    g_visorPsKept = true;
}

// The patched bytecode for one side, or why not. Verifies the sequence and the container magic before the one-byte change.
bool visorVariantBytes(bool right, std::vector<uint8_t>& out, std::string& why) {
    const std::vector<uint8_t>& in = g_visorPsBytes;
    if (in.size() < kVisorEyeOffset + sizeof(kVisorEyeSeq) || in[0] != 'D' || in[1] != 'X' || in[2] != 'B' || in[3] != 'C') {
        why = "the kept bytecode is not a DXBC container of the expected size";
        return false;
    }
    if (std::memcmp(in.data() + kVisorEyeOffset - 4, kVisorEyeSeq, sizeof(kVisorEyeSeq)) != 0 || in[kVisorEyeOffset] != 0x81) {
        why = "the token bytes at offset 1720 are not the expected sequence (the modifier at 1724 is not 0x81)";
        return false;
    }
    out = in;
    out[kVisorEyeOffset] = right ? 0x01 : 0x41;
    ComputeHashRetail(out.data() + 20, static_cast<UINT>(out.size() - 20), out.data() + 4);
    return true;
}

// The variant for a side, created on first use (both sides together). Null when refused; the refusal is logged once.
ID3D11PixelShader* visorVariant(ID3D11DeviceContext* ctx, bool right) {
    if (!g_visorPsTried) {
        g_visorPsTried = true;
        std::string why = g_visorPsKept ? "" : "the visor shader's bytecode was not seen at creation";
        ID3D11Device* dev = nullptr;
        if (why.empty()) ctx->GetDevice(&dev);
        for (int side = 0; side < 2 && why.empty(); ++side) {
            std::vector<uint8_t> bytes;
            if (!visorVariantBytes(side == 1, bytes, why)) break;
            ID3D11PixelShader* ps = nullptr;
            const HRESULT hr = dev ? dev->CreatePixelShader(bytes.data(), bytes.size(), nullptr, &ps) : E_FAIL;
            if (FAILED(hr) || !ps) {
                char t[64];
                _snprintf_s(t, _TRUNCATE, "CreatePixelShader 0x%08X", static_cast<unsigned>(hr));
                why = t;
                break;
            }
            g_visorPsVariant[side] = ps;
        }
        if (dev) dev->Release();
        if (why.empty()) {
            Log::get().note("remlok lines (on foot): patched PS created (L, R): the visor's edge weight is per eye (68ABCB9FEF6CA66C, byte 1724 %s / %s, checksum recomputed)",
                            "0x41", "0x01");
        } else {
            ++g_visorRefusals;
            Log::get().note("remlok lines (on foot): NOT patched -- %s. The visor draws unchanged this session.", why.c_str());
        }
    }
    return g_visorPsVariant[right ? 1 : 0];
}

// The begin of the on-foot visor draw (outer mode): binds the variant for this draw's eye, and keeps the game's shader for the end.
void visorSwapBegin(ID3D11DeviceContext* ctx) {
    g_visorPending = false;
    ResourceInfo rt{};
    if (!bindingResolve(bindingGet(BindSlot::Rtv0), &rt)) {
        ++g_visorRefusals;
        return;
    }
    const int eye = uiDepthEyeOfTargetReadOnly(rt.resource);   // never registers: the eye the filing gave this target (native order included)
    if (eye < 0) {
        ++g_visorRefusals;
        return;
    }
    const bool right = (eye == 1) != g_swap;                   // the control (remlok_swap_eyes) swaps the two
    ID3D11PixelShader* variant = visorVariant(ctx, right);
    if (!variant) return;
    ctx->PSGetShader(&g_visorSaved, nullptr, nullptr);         // AddRef'd: released at the end
    ctx->PSSetShader(variant, nullptr, 0);
    g_visorActive = true;
    ++g_visorUses[right ? 1 : 0];
    const uint64_t now = GetTickCount64();
    if (!g_visorWindowMs) {
        g_visorWindowMs = now;
    } else if (now - g_visorWindowMs >= 30000) {
        Log::get().note("remlok lines (on foot): visor PS variants bound in the last 30 s: left eye %llu, right eye %llu (%llu refusals); eye rule %s",
                        static_cast<unsigned long long>(g_visorUses[0]), static_cast<unsigned long long>(g_visorUses[1]),
                        static_cast<unsigned long long>(g_visorRefusals), "cockpit (first = left)");
        g_visorUses[0] = g_visorUses[1] = 0;
        g_visorWindowMs = now;
    }
}

void visorSwapEnd(ID3D11DeviceContext* ctx) {
    if (!g_visorActive) return;
    g_visorActive = false;
    ctx->PSSetShader(g_visorSaved, nullptr, 0);
    if (g_visorSaved) g_visorSaved->Release();
    g_visorSaved = nullptr;
}

// F26: the one-shot diagnostic for the on-foot visor draw (the flight: the match never fired). The first time per session, and every 30 s while
// the draw keeps coming, it logs every attribute the matcher can see and which test rejects the draw. Read only.
void remlokOnFootDiagnose(char kind, uint32_t count, uint32_t instances) {
    if (bindingShaderHash(BindSlot::Vs) != kOnFootVisorVs) return;
    static uint64_t draws = 0, lastMs = 0;
    ++draws;
    const uint64_t nowMs = GetTickCount64();
    if (lastMs && nowMs - lastMs < 30000) return;
    lastMs = nowMs;
    ResourceInfo rt{}, s2{}, s3{};
    const bool rtOk = bindingResolve(bindingGet(BindSlot::Rtv0), &rt);
    const bool s2Ok = bindingResolve(bindingGet(BindSlot::PsSrv2), &s2);
    const bool s3Ok = bindingResolve(bindingGet(BindSlot::PsSrv3), &s3);
    const bool dsv = bindingGet(BindSlot::Dsv0) != nullptr;
    const bool onFoot = nativeOnFootActive();
    const bool wanted = remlokWantsDraws();
    const bool right = (((g_matchesThisFrame & 1u) != 0u) != remlokSwapNow());
    const char* rejected = "none: the draw matches (VS and shape)";
    if (!wanted) rejected = "fix.remlok_lines is stock (remlokWantsDraws)";
    else if (kind != 'N') rejected = "indexed draw (the shape wants non-indexed)";
    else if (count != 3 || instances != 1) rejected = "not 3 vertices and 1 instance";
    Log::get().note("remlok lines (on foot): diagnostic (draw %llu with VS B553BB479B7C0B97): kind %c, %u vertices, %u instances; PS %016llX; "
                    "RTV %s %ux%u format %u; DSV %s; PS SRV2 %s %ux%u format %u; PS SRV3 %s %ux%u format %u; the eye the parity assigns: %s; "
                    "native on foot %s; fix.remlok_lines %s; rejected by: %s",
                    static_cast<unsigned long long>(draws), kind, static_cast<unsigned>(count), static_cast<unsigned>(instances),
                    static_cast<unsigned long long>(bindingShaderHash(BindSlot::Ps)),
                    rtOk ? "resolved" : "NOT resolved", static_cast<unsigned>(rt.a), static_cast<unsigned>(rt.b), static_cast<unsigned>(rt.fmt),
                    dsv ? "bound" : "not bound",
                    s2Ok ? "resolved" : "NOT resolved", static_cast<unsigned>(s2.a), static_cast<unsigned>(s2.b), static_cast<unsigned>(s2.fmt),
                    s3Ok ? "resolved" : "NOT resolved", static_cast<unsigned>(s3.a), static_cast<unsigned>(s3.b), static_cast<unsigned>(s3.fmt),
                    right ? "right" : "left", onFoot ? "now" : "not now", wanted ? "outer or hide" : "stock", rejected);
}

RemlokAction remlokOnEyeDraw(char kind, uint32_t count, uint32_t instances) {
    if (detail::g_remlokMode == Mode::kStock) return RemlokAction::kNone;
    if (!remlokOverlayShape(kind, count, instances)) {
        return RemlokAction::kNone;
    }
    const bool footVisor = onFootVisorMatches();
    if (!footVisor) {
    // The overlay binds no depth; scene and HUD draws do. Checked before the
    // SRV resolve so the resolve only runs for depthless fullscreen
    // triangles, which are a handful a frame.
    if (bindingGet(BindSlot::Dsv0) != nullptr) return RemlokAction::kNone;
    ResourceInfo info;
    if (!bindingResolve(bindingGet(BindSlot::PsSrv0), &info) ||
        !info.isTexture2D || info.a != kSrvW || info.b != kSrvH) {
        return RemlokAction::kNone;
    }
    }

    const uint32_t match = g_matchesThisFrame++;
    if (footVisor) {
        // F27: the outer mode binds the per-eye variant at the draw (visorSwapBegin); not the scissor.
        g_visorPending = detail::g_remlokMode == Mode::kOuter;
        // F27: per-eye counts over each 30 s window prove the parity (the eye rule follows the native eye order, F27).
        static uint64_t footCount = 0, footLeft = 0, footRight = 0, footLastMs = 0;
        ++footCount;
        const bool rightEye = ((match & 1u) != 0u) != remlokSwapNow();
        if (rightEye) ++footRight; else ++footLeft;
        const uint64_t nowMs = GetTickCount64();
        if (footLastMs == 0) {
            footLastMs = nowMs;
            Log::get().note("remlok lines (on foot): first match of VS B553BB479B7C0B97 (non-indexed, 3 vertices, 1 instance): the %s eye; eye rule %s",
                            rightEye ? "right" : "left", "cockpit (first = left)");
        } else if (nowMs - footLastMs >= 30000) {
            Log::get().note("remlok lines (on foot): %llu matches in the last 30 s: left eye %llu, right eye %llu (%llu this session); eye rule %s",
                            static_cast<unsigned long long>(footLeft + footRight), static_cast<unsigned long long>(footLeft),
                            static_cast<unsigned long long>(footRight), static_cast<unsigned long long>(footCount),
                            "cockpit (first = left)");
            footLastMs = nowMs;
            footLeft = footRight = 0;
        }
    }
    if (detail::g_remlokMode == Mode::kHide) {
        if (++g_hidden == 1) {
            Log::get().note("remlok lines: hidden (first overlay draw "
                            "suppressed this session).");
        }
        return RemlokAction::kHide;
    }
    g_pendingRight = ((match & 1u) != 0u) != remlokSwapNow();
    return RemlokAction::kScissor;
}

void remlokScissorBegin(ID3D11DeviceContext* ctx) {
    g_engaged = false;
    if (g_visorPending) {   // F27: the on-foot visor draw takes the per-eye variant, not the scissor
        visorSwapBegin(ctx);
        return;
    }

    ID3D11RasterizerState* current = nullptr;
    ctx->RSGetState(&current);   // AddRef'd when non-null
    ID3D11RasterizerState* scissored = cloneWithScissor(ctx, current);
    if (!scissored) {
        if (current) current->Release();
        return;   // the draw runs untouched, which stock already survives
    }

    UINT vpCount = 1;
    D3D11_VIEWPORT vp{};
    ctx->RSGetViewports(&vpCount, &vp);
    if (vpCount == 0 || vp.Width <= 0.0f) {
        if (current) current->Release();
        return;
    }

    g_savedRS = current;   // keep the reference until end restores it
    g_savedRectCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    ctx->RSGetScissorRects(&g_savedRectCount, g_savedRects);

    // The substituted viewport, when a scale applies: the same centre, a
    // fraction of the size -- derived from remlok_line_angle and this
    // headset's true tangents when the openvr half has published them,
    // remlok_scale otherwise. The fullscreen triangle renders the whole
    // overlay into it, which moves the edge lines inward from the image
    // border -- the border is the lens rim on a wide-FOV headset, and a
    // line drawn there is a line nobody sees. The undrawn margin ring is
    // territory the overlay used to cover; nothing else draws after it on
    // this target, so leaving it untouched is leaving the scene visible.
    const float scale = effectiveScale();
    g_vpEngaged = false;
    if (scale < 0.999f) {
        g_savedVpCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
        ctx->RSGetViewports(&g_savedVpCount, g_savedVps);
        D3D11_VIEWPORT scaled = vp;
        scaled.Width = vp.Width * scale;
        scaled.Height = vp.Height * scale;
        scaled.TopLeftX = vp.TopLeftX + (vp.Width - scaled.Width) * 0.5f;
        scaled.TopLeftY = vp.TopLeftY + (vp.Height - scaled.Height) * 0.5f;
        ctx->RSSetViewports(1, &scaled);
        g_vpEngaged = true;
        vp = scaled;   // the scissor below clips against what is drawn
    }

    // Each eye keeps the g_keep fraction nearest its own temple: the left
    // eye's rectangle starts at the (possibly scaled) viewport's left edge,
    // the right eye's ends at its right edge. What is clipped is the nasal
    // remainder, where the wrong-side line sat.
    const LONG x0 = static_cast<LONG>(vp.TopLeftX);
    const LONG y0 = static_cast<LONG>(vp.TopLeftY);
    const LONG w = static_cast<LONG>(vp.Width);
    const LONG h = static_cast<LONG>(vp.Height);
    const LONG keep = static_cast<LONG>(vp.Width * g_keep);
    D3D11_RECT r{};
    r.top = y0;
    r.bottom = y0 + h;
    if (g_pendingRight) {
        r.left = x0 + w - keep;
        r.right = x0 + w;
    } else {
        r.left = x0;
        r.right = x0 + keep;
    }
    ctx->RSSetScissorRects(1, &r);
    ctx->RSSetState(scissored);
    g_engaged = true;

    if (++g_applied == 1) {
        Log::get().note("remlok lines: outer mode engaged -- each eye now "
                        "keeps the line at its own temple and loses the one "
                        "along the nose. First applied to the %s eye, %ld of "
                        "%ld px kept, overlay scale %.2f.",
                        g_pendingRight ? "right" : "left", keep, w,
                        scale);
    }
}

void remlokScissorEnd(ID3D11DeviceContext* ctx) {
    if (g_visorActive) {   // F27: restores the game's pixel shader after the visor draw
        visorSwapEnd(ctx);
        return;
    }
    if (!g_engaged) return;
    g_engaged = false;
    ctx->RSSetState(g_savedRS);
    if (g_savedRS) {
        g_savedRS->Release();
        g_savedRS = nullptr;
    }
    ctx->RSSetScissorRects(g_savedRectCount,
                           g_savedRectCount ? g_savedRects : nullptr);
    if (g_vpEngaged) {
        g_vpEngaged = false;
        ctx->RSSetViewports(g_savedVpCount,
                            g_savedVpCount ? g_savedVps : nullptr);
    }
}

void remlokFrameBoundary() { g_matchesThisFrame = 0; }

void remlokShutdown() {
    if (g_clone) {
        g_clone->Release();
        g_clone = nullptr;
    }
    g_cloneSource = nullptr;
    if (g_savedRS) {
        g_savedRS->Release();
        g_savedRS = nullptr;
    }
}

}  // namespace edvr
