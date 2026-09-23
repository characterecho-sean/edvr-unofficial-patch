#pragma once
#include <cstdint>
struct ID3D11DeviceContext;
namespace edvr {
class Config;
void nightVisionConfigure(Config&);
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
