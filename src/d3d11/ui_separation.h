#pragma once
#include <cstddef>
#include <cstdint>
struct ID3D11DeviceContext;
struct ID3D11PixelShader;
struct ID3D11Resource;
struct ID3D11View;
struct ID3D11Texture2D;
struct ID3D11ShaderResourceView;
namespace edvr {
class Config;
void uiSeparationConfigure(Config&);

// Is this fix live at all? The first term of every Begin below, published so
// the draw path can decline without a call.
//
// It is bundled with fix.temporal_aa's external engines (uiSeparationConfigure
// reads temporalExternalEngine), so in a session with temporal AA off both
// Begins were cross-TU calls that only ever returned false -- once per draw
// each. uiSeparationToneBegin alone was 72 innermost samples of the
// 1349-frame window of 2026-09-22. Neither Begin touches anything before its
// first test, so declining here and declining inside are the same decline.
namespace detail {
extern bool g_uiSeparationEnabled;
extern bool g_uiSeparationFailed;
}  // namespace detail
inline bool uiSeparationLive() {
    return detail::g_uiSeparationEnabled && !detail::g_uiSeparationFailed;
}

void uiSeparationRemember(ID3D11PixelShader*,const void*,size_t,bool linked);
bool uiSeparationBegin(ID3D11DeviceContext*);
void uiSeparationEnd(ID3D11DeviceContext*);
bool uiSeparationToneBegin(ID3D11DeviceContext*,char kind,uint32_t count,uint32_t instances);
void uiSeparationToneEnd(ID3D11DeviceContext*);
void uiSeparationResourceWrite(ID3D11Resource* destination);
void uiSeparationViewWrite(ID3D11View* destination);
void uiSeparationUnknownWrite();
void uiSeparationFrameBoundary();
void uiSeparationShutdown();
struct UiSeparatedInputs {
    ID3D11Texture2D* colour=nullptr;
    ID3D11Texture2D* mask=nullptr;
    ID3D11ShaderResourceView* colourView=nullptr;
    ID3D11ShaderResourceView* influence=nullptr;
    ID3D11ShaderResourceView* depth=nullptr;
    ID3D11ShaderResourceView* holo=nullptr;
    ID3D11ShaderResourceView* edits=nullptr;
};
bool uiSeparationInputs(ID3D11Texture2D* submitted,ID3D11Texture2D* scene,
                        int eye,uint32_t w,uint32_t h,UiSeparatedInputs&);
bool uiSeparationFailed();
void uiSeparationEvaluated();
}
