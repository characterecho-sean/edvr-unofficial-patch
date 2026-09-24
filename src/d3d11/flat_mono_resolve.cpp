#include "flat_mono_resolve.h"
#include "dlaa.h"
#include "fsr3_engine.h"
#include "temporal_shader_bytecode.h"
#include <d3d11_1.h>
#include <wrl/client.h>
#include <cmath>
#include <cstring>
#include <limits>

namespace edvr {
namespace {
using Microsoft::WRL::ComPtr;
struct Image {
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11ShaderResourceView> srgb;
    ComPtr<ID3D11UnorderedAccessView> uav;
};
struct State {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext1> context;
    ComPtr<ID3DDeviceContextState> isolated;
    ComPtr<ID3D11ComputeShader> prep, taa, finish;
    ComPtr<ID3D11Buffer> constants;
    ComPtr<ID3D11SamplerState> sampler;
    Image color, depth[2], motion, rejection, expected, output[2];
    uint32_t width=0, height=0, outWidth=0, outHeight=0, current=0;
    uint64_t lastFrame=0;
    FlatMonoResolveMode mode=FlatMonoResolveMode::Taa;
    bool history=false;
    DXGI_FORMAT inputFormat=DXGI_FORMAT_UNKNOWN;
};
// Explicit owner-thread cleanup only: releasing driver objects from a static
// destructor during DLL detach would run under the loader lock.
State& g=*new State;
struct Constants { float camera[6][4], previous[6][4]; uint32_t size[4], flags[4]; };
static_assert(sizeof(Constants)==224, "HLSL cbuffer layout");
struct Isolate {
    ID3D11DeviceContext1* context;
    ComPtr<ID3DDeviceContextState> previous;
    Isolate(ID3D11DeviceContext1* c, ID3DDeviceContextState* state):context(c) {
        context->SwapDeviceContextState(state, previous.GetAddressOf());
        context->ClearState();
    }
    ~Isolate() {
        // Keep our reusable state free of resource bindings; restoring the game
        // cannot leave our UAVs aliased with its pending output-copy SRV.
        context->ClearState();
        context->SwapDeviceContextState(previous.Get(), nullptr);
    }
};
bool fail(const char** reason, const char* text) {
    g.history=false;
    if(reason)*reason=text;
    return false;
}
bool cameraValid(const float (&c)[6][4]) {
    for(const auto& row:c)for(float v:row)if(!std::isfinite(v))return false;
    if(c[0][2]!=0 || c[1][2]!=0 || c[2][2]!=0 || c[3][3]!=0 || !(c[3][2]>0))return false;
    const double ax=c[0][0],ay=c[1][0],az=c[2][0], bx=c[0][1],by=c[1][1],bz=c[2][1];
    const double cx=c[0][3],cy=c[1][3],cz=c[2][3];
    const double det=ax*(by*cz-bz*cy)+ay*(bz*cx-bx*cz)+az*(bx*cy-by*cx);
    return std::isfinite(det) && std::abs(det)>1e-8;
}
bool image(ID3D11Device* device,uint32_t width,uint32_t height,DXGI_FORMAT format,Image& out,bool writable=true) {
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width=width;desc.Height=height;desc.MipLevels=desc.ArraySize=1;desc.Format=format;
    desc.SampleDesc.Count=1;desc.Usage=D3D11_USAGE_DEFAULT;
    desc.BindFlags=D3D11_BIND_SHADER_RESOURCE|(writable?D3D11_BIND_UNORDERED_ACCESS:0);
    if(FAILED(device->CreateTexture2D(&desc,nullptr,out.texture.GetAddressOf())))return false;
    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};sd.Format=format==DXGI_FORMAT_R8G8B8A8_TYPELESS?DXGI_FORMAT_R8G8B8A8_UNORM:format;
    sd.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D;sd.Texture2D.MipLevels=1;
    D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};ud.Format=sd.Format;ud.ViewDimension=D3D11_UAV_DIMENSION_TEXTURE2D;
    if(FAILED(device->CreateShaderResourceView(out.texture.Get(),&sd,out.srv.GetAddressOf())) ||
       (writable && FAILED(device->CreateUnorderedAccessView(out.texture.Get(),&ud,out.uav.GetAddressOf()))))return false;
    if(format==DXGI_FORMAT_R8G8B8A8_TYPELESS) {
        sd.Format=DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        if(FAILED(device->CreateShaderResourceView(out.texture.Get(),&sd,out.srgb.GetAddressOf())))return false;
    }
    return true;
}
bool initialize(ID3D11Device* device,ID3D11DeviceContext* context,const char** reason) {
    if(g.device.Get()==device && g.context.Get()==context && g.prep && g.taa && g.finish && g.constants && g.sampler && g.isolated)return true;
    g=State{};
    if(context->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE)return fail(reason,"flat-resolve-requires-immediate-context");
    ComPtr<ID3D11Device> contextDevice;context->GetDevice(contextDevice.GetAddressOf());
    if(contextDevice.Get()!=device)return fail(reason,"flat-resolve-context-device-mismatch");
    ComPtr<ID3D11Device1> d1;
    if(FAILED(device->QueryInterface(IID_PPV_ARGS(d1.GetAddressOf()))) ||
       FAILED(context->QueryInterface(IID_PPV_ARGS(g.context.GetAddressOf()))))
        return fail(reason,"flat-resolve-requires-context-state-isolation");
    D3D_FEATURE_LEVEL level=device->GetFeatureLevel(),selected{};
    UINT flags=(device->GetCreationFlags()&D3D11_CREATE_DEVICE_SINGLETHREADED)?D3D11_1_CREATE_DEVICE_CONTEXT_STATE_SINGLETHREADED:0;
    if(level<D3D_FEATURE_LEVEL_11_0 || FAILED(d1->CreateDeviceContextState(flags,&level,1,D3D11_SDK_VERSION,
        __uuidof(ID3D11Device),&selected,g.isolated.GetAddressOf())))return fail(reason,"flat-resolve-context-state-create-failed");
    if(FAILED(device->CreateComputeShader(kFlatMonoPrepBytecode,sizeof(kFlatMonoPrepBytecode),nullptr,g.prep.GetAddressOf())) ||
       FAILED(device->CreateComputeShader(kFlatMonoTaaBytecode,sizeof(kFlatMonoTaaBytecode),nullptr,g.taa.GetAddressOf())) ||
       FAILED(device->CreateComputeShader(kFlatMonoFinishBytecode,sizeof(kFlatMonoFinishBytecode),nullptr,g.finish.GetAddressOf())))
        return fail(reason,"flat-resolve-shader-create-failed");
    D3D11_BUFFER_DESC cb{};cb.ByteWidth=sizeof(Constants);cb.Usage=D3D11_USAGE_DEFAULT;cb.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
    if(FAILED(device->CreateBuffer(&cb,nullptr,g.constants.GetAddressOf())))return fail(reason,"flat-resolve-constants-create-failed");
    D3D11_SAMPLER_DESC sm{};sm.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sm.AddressU=sm.AddressV=sm.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;sm.MaxLOD=D3D11_FLOAT32_MAX;
    if(FAILED(device->CreateSamplerState(&sm,g.sampler.GetAddressOf())))return fail(reason,"flat-resolve-sampler-create-failed");
    g.device=device;
    return true;
}
bool resources(const FlatMonoResolveFrame& f,const char** reason) {
    if(g.width==f.renderWidth && g.height==f.renderHeight && g.outWidth==f.outputWidth &&
       g.outHeight==f.outputHeight && g.mode==f.mode)return true;
    g.color={};g.depth[0]={};g.depth[1]={};g.motion={};g.rejection={};g.expected={};g.output[0]={};g.output[1]={};
    g.width=g.height=g.outWidth=g.outHeight=0;g.current=0;g.history=false;
    const bool taa=f.mode==FlatMonoResolveMode::Taa;
    auto make=[&](Image& out,DXGI_FORMAT format,bool output=false,bool writable=true) {
        return image(g.device.Get(),output?f.outputWidth:f.renderWidth,output?f.outputHeight:f.renderHeight,format,out,writable);
    };
    if(!make(g.color,DXGI_FORMAT_R8G8B8A8_UNORM,false,false) || !make(g.depth[0],DXGI_FORMAT_R32_FLOAT) ||
       !make(g.motion,DXGI_FORMAT_R16G16_FLOAT) || !make(g.rejection,DXGI_FORMAT_R8_UNORM) ||
       !make(g.output[0],taa?DXGI_FORMAT_R8G8B8A8_TYPELESS:DXGI_FORMAT_R8G8B8A8_UNORM,true) ||
       !make(g.output[1],DXGI_FORMAT_R8G8B8A8_TYPELESS,true) ||
       (taa && (!make(g.depth[1],DXGI_FORMAT_R32_FLOAT) || !make(g.expected,DXGI_FORMAT_R32_FLOAT))))
        return fail(reason,"flat-resolve-texture-create-failed");
    g.width=f.renderWidth;g.height=f.renderHeight;g.outWidth=f.outputWidth;g.outHeight=f.outputHeight;g.mode=f.mode;
    return true;
}
bool inputTexture(ID3D11ShaderResourceView* view,uint32_t width,uint32_t height,bool color,ComPtr<ID3D11Texture2D>& out) {
    if(!view)return false;
    D3D11_SHADER_RESOURCE_VIEW_DESC srv{};view->GetDesc(&srv);
    if(srv.ViewDimension!=D3D11_SRV_DIMENSION_TEXTURE2D || srv.Texture2D.MostDetailedMip!=0 ||
       (srv.Texture2D.MipLevels!=1 && srv.Texture2D.MipLevels!=UINT(-1)))return false;
    if(color && srv.Format!=DXGI_FORMAT_R8G8B8A8_UNORM && srv.Format!=DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)return false;
    if(!color && srv.Format!=DXGI_FORMAT_R32_FLOAT && srv.Format!=DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS &&
       srv.Format!=DXGI_FORMAT_R24_UNORM_X8_TYPELESS && srv.Format!=DXGI_FORMAT_R16_UNORM)return false;
    ComPtr<ID3D11Resource> resource;view->GetResource(resource.GetAddressOf());
    if(FAILED(resource.As(&out)))return false;
    D3D11_TEXTURE2D_DESC desc{};out->GetDesc(&desc);
    ComPtr<ID3D11Device> device;out->GetDevice(device.GetAddressOf());
    return device.Get()==g.device.Get() && desc.Width==width && desc.Height==height && desc.MipLevels==1 &&
        desc.ArraySize==1 && desc.SampleDesc.Count==1 && (desc.BindFlags&D3D11_BIND_SHADER_RESOURCE)!=0;
}
} // namespace

void flatMonoResolveReset() { g=State{}; }
void flatMonoResolveInvalidateHistory() { g.history=false; }

bool flatMonoResolve(ID3D11Device* device,ID3D11DeviceContext* context,const FlatMonoResolveFrame& f,
                     ID3D11ShaderResourceView** output,const char** reason) {
    if(output)*output=nullptr;
    if(reason)*reason=nullptr;
    if(!output || !device || !context || !f.renderWidth || !f.renderHeight || !f.outputWidth || !f.outputHeight ||
       f.renderWidth>D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION || f.renderHeight>D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
       f.outputWidth>D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION || f.outputHeight>D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
       !std::isfinite(f.deltaMs) || f.deltaMs<0 || !cameraValid(f.camera))return fail(reason,"flat-resolve-invalid-frame");
    if(f.mode!=FlatMonoResolveMode::Taa && f.mode!=FlatMonoResolveMode::Dlaa && f.mode!=FlatMonoResolveMode::Dlss &&
       f.mode!=FlatMonoResolveMode::Fsr)return fail(reason,"flat-resolve-invalid-mode");
    if(f.mode==FlatMonoResolveMode::Dlaa && (f.renderWidth!=f.outputWidth || f.renderHeight!=f.outputHeight))
        return fail(reason,"flat-dlaa-requires-native-render-size");
    if(f.mode!=FlatMonoResolveMode::Taa && (f.renderWidth>f.outputWidth || f.renderHeight>f.outputHeight))
        return fail(reason,"flat-trained-resolve-cannot-downsample");
    if(!initialize(device,context,reason))return false;
    ComPtr<ID3D11Texture2D> color,depth;
    if(!inputTexture(f.color,f.renderWidth,f.renderHeight,true,color) ||
       !inputTexture(f.depth,f.renderWidth,f.renderHeight,false,depth))return fail(reason,"flat-resolve-input-view-mismatch");
    if(!resources(f,reason))return false;
    const bool taa=f.mode==FlatMonoResolveMode::Taa;
    D3D11_SHADER_RESOURCE_VIEW_DESC colorDesc{};f.color->GetDesc(&colorDesc);
    bool reset=f.reset || !g.history || f.frame!=g.lastFrame+1 || !cameraValid(f.previousCamera) || g.inputFormat!=colorDesc.Format;
    if(!reset)for(unsigned i=0;i<3;++i)if(std::abs(f.camera[5][i]-f.previousCamera[5][i])>50)reset=true;
    const bool engine=f.engine.slots && f.engine.pool && f.engine.sceneNow && f.engine.scenePrev;
    if(!reset && !engine)return fail(reason,"flat-resolve-engine-source-views-unavailable");
    // All external backend work is inside the same complete state isolation.
    Isolate isolated(g.context.Get(),g.isolated.Get());
    if(f.mode==FlatMonoResolveMode::Dlaa || f.mode==FlatMonoResolveMode::Dlss) {
        if(!dlaaAvailable(device,reason)) {g.history=false;return false;}
    } else if(f.mode==FlatMonoResolveMode::Fsr && !fsr3Available(device,reason)) {g.history=false;return false;}
    const uint32_t index=taa?g.current:0;
    Constants constants{};std::memcpy(constants.camera,f.camera,sizeof(f.camera));
    std::memcpy(constants.previous,reset?f.camera:f.previousCamera,sizeof(f.previousCamera));
    constants.size[0]=f.renderWidth;constants.size[1]=f.renderHeight;constants.size[2]=f.outputWidth;constants.size[3]=f.outputHeight;
    constants.flags[0]=reset;constants.flags[1]=engine;constants.flags[2]=taa;
    context->UpdateSubresource(g.constants.Get(),0,nullptr,&constants,0,0);
    context->CopyResource(g.color.texture.Get(),color.Get());
    ID3D11Buffer* cb[]={g.constants.Get(),f.engine.sceneNow,f.engine.scenePrev};
    context->CSSetConstantBuffers(0,3,cb);
    ID3D11ShaderResourceView* prepViews[]={g.color.srv.Get(),f.depth,f.engine.slots,f.engine.pool};
    context->CSSetShaderResources(0,4,prepViews);
    ID3D11UnorderedAccessView* prepOutputs[]={g.depth[index].uav.Get(),g.motion.uav.Get(),g.rejection.uav.Get(),g.expected.uav.Get()};
    context->CSSetUnorderedAccessViews(0,4,prepOutputs,nullptr);
    context->CSSetShader(g.prep.Get(),nullptr,0);
    context->Dispatch((f.renderWidth+7)/8,(f.renderHeight+7)/8,1);
    ID3D11UnorderedAccessView* nullUavs[5]={};ID3D11ShaderResourceView* nullViews[9]={};
    context->CSSetUnorderedAccessViews(0,5,nullUavs,nullptr);context->CSSetShaderResources(0,9,nullViews);
    bool ok=true;
    if(taa) {
        ID3D11ShaderResourceView* views[]={g.color.srv.Get(),nullptr,nullptr,nullptr,g.motion.srv.Get(),
            g.rejection.srv.Get(),g.expected.srv.Get(),g.output[index^1].srv.Get(),g.depth[index^1].srv.Get()};
        context->CSSetShaderResources(0,9,views);
        ID3D11UnorderedAccessView* out=g.output[index].uav.Get();context->CSSetUnorderedAccessViews(4,1,&out,nullptr);
        ID3D11SamplerState* sampler=g.sampler.Get();context->CSSetSamplers(0,1,&sampler);
        context->CSSetShader(g.taa.Get(),nullptr,0);context->Dispatch((f.outputWidth+7)/8,(f.outputHeight+7)/8,1);
    } else if(f.mode==FlatMonoResolveMode::Fsr) {
        const float sy=std::sqrt(f.camera[0][1]*f.camera[0][1]+f.camera[1][1]*f.camera[1][1]+f.camera[2][1]*f.camera[2][1]);
        ok=fsr3Evaluate(context,0,g.color.texture.Get(),g.depth[0].texture.Get(),g.motion.texture.Get(),g.rejection.texture.Get(),
            g.output[0].texture.Get(),f.renderWidth,f.renderHeight,f.outputWidth,f.outputHeight,0,0,reset,f.deltaMs,
            f.camera[3][2],(std::numeric_limits<float>::max)(),2*std::atan(1/sy),reason,true);
    } else {
        ok=dlaaEvaluate(context,0,g.color.texture.Get(),g.depth[0].texture.Get(),g.motion.texture.Get(),g.output[0].texture.Get(),
            g.rejection.texture.Get(),f.renderWidth,f.renderHeight,f.outputWidth,f.outputHeight,0,0,reset,f.deltaMs,reason);
    }
    if(!ok) {g.history=false;return false;}
    if(!taa) {
        // SDKs may alter every stage. Start our final composite from the isolated
        // empty state; the outer guard still owns the untouched game's state.
        context->ClearState();
        ID3D11Buffer* cb0=g.constants.Get();context->CSSetConstantBuffers(0,1,&cb0);
        ID3D11ShaderResourceView* views[]={g.color.srv.Get(),nullptr,nullptr,nullptr,nullptr,
            g.rejection.srv.Get(),nullptr,g.output[0].srv.Get()};
        context->CSSetShaderResources(0,8,views);
        ID3D11SamplerState* sampler=g.sampler.Get();context->CSSetSamplers(0,1,&sampler);
        ID3D11UnorderedAccessView* out=g.output[1].uav.Get();context->CSSetUnorderedAccessViews(4,1,&out,nullptr);
        context->CSSetShader(g.finish.Get(),nullptr,0);context->Dispatch((f.outputWidth+7)/8,(f.outputHeight+7)/8,1);
    }
    g.history=true;g.lastFrame=f.frame;g.current=index^1;g.inputFormat=colorDesc.Format;
    *output=(colorDesc.Format==DXGI_FORMAT_R8G8B8A8_UNORM_SRGB?g.output[taa?index:1].srgb:g.output[taa?index:1].srv).Get();
    (*output)->AddRef();
    return true;
}
} // namespace edvr
