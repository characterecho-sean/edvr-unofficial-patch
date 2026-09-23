// The scanned body black in one eye: the second eye's lighting resolve is
// issued with NO vertex buffer bound, and this lends it the first eye's.
//
// WHERE THIS COMES FROM (2026-09-01, the end of the black-planet hunt)
//
// A planetary body renders as a featureless black disc in the right eye in
// the DSS and FSS, on a stock game, on some machines. A fortnight of
// probes came back null -- the resolve's shader replaced with a constant,
// its depth, stencil and blend all forced off, fifteen draws skipped one
// at a time -- because every one of them modified the PIXEL pipeline, and
// no pixel ever ran.
//
// The census's vb= column found it. The deferred lighting resolve's vertex
// shader declares real vertex inputs -- v0.xyw and v1.xy, stride 20, the
// full-screen quad with per-eye ray data -- and in every failing capture
// the frame's second resolve draw arrives with NOTHING at IA slot 0:
//
//     logsK  f0  @148 vb=@151   @181 vb=-      body black in @181's eye
//     logsD  f0  @145 vb=@148   @180 vb=-      body black
//     logsP  f0  @133 vb=@212   @175 vb=-      body black    (x2 sessions)
//     logsO  c1  @117 vb=@190   @159 vb=-      body black    (c2 the same)
//     logsO  c3  @135 vb=@322   @285 vb=@322   NORMAL FLIGHT -- both fine
//
// The one healthy capture is the one where both eyes have the buffer --
// and it is the SAME buffer, which is what makes the repair exact. With no
// buffer bound the input assembler feeds zeros, the quad is degenerate,
// nothing rasterises: the draw runs with every recorded state clean and
// paints nothing. The eye's lit image then never receives its lighting;
// the scanner backdrop repaints most of the view (its own buffers are
// fine), except over the body's disc, which the backdrop is depth-excluded
// from -- a featureless black disc with the body's exact silhouette.
//
// THE REPAIR
//
// Remember the buffer, stride and offset from every resolve draw that HAS
// one (a reference is held, so the object stays valid even across a scene
// change; the next healthy sighting refreshes it). When a resolve draw
// arrives with slot 0 empty, bind the remembered buffer for that one draw
// and put the empty binding back afterwards. On a healthy rig the empty
// case never occurs and this never engages -- the observed cost is one
// IAGetVertexBuffers on resolve draws only.
//
// fix.scanner_body = on | off, default on.
#pragma once

#include <cstdint>

struct ID3D11DeviceContext;

namespace edvr {

class Config;

void resolveBindConfigure(Config& cfg);

// One bool for the draw path's early-out set.
//
// Inline: asked per eye draw, and the build has no /GL to fold a cross-TU
// getter for one bool load.
namespace detail {
extern bool g_resolveBindOn;

// The deferred resolve's PIXEL shader -- the same content hash
// resolve_probe.cpp matches on, measured 2026-08-30 from a field shader
// dump and confirmed by disassembly. Duplicated rather than shared because
// the two modules stand alone: one is a probe somebody arms for an
// evening, this is a fix that ships on.
constexpr uint64_t kResolveBindPs = 0x7CECABDE34FFBE9EULL;

// What the binding shadow's pixel shader slot says about this draw. Unknown
// when the slot holds no pointer or a zero hash -- the callee then asks the
// real context (resolve_bind_fix.cpp says why).
enum class ResolveBindShadow : uint8_t { Unknown, No, Yes };
constexpr ResolveBindShadow resolveBindShadowMatch(bool hasShader, uint64_t hash) {
    if (!hasShader || !hash) return ResolveBindShadow::Unknown;
    return hash == kResolveBindPs ? ResolveBindShadow::Yes : ResolveBindShadow::No;
}
}  // namespace detail
inline bool resolveBindWants() { return detail::g_resolveBindOn; }

// True when this eye draw is the lighting resolve (matched by PIXEL shader
// content hash, the same key resolve_probe matches on). Uses the owner
// context's binding shadow when its pointer and hash are both known; otherwise
// reads the real context and repairs that shadow slot when possible.
//
// resolveBindShadowSaysNo is that shadow answer's "No", inline: the callee
// returns false on it without touching anything, and with the fix on by
// default the call per eye draw (19 innermost samples of the 1355-frame
// parked-5 window) almost always ended there. Pass the shadow's own
// (pointer held, hash) pair.
inline bool resolveBindShadowSaysNo(bool hasShader, uint64_t hash) {
    return detail::resolveBindShadowMatch(hasShader, hash) == detail::ResolveBindShadow::No;
}
bool resolveBindOnEyeDraw(ID3D11DeviceContext* ctx);

// Around the matched draw: cache the vertex buffer if one is bound; lend
// the cached one if not. End restores the empty binding only when Begin
// lent, and must run even when Begin declined -- it does.
void resolveBindBegin(ID3D11DeviceContext* ctx);
void resolveBindEnd(ID3D11DeviceContext* ctx);

void resolveBindShutdown();

}  // namespace edvr
