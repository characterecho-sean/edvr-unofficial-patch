#pragma once
#include <cstdint>
struct ID3D11DeviceContext;
namespace edvr {
class Config;
void nightVisionConfigure(Config&);
// Is the fix configured to act at all: fix.night_vision_stability or
// experimental.night_vision_realistic on, the two switches nightVisionConfigure
// reads (experimental.night_vision_brightness only scales the second). The
// draw gate (draw_gate.h) asks this once a frame, and has to: the draw
// nightVisionMatches recognises is met below that gate in beginPanelOverride,
// so a fix left out of the gate's list is starved of every draw whenever no
// other subscriber is on. Set by nightVisionConfigure from the same variant()
// nightVisionMatches tests first, so the two cannot drift. "Configured", not
// "working": a replacement shader that failed to build still answers true,
// which only keeps the gate open for a match that declines. Inline with its
// state, as wake_pulse.h's wakePulseWantsDraws is.
namespace detail{extern bool g_nightVisionOn;}
inline bool nightVisionWantsDraws(){return detail::g_nightVisionOn;}
// The draw shape nightVisionMatches requires (a 240-index single-instance
// DrawIndexedInstanced), inline so the draw path asks it before the call:
// nightVisionMatches is pure, and was a cross-TU call per eye draw that
// failed on this test (29 innermost samples of the 1355-frame parked-5
// window). nightVisionMatches itself asks this same function, so the two
// cannot drift.
inline bool nightVisionShape(char kind,uint32_t count,uint32_t instances){
    return kind=='X' && count==240 && instances==1;
}
bool nightVisionMatches(char kind,uint32_t count,uint32_t instances);
void nightVisionBegin(ID3D11DeviceContext*);
void nightVisionEnd(ID3D11DeviceContext*);
void nightVisionShutdown();
}
