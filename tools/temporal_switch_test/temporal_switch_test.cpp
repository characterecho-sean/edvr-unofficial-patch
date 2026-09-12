#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstdlib>
#include <vector>
using Microsoft::WRL::ComPtr;
void require(bool ok,const char* why){if(!ok){std::printf("FAIL: %s\n",why);std::exit(1);}}
int wmain(int argc,wchar_t** argv){
    require(argc==2,"DLL path");HMODULE module=LoadLibraryW(argv[1]);require(module!=nullptr,"DLL loads");
    auto create=reinterpret_cast<decltype(&D3D11CreateDevice)>(GetProcAddress(module,"D3D11CreateDevice"));
    using Taa=void*(*)(void*,int,const float*,const float*,const float*,float,float,const float*,const float*,const float*,float,float,float,int,float,float,unsigned,unsigned,unsigned);
    auto taa=reinterpret_cast<Taa>(GetProcAddress(module,"edvrTemporalAa"));require(create&&taa,"exports");
    ComPtr<ID3D11Device> dev;ComPtr<ID3D11DeviceContext> ctx;D3D_FEATURE_LEVEL level;
    require(SUCCEEDED(create(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&dev,&level,&ctx)),"hardware device");

    const int modes[]={2,0,2,1,0,3,2,0}; // DLSS, native, DLAA, refused format
    const float tangent[4]={-1,1,-1,1},identity[9]={1,0,0,0,1,0,0,0,1};
    for(int step=0;step<8;++step){
        const int mode=modes[step];const UINT w=step<5?400:400+step*17,h=step<5?304:304+step*9;
        D3D11_TEXTURE2D_DESC td{};td.Width=w;td.Height=h;td.MipLevels=td.ArraySize=1;
        td.Format=mode==3?DXGI_FORMAT_B8G8R8A8_UNORM:DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count=1;td.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS;
        std::vector<unsigned> pixels(w*h,0xffcc9966);D3D11_SUBRESOURCE_DATA data{pixels.data(),w*4,0};
        ComPtr<ID3D11Texture2D> source,sentinel;
        require(SUCCEEDED(dev->CreateTexture2D(&td,&data,&source)),"source");
        td.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
        require(SUCCEEDED(dev->CreateTexture2D(&td,&data,&sentinel)),"sentinel");
        ComPtr<ID3D11ShaderResourceView> srv;ComPtr<ID3D11UnorderedAccessView> uav;
        require(SUCCEEDED(dev->CreateShaderResourceView(source.Get(),nullptr,&srv)),"source view");
        require(SUCCEEDED(dev->CreateUnorderedAccessView(sentinel.Get(),nullptr,&uav)),"sentinel view");
        for(int frame=0;frame<4;++frame)for(int eye=0;eye<2;++eye){
            ctx->CSSetShaderResources(8,1,srv.GetAddressOf());ctx->CSSetUnorderedAccessViews(6,1,uav.GetAddressOf(),nullptr);
            const UINT ow=mode==2?w*3/2:w,oh=mode==2?h*3/2:h;
            auto* output=static_cast<ID3D11Texture2D*>(taa(source.Get(),eye,nullptr,tangent,tangent,0,0,
                identity,nullptr,nullptr,0,0,0,1,.9f,1,ow,oh,(frame==0?1u:0u)|(mode?2u:0u)));
            require(output&&output!=source.Get(),"owned treated/fallback output");
            D3D11_TEXTURE2D_DESC actual{};output->GetDesc(&actual);
            require(actual.Width==ow&&actual.Height==oh,"native/DLAA/DLSS/fallback resize dimensions");
            require(actual.Format== (mode==3?DXGI_FORMAT_B8G8R8A8_UNORM:DXGI_FORMAT_R8G8B8A8_UNORM),"output format preserved");
            ComPtr<ID3D11ShaderResourceView> afterSrv;ComPtr<ID3D11UnorderedAccessView> afterUav;
            ctx->CSGetShaderResources(8,1,&afterSrv);ctx->CSGetUnorderedAccessViews(6,1,&afterUav);
            require(afterSrv.Get()==srv.Get()&&afterUav.Get()==uav.Get(),"caller t8/u6 restored");
        }
        ctx->ClearState();std::printf("PASS: step %d mode %d, resized %ux%u, 8 eye evaluations.\n",step,mode,w,h);
    }
    std::puts("PASS: 64 eye evaluations across native/DLAA/DLSS switches, resizing and format fallback.");
}
