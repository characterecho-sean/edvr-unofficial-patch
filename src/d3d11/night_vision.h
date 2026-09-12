#pragma once
#include <cstdint>
struct ID3D11DeviceContext;
namespace edvr {
class Config;
void nightVisionConfigure(Config&);
bool nightVisionMatches(char kind,uint32_t count,uint32_t instances);
void nightVisionBegin(ID3D11DeviceContext*);
void nightVisionEnd(ID3D11DeviceContext*);
void nightVisionShutdown();
}
