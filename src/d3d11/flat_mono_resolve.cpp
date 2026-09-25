#include "flat_mono_resolve.h"
#include "dlaa.h"
#include "fsr3_engine.h"
#include "temporal_shader_bytecode.h"
#include <d3d11_1.h>
#include <wrl/client.h>
#include <cmath>
#include <cstring>
#include <limits>
#include "../common/log.h"
#include "flat_pixel_capture.h"

namespace edvr {
namespace {
using Microsoft::WRL::ComPtr;
// Like renderer state, explicit owner-thread cleanup only: never release live
// driver resources from static destruction under the DLL loader lock.
FlatPixelCapture& pixels=*new FlatPixelCapture;
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
    ComPtr<ID3D11ComputeShader> prep, taa, finish, spatial;
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
FlatMonoResolveStats& stats=*new FlatMonoResolveStats;
// Session budgets survive flatMonoResolveReset: repeated backend/resource
// resets must not restart diagnostic output. Keep camera cuts independently
// visible even if ordinary requested resets use their budget first.
uint32_t resetEventsLogged=0, cameraCutEventsLogged=0;
constexpr uint32_t kResetEventLogCap=32;
struct Constants { float camera[6][4], previous[6][4]; uint32_t size[4], flags[4]; float jitter[4]; };
static_assert(sizeof(Constants)==240, "HLSL cbuffer layout");
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
    stats.currentContinueRun=0;
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
bool jitterValid(const FlatMonoResolveFrame& f) {
    return std::isfinite(f.jitterX) && std::isfinite(f.jitterY) &&
        std::isfinite(f.previousJitterX) && std::isfinite(f.previousJitterY) &&
        std::abs(f.jitterX)<=.5f && std::abs(f.jitterY)<=.5f &&
        std::abs(f.previousJitterX)<=.5f && std::abs(f.previousJitterY)<=.5f;
}
bool resolveModeValid(FlatMonoResolveMode mode) {
    return mode==FlatMonoResolveMode::Taa || mode==FlatMonoResolveMode::Dlaa ||
        mode==FlatMonoResolveMode::Dlss || mode==FlatMonoResolveMode::Fsr;
}
bool preflightMetadataValid(const FlatMonoResolvePreflight& f,const char** reason) {
    if(!f.renderWidth || !f.renderHeight || !f.outputWidth || !f.outputHeight ||
       f.renderWidth>D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
       f.renderHeight>D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
       f.outputWidth>D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
       f.outputHeight>D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION) {
        if(reason)*reason="flat-preflight-invalid-extent";
        return false;
    }
    if(!resolveModeValid(f.mode)) {
        if(reason)*reason="flat-preflight-invalid-mode";
        return false;
    }
    if(f.mode==FlatMonoResolveMode::Dlaa &&
       (f.renderWidth!=f.outputWidth || f.renderHeight!=f.outputHeight)) {
        if(reason)*reason="flat-preflight-dlaa-requires-native-render-size";
        return false;
    }
    if(f.mode!=FlatMonoResolveMode::Taa &&
       (f.renderWidth>f.outputWidth || f.renderHeight>f.outputHeight)) {
        if(reason)*reason="flat-preflight-trained-resolve-cannot-downsample";
        return false;
    }
    const bool colorFormat=f.colorViewFormat==DXGI_FORMAT_R8G8B8A8_UNORM ||
        f.colorViewFormat==DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    const bool depthFormat=f.depthViewFormat==DXGI_FORMAT_R32_FLOAT ||
        f.depthViewFormat==DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS ||
        f.depthViewFormat==DXGI_FORMAT_R24_UNORM_X8_TYPELESS ||
        f.depthViewFormat==DXGI_FORMAT_R16_UNORM;
    if(!colorFormat || !depthFormat) {
        if(reason)*reason="flat-preflight-unsupported-source-format";
        return false;
    }
    const bool colorViewRange=f.colorViewMipLevels==1 || f.colorViewMipLevels==UINT32_MAX;
    const bool depthViewRange=f.depthViewMipLevels==1 || f.depthViewMipLevels==UINT32_MAX;
    if(!f.colorViewIsTexture2D || !f.depthViewIsTexture2D ||
       f.colorMostDetailedMip!=0 || f.depthMostDetailedMip!=0 ||
       !colorViewRange || !depthViewRange || f.colorResourceMipLevels!=1 ||
       f.depthResourceMipLevels!=1 || f.colorArraySize!=1 || f.depthArraySize!=1 ||
       f.colorSampleCount!=1 || f.depthSampleCount!=1) {
        if(reason)*reason="flat-preflight-source-layout-unsupported";
        return false;
    }
    return true;
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
    if(g.device.Get()==device && g.context.Get()==context && g.prep && g.taa && g.finish && g.spatial && g.constants && g.sampler && g.isolated)return true;
    if(g.device.Get()==device && g.context && g.context.Get()!=context)++stats.contextPointerMismatches;
    ++stats.initializations;
    g=State{};
    stats.currentContinueRun=0;
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
       FAILED(device->CreateComputeShader(kFlatMonoFinishBytecode,sizeof(kFlatMonoFinishBytecode),nullptr,g.finish.GetAddressOf())) ||
       FAILED(device->CreateComputeShader(kFlatMonoSpatialBytecode,sizeof(kFlatMonoSpatialBytecode),nullptr,g.spatial.GetAddressOf())))
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
    ++stats.allocations;
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

FlatMonoResolveStats flatMonoResolveStats() { return stats; }
FlatMonoResolvePreflightResult flatMonoResolvePreflight(ID3D11Device* device,
    ID3D11DeviceContext* context,const FlatMonoResolvePreflight& planned) {
    FlatMonoResolvePreflightResult result{};
    const char* why=nullptr;
    if(!preflightMetadataValid(planned,&why)) {
        result.status=FlatMonoResolvePreflightStatus::InvalidMetadata;
        result.reason=why;
        return result;
    }
    if(!device || !context) {
        result.status=FlatMonoResolvePreflightStatus::RendererUnavailable;
        result.reason="flat-preflight-missing-device-or-context";
        return result;
    }
    if(!initialize(device,context,&why)) {
        result.status=FlatMonoResolvePreflightStatus::RendererUnavailable;
        result.reason=why?why:"flat-preflight-renderer-initialize-failed";
        return result;
    }
    result.rendererReady=true;
    FlatMonoResolveFrame frame{};
    frame.renderWidth=planned.renderWidth;frame.renderHeight=planned.renderHeight;
    frame.outputWidth=planned.outputWidth;frame.outputHeight=planned.outputHeight;
    frame.mode=planned.mode;
    if(!resources(frame,&why)) {
        result.status=FlatMonoResolvePreflightStatus::FallbackUnavailable;
        result.reason=why?why:"flat-preflight-resource-allocation-failed";
        return result;
    }
    result.spatialFallbackReady=g.spatial && g.constants && g.sampler &&
        g.output[1].texture && g.output[1].srv && g.output[1].uav &&
        g.width==planned.renderWidth && g.height==planned.renderHeight &&
        g.outWidth==planned.outputWidth && g.outHeight==planned.outputHeight &&
        g.mode==planned.mode;
    if(!result.spatialFallbackReady) {
        result.status=FlatMonoResolvePreflightStatus::FallbackUnavailable;
        result.reason="flat-preflight-spatial-output-not-ready";
        return result;
    }
    result.backendFeatureCreationDeferred=planned.mode!=FlatMonoResolveMode::Taa;
    if(planned.mode==FlatMonoResolveMode::Dlaa || planned.mode==FlatMonoResolveMode::Dlss)
        result.backendAvailable=dlaaAvailable(device,&why);
    else if(planned.mode==FlatMonoResolveMode::Fsr)
        result.backendAvailable=fsr3Available(device,&why);
    else result.backendAvailable=true;
    if(!result.backendAvailable) {
        result.status=FlatMonoResolvePreflightStatus::BackendUnavailable;
        result.reason=why?why:"flat-preflight-backend-unavailable";
        return result;
    }
    result.status=FlatMonoResolvePreflightStatus::Ready;
    result.reason=result.backendFeatureCreationDeferred?
        "ready-backend-feature-creation-deferred":"ready";
    return result;
}
void flatMonoResolveReset() { pixels.cancel();++stats.fullResets;stats.currentContinueRun=0;g=State{}; }
void flatMonoResolveArmPixels(uint64_t frame) {
    try { pixels.arm(frame); } catch(...) { pixels.cancel(); }
}
void flatMonoResolvePollPixels(ID3D11DeviceContext* context,uint64_t frame) {
    if(!pixels.active())return;
    try { pixels.poll(context,frame); } catch(...) { pixels.cancel(); }
}
void flatMonoResolveInvalidateHistory() { ++stats.invalidations;stats.currentContinueRun=0;g.history=false; }

bool flatMonoResolve(ID3D11Device* device,ID3D11DeviceContext* context,const FlatMonoResolveFrame& f,
                     ID3D11ShaderResourceView** output,const char** reason) {
    ++stats.calls;
    if(output)*output=nullptr;
    if(reason)*reason=nullptr;
    if(!output || !device || !context || !f.renderWidth || !f.renderHeight || !f.outputWidth || !f.outputHeight ||
       f.renderWidth>D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION || f.renderHeight>D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
       f.outputWidth>D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION || f.outputHeight>D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
       !std::isfinite(f.deltaMs) || f.deltaMs<0 || !cameraValid(f.camera) || !jitterValid(f))return fail(reason,"flat-resolve-invalid-frame");
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
    const bool requestedReset=f.reset, lostHistory=!g.history, frameGap=f.frame!=g.lastFrame+1;
    const bool invalidPreviousCamera=!cameraValid(f.previousCamera);
    const bool formatChange=g.inputFormat!=colorDesc.Format;
    bool cameraCut=false;
    if(!requestedReset && !lostHistory && !frameGap && !invalidPreviousCamera && !formatChange)
        for(unsigned i=0;i<3;++i)if(std::abs(f.camera[5][i]-f.previousCamera[5][i])>50)cameraCut=true;
    const bool reset=requestedReset || lostHistory || frameGap || invalidPreviousCamera || formatChange || cameraCut;
    const bool engine=f.engine.slots && f.engine.pool && f.engine.sceneNow && f.engine.scenePrev;
    if(!reset && !engine)return fail(reason,"flat-resolve-engine-source-views-unavailable");
    // All external backend work is inside the same complete state isolation.
    Isolate isolated(g.context.Get(),g.isolated.Get());
    if(f.mode==FlatMonoResolveMode::Dlaa || f.mode==FlatMonoResolveMode::Dlss) {
        if(!dlaaAvailable(device,reason)) {++stats.backendFailures;g.history=false;stats.currentContinueRun=0;return false;}
    } else if(f.mode==FlatMonoResolveMode::Fsr && !fsr3Available(device,reason)) {++stats.backendFailures;g.history=false;stats.currentContinueRun=0;return false;}
    const uint32_t index=taa?g.current:0;
    Constants constants{};std::memcpy(constants.camera,f.camera,sizeof(f.camera));
    std::memcpy(constants.previous,reset?f.camera:f.previousCamera,sizeof(f.previousCamera));
    constants.size[0]=f.renderWidth;constants.size[1]=f.renderHeight;constants.size[2]=f.outputWidth;constants.size[3]=f.outputHeight;
    constants.flags[0]=reset;constants.flags[1]=engine;constants.flags[2]=taa;
    constants.jitter[0]=f.jitterX;constants.jitter[1]=f.jitterY;
    constants.jitter[2]=f.previousJitterX;constants.jitter[3]=f.previousJitterY;
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
            g.output[0].texture.Get(),f.renderWidth,f.renderHeight,f.outputWidth,f.outputHeight,f.jitterX,f.jitterY,reset,f.deltaMs,
            f.camera[3][2],(std::numeric_limits<float>::max)(),2*std::atan(1/sy),reason,true);
    } else {
        ok=dlaaEvaluate(context,0,g.color.texture.Get(),g.depth[0].texture.Get(),g.motion.texture.Get(),g.output[0].texture.Get(),
            g.rejection.texture.Get(),f.renderWidth,f.renderHeight,f.outputWidth,f.outputHeight,f.jitterX,f.jitterY,reset,f.deltaMs,reason);
    }
    if(!ok) {++stats.backendFailures;g.history=false;stats.currentContinueRun=0;return false;}
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
    if(pixels.active() && (f.mode==FlatMonoResolveMode::Dlss || f.mode==FlatMonoResolveMode::Dlaa)) {
        ID3D11Texture2D* textures[]={g.color.texture.Get(),g.depth[0].texture.Get(),g.motion.texture.Get(),
            g.rejection.texture.Get(),g.output[0].texture.Get(),g.output[1].texture.Get()};
        try { pixels.capture(device,context,f,reset,textures); } catch(...) { pixels.cancel(); }
    }
    g.history=true;g.lastFrame=f.frame;g.current=index^1;g.inputFormat=colorDesc.Format;
    if(reset) {
        ++stats.acceptedResets;stats.currentContinueRun=0;
        if(requestedReset)++stats.requestedResets;
        if(lostHistory)++stats.lostHistory;
        if(frameGap && !lostHistory)++stats.frameGaps;
        if(invalidPreviousCamera)++stats.invalidPreviousCameras;
        if(formatChange && !lostHistory)++stats.formatChanges;
        if(cameraCut)++stats.cameraCuts;
        uint32_t& logged=cameraCut?cameraCutEventsLogged:resetEventsLogged;
        if(logged<kResetEventLogCap) {
            ++logged;
            float maxMatrixDelta=0;
            for(unsigned row=0;row<5;++row)for(unsigned col=0;col<4;++col) {
                const float delta=std::abs(f.camera[row][col]-f.previousCamera[row][col]);
                if(delta>maxMatrixDelta)maxMatrixDelta=delta;
            }
            const char* modeName=f.mode==FlatMonoResolveMode::Taa?"taa":
                f.mode==FlatMonoResolveMode::Dlaa?"dlaa":f.mode==FlatMonoResolveMode::Dlss?"dlss":"fsr";
            Log::get().note("flat resolve reset event: frame=%llu mode=%s render=%ux%u output=%ux%u "
                "requested=%u lost=%u gap=%u invalid-prev-camera=%u format=%u camera-cut=%u "
                "delta-ms=%.9g now=(%.9g,%.9g,%.9g) previous=(%.9g,%.9g,%.9g) "
                "origin-delta=(%.9g,%.9g,%.9g) max-matrix-delta=%.9g "
                "jitter-now=(%.9g,%.9g) jitter-previous=(%.9g,%.9g) event=%u/%u",
                static_cast<unsigned long long>(f.frame),modeName,f.renderWidth,f.renderHeight,f.outputWidth,f.outputHeight,
                requestedReset?1u:0u,lostHistory?1u:0u,frameGap?1u:0u,invalidPreviousCamera?1u:0u,
                formatChange?1u:0u,cameraCut?1u:0u,f.deltaMs,
                f.camera[5][0],f.camera[5][1],f.camera[5][2],
                f.previousCamera[5][0],f.previousCamera[5][1],f.previousCamera[5][2],
                f.camera[5][0]-f.previousCamera[5][0],
                f.camera[5][1]-f.previousCamera[5][1],
                f.camera[5][2]-f.previousCamera[5][2],maxMatrixDelta,
                f.jitterX,f.jitterY,f.previousJitterX,f.previousJitterY,logged,kResetEventLogCap);
        }
    } else {
        ++stats.acceptedContinues;
        if(++stats.currentContinueRun>stats.longestContinueRun)stats.longestContinueRun=stats.currentContinueRun;
    }
    *output=(colorDesc.Format==DXGI_FORMAT_R8G8B8A8_UNORM_SRGB?g.output[taa?index:1].srgb:g.output[taa?index:1].srv).Get();
    (*output)->AddRef();
    return true;
}

bool flatMonoResolveSpatialFallback(ID3D11Device* device,ID3D11DeviceContext* context,const FlatMonoResolveFrame& f,
                                    ID3D11ShaderResourceView** output,const char** reason) {
    if(output)*output=nullptr;
    if(reason)*reason=nullptr;
    g.history=false;stats.currentContinueRun=0;
    if(!output || !device || !context || !f.renderWidth || !f.renderHeight || !f.outputWidth || !f.outputHeight ||
       f.renderWidth>D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION || f.renderHeight>D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
       f.outputWidth>D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION || f.outputHeight>D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
       !jitterValid(f) || (f.mode!=FlatMonoResolveMode::Taa && f.mode!=FlatMonoResolveMode::Dlaa &&
       f.mode!=FlatMonoResolveMode::Dlss && f.mode!=FlatMonoResolveMode::Fsr))
        return fail(reason,"flat-spatial-invalid-frame");
    if(!initialize(device,context,reason))return false;
    ComPtr<ID3D11Texture2D> color;
    if(!inputTexture(f.color,f.renderWidth,f.renderHeight,true,color))return fail(reason,"flat-spatial-input-view-mismatch");
    if(!resources(f,reason))return false;
    D3D11_SHADER_RESOURCE_VIEW_DESC colorDesc{};f.color->GetDesc(&colorDesc);
    Isolate isolated(g.context.Get(),g.isolated.Get());
    context->CopyResource(g.color.texture.Get(),color.Get());
    Constants constants{};
    constants.size[0]=f.renderWidth;constants.size[1]=f.renderHeight;
    constants.size[2]=f.outputWidth;constants.size[3]=f.outputHeight;
    constants.jitter[0]=f.jitterX;constants.jitter[1]=f.jitterY;
    context->UpdateSubresource(g.constants.Get(),0,nullptr,&constants,0,0);
    ID3D11Buffer* cb=g.constants.Get();context->CSSetConstantBuffers(0,1,&cb);
    ID3D11ShaderResourceView* source=g.color.srv.Get();context->CSSetShaderResources(0,1,&source);
    ID3D11SamplerState* sampler=g.sampler.Get();context->CSSetSamplers(0,1,&sampler);
    ID3D11UnorderedAccessView* target=g.output[1].uav.Get();context->CSSetUnorderedAccessViews(4,1,&target,nullptr);
    context->CSSetShader(g.spatial.Get(),nullptr,0);
    context->Dispatch((f.outputWidth+7)/8,(f.outputHeight+7)/8,1);
    *output=(colorDesc.Format==DXGI_FORMAT_R8G8B8A8_UNORM_SRGB?g.output[1].srgb:g.output[1].srv).Get();
    (*output)->AddRef();
    return true;
}
} // namespace edvr
