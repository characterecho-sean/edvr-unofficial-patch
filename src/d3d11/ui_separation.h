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
