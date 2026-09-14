#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <cstdio>
#include <vector>
#include <string>
#include "../../src/d3d11/dxbc_fanout.h"
#include "../../src/d3d11/ui_colour_layer.h"
#include "../../third_party/dxbc_hash/DxilHash.cpp"
using Microsoft::WRL::ComPtr;
namespace edvr {
void vScreenSetRenderTargetsRaw(ID3D11DeviceContext* c,uint32_t n,ID3D11RenderTargetView*const* r,ID3D11DepthStencilView* d){c->OMSetRenderTargets(n,r,d);}
}
#include "gpu_tests.h"
int main(int argc,char** argv){
 const bool hardware=argc>1&&strcmp(argv[1],"hardware")==0;
 ComPtr<ID3D11Device> dev;ComPtr<ID3D11DeviceContext> ctx;D3D_FEATURE_LEVEL level{};
 hr(D3D11CreateDevice(nullptr,hardware?D3D_DRIVER_TYPE_HARDWARE:D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&dev,&level,&ctx));
 auto code=compile("float4 main():SV_Target{return float4(.25,.5,.75,1.2);}","ps_5_0");
 std::vector<BYTE> fanout;std::string reason;
 check(edvr::uiColourFanout(code->GetBufferPointer(),code->GetBufferSize(),fanout,reason),reason.c_str());
 ComPtr<ID3D11PixelShader> ps;hr(dev->CreatePixelShader(fanout.data(),fanout.size(),nullptr,&ps));
 auto bad=fanout;bad[0]='X';check(!edvr::uiColourFanout(bad.data(),bad.size(),fanout,reason),"corrupt DXBC rejected");
 runGpu(dev.Get(),ctx.Get());
 printf("UI colour layer: %u checks passed on %s (feature 0x%X).\n",checks,hardware?"hardware":"WARP",unsigned(level));
 return 0;
}

