#include "night_vision.h"
#include "night_vision_shader.h"
#include "../../d3d11/plugin_registry.h"
#include "temporal_shader_bytecode.h"
#include <d3d11.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include "../../d3d11/binding_shadow.h"
#include "../../d3d11/shader_swap.h"
#include "../../d3d11/vscreen.h"
#include "../../common/config.h"
#include "../../common/log.h"
#include "../../common/plugin_cost.h"
namespace edvr { namespace {
template<class T>using Ptr=Microsoft::WRL::ComPtr<T>;

// Stable first-slice coverage IDs for direct ID3D11DeviceContext calls in the
// Night Vision claim. Notes run only on the immediate render-owner thread and
// only when the API sample flag captured at Begin is set. Repeated loop
// iterations intentionally share a source site ID but each invocation
// increments the actual API-call total. This is partial call-site coverage:
// device/resource creation, helper internals, ComPtr/IUnknown, shader
// compilation, runtime/SDK, and all other unannotated paths are excluded.
enum class NvD3dCallSite : uint16_t {
    OmGetDepthStencilState = 0,
    ExteriorOmGetRenderTargets = 1,
    OmGetRenderTargetsAndUavs = 2,
    GetPredication = 3,
    ExteriorGetDevice = 4,
    CsGetShader = 5,
    CsGetShaderResources = 6,
    CsGetUnorderedAccessViews = 7,
    DispatchOmGetRenderTargets = 8,
    CsSetShaderDispatch = 9,
    CsSetShaderResourcesDispatch = 10,
    CsSetUnorderedAccessViewsDispatch = 11,
    Dispatch = 12,
    CsSetUnorderedAccessViewsClear = 13,
    CsSetShaderResourcesClear = 14,
    CsSetShaderRestore = 15,
    CsSetShaderResourcesRestore = 16,
    CsSetUnorderedAccessViewsRestore = 17,
    GetType = 18,
    PsGetCameraConstantBuffer = 19,
    PsGetSettingsConstantBuffer = 20,
    PsGetShaderResources = 21,
    RsGetViewports = 22,
    BeginOmGetRenderTargets = 23,
    OmGetBlendState = 24,
    ShaderGetDevice = 25,
    BlendGetDevice = 26,
    UpdateSubresource = 27,
    PsGetControlConstantBuffer = 28,
    PsSetControlConstantBuffer = 29,
    PsGetExteriorShaderResources = 30,
    PsSetExteriorShaderResources = 31,
    OmSetBlendState = 32,
    PsGetShader = 33,
    PsSetReplacementShader = 34,
    PsSetControlConstantBufferRestore = 35,
    PsSetExteriorShaderResourcesRestore = 36,
    OmSetBlendStateRestore = 37,
    PsSetShaderRestore = 38,
    Count = 39,
};
static_assert(static_cast<uint16_t>(NvD3dCallSite::Count) <= 128,
              "Night Vision API coverage must fit the fixed site mask");

bool enabled=false,pulseEnabled=true,configured=false;
float brightness=8.0f;
unsigned variant(){return (enabled?1u:0u)|(pulseEnabled?2u:0u);}
struct State {
    Ptr<ID3D11PixelShader> shader[4],saved;
    Ptr<ID3D11Buffer> control,savedControl;
    float uploadedBrightness=-1;
    Ptr<ID3D11ComputeShader> classify;
    Ptr<ID3D11Texture2D> exterior;
    Ptr<ID3D11ShaderResourceView> exteriorView,savedExterior;
    Ptr<ID3D11UnorderedAccessView> exteriorUav;
    struct Depth {Ptr<ID3D11Resource> resource;Ptr<ID3D11ShaderResourceView> stencil;} depths[2];
    UINT width=0,height=0,nextDepth=0;
    Ptr<ID3D11BlendState> blend,savedBlend;
    FLOAT blendFactors[4]{};
    UINT sampleMask=~0u;
    ID3D11ClassInstance* classes[256]{};
    UINT count=0;
    bool failed[4]{},noted[4]{},engaged=false,realisticDraw=false,costSample=false;
} state;

template<NvD3dCallSite Site>
inline void noteNvD3dCall(plugin_cost::ApiClass apiClass) noexcept {
    if (!state.costSample) return;
    edvrPluginCostNoteD3dCall(static_cast<uint8_t>(plugin_cost::Owner::CockpitVisuals),
                              static_cast<uint16_t>(Site),
                              static_cast<uint8_t>(apiClass));
}
bool exteriorMask(ID3D11DeviceContext* ctx,UINT w,UINT h){
    Ptr<ID3D11DepthStencilState> stencil;UINT reference;noteNvD3dCall<NvD3dCallSite::OmGetDepthStencilState>(plugin_cost::ApiClass::ReadQuery);ctx->OMGetDepthStencilState(&stencil,&reference);if(!stencil)return false;
    D3D11_DEPTH_STENCIL_DESC contract{};stencil->GetDesc(&contract);
    if(contract.DepthEnable || !contract.StencilEnable || contract.StencilReadMask!=128 || contract.StencilWriteMask!=4 || (reference&128) ||
       contract.FrontFace.StencilFunc!=D3D11_COMPARISON_EQUAL ||
       contract.FrontFace.StencilPassOp!=D3D11_STENCIL_OP_REPLACE)return false;
    Ptr<ID3D11DepthStencilView> dsv;noteNvD3dCall<NvD3dCallSite::ExteriorOmGetRenderTargets>(plugin_cost::ApiClass::ReadQuery);ctx->OMGetRenderTargets(0,nullptr,&dsv);if(!dsv)return false;
    D3D11_DEPTH_STENCIL_VIEW_DESC vd{};dsv->GetDesc(&vd);
    if(vd.Format!=DXGI_FORMAT_D32_FLOAT_S8X24_UINT || vd.ViewDimension!=D3D11_DSV_DIMENSION_TEXTURE2D || vd.Texture2D.MipSlice)return false;
    Ptr<ID3D11Resource> resource;dsv->GetResource(&resource);Ptr<ID3D11Texture2D> texture;
    if(FAILED(resource.As(&texture)))return false;D3D11_TEXTURE2D_DESC td{};texture->GetDesc(&td);
    if(td.Format!=DXGI_FORMAT_R32G8X24_TYPELESS || !(td.BindFlags&D3D11_BIND_SHADER_RESOURCE) || td.ArraySize!=1 ||
       td.SampleDesc.Count!=1 || td.Width!=w || td.Height!=h || uint64_t(w)*h>16*1024*1024)return false;
    // Do not disturb an OM UAV or predicated compute dispatch.
    ID3D11UnorderedAccessView* om[8]{};noteNvD3dCall<NvD3dCallSite::OmGetRenderTargetsAndUavs>(plugin_cost::ApiClass::ReadQuery);ctx->OMGetRenderTargetsAndUnorderedAccessViews(0,nullptr,nullptr,0,8,om);
    bool busy=false;for(auto* p:om)if(p){busy=true;p->Release();}if(busy)return false;
    Ptr<ID3D11Predicate> predicate;BOOL pred;noteNvD3dCall<NvD3dCallSite::GetPredication>(plugin_cost::ApiClass::ReadQuery);ctx->GetPredication(&predicate,&pred);if(predicate)return false;
    Ptr<ID3D11Device> dev;noteNvD3dCall<NvD3dCallSite::ExteriorGetDevice>(plugin_cost::ApiClass::ReadQuery);ctx->GetDevice(&dev);
    State::Depth* cached=nullptr;for(auto& entry:state.depths)if(entry.resource==resource)cached=&entry;
    if(!cached){
        cached=&state.depths[state.nextDepth++%2];*cached=State::Depth{};
        D3D11_SHADER_RESOURCE_VIEW_DESC sd{};sd.Format=DXGI_FORMAT_X32_TYPELESS_G8X24_UINT;
        sd.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D;sd.Texture2D.MipLevels=1;
        if(FAILED(dev->CreateShaderResourceView(resource.Get(),&sd,&cached->stencil)))return false;
        cached->resource=resource;
    }
    if(!state.classify)state.classify.Attach(shaderSwapCreateCs(ctx,kNightExteriorBytecode,sizeof(kNightExteriorBytecode),"night exterior","night vision exterior"));
    if(!state.classify){state.failed[variant()]=true;return false;}
    if(!state.exterior || state.width!=w || state.height!=h){
        state.exterior.Reset();state.exteriorView.Reset();state.exteriorUav.Reset();state.width=state.height=0;
        D3D11_TEXTURE2D_DESC out{};out.Width=w;out.Height=h;out.MipLevels=out.ArraySize=out.SampleDesc.Count=1;
        out.Format=DXGI_FORMAT_R8_UNORM;out.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS;
        if(FAILED(dev->CreateTexture2D(&out,nullptr,&state.exterior)) ||
           FAILED(dev->CreateShaderResourceView(state.exterior.Get(),nullptr,&state.exteriorView)) ||
           FAILED(dev->CreateUnorderedAccessView(state.exterior.Get(),nullptr,&state.exteriorUav)))return false;
        state.width=w;state.height=h;
    }
    Ptr<ID3D11ComputeShader> saved;ID3D11ClassInstance* classes[256]{};UINT count=256;noteNvD3dCall<NvD3dCallSite::CsGetShader>(plugin_cost::ApiClass::ReadQuery);ctx->CSGetShader(&saved,classes,&count);
    Ptr<ID3D11ShaderResourceView> srv;noteNvD3dCall<NvD3dCallSite::CsGetShaderResources>(plugin_cost::ApiClass::ReadQuery);ctx->CSGetShaderResources(0,1,&srv);
    Ptr<ID3D11UnorderedAccessView> uav;noteNvD3dCall<NvD3dCallSite::CsGetUnorderedAccessViews>(plugin_cost::ApiClass::ReadQuery);ctx->CSGetUnorderedAccessViews(0,1,&uav);
    ID3D11RenderTargetView* rt[8]{};noteNvD3dCall<NvD3dCallSite::DispatchOmGetRenderTargets>(plugin_cost::ApiClass::ReadQuery);ctx->OMGetRenderTargets(8,rt,nullptr);
    vScreenSetRenderTargetsRaw(ctx,0,nullptr,nullptr);
    noteNvD3dCall<NvD3dCallSite::CsSetShaderDispatch>(plugin_cost::ApiClass::State);ctx->CSSetShader(state.classify.Get(),nullptr,0);noteNvD3dCall<NvD3dCallSite::CsSetShaderResourcesDispatch>(plugin_cost::ApiClass::State);ctx->CSSetShaderResources(0,1,cached->stencil.GetAddressOf());
    noteNvD3dCall<NvD3dCallSite::CsSetUnorderedAccessViewsDispatch>(plugin_cost::ApiClass::State);ctx->CSSetUnorderedAccessViews(0,1,state.exteriorUav.GetAddressOf(),nullptr);noteNvD3dCall<NvD3dCallSite::Dispatch>(plugin_cost::ApiClass::Work);ctx->Dispatch((w+7)/8,(h+7)/8,1);
    ID3D11UnorderedAccessView* noneUav=nullptr;ID3D11ShaderResourceView* noneSrv=nullptr;
    noteNvD3dCall<NvD3dCallSite::CsSetUnorderedAccessViewsClear>(plugin_cost::ApiClass::State);ctx->CSSetUnorderedAccessViews(0,1,&noneUav,nullptr);noteNvD3dCall<NvD3dCallSite::CsSetShaderResourcesClear>(plugin_cost::ApiClass::State);ctx->CSSetShaderResources(0,1,&noneSrv);
    vScreenSetRenderTargetsRaw(ctx,8,rt,dsv.Get());for(auto* p:rt)if(p)p->Release();
    noteNvD3dCall<NvD3dCallSite::CsSetShaderRestore>(plugin_cost::ApiClass::State);ctx->CSSetShader(saved.Get(),classes,count);noteNvD3dCall<NvD3dCallSite::CsSetShaderResourcesRestore>(plugin_cost::ApiClass::State);ctx->CSSetShaderResources(0,1,srv.GetAddressOf());
    UINT keep=~0u;noteNvD3dCall<NvD3dCallSite::CsSetUnorderedAccessViewsRestore>(plugin_cost::ApiClass::State);ctx->CSSetUnorderedAccessViews(0,1,uav.GetAddressOf(),&keep);
    for(UINT i=0;i<count;++i)classes[i]->Release();
    return true;
}
}
namespace detail{bool g_nightVisionOn=false;}
void nightVisionConfigure(Config& cfg){
    bool pulse=cfg.getBool("fix.night_vision_stability",true);
    bool on=cfg.getBool("experimental.night_vision_realistic",false);
    float gain=cfg.getFloat("experimental.night_vision_brightness",8.0f);
    if(!std::isfinite(gain))gain=8.0f;
    gain=(std::max)(1.0f,(std::min)(gain,16.0f));
    if(!configured || on!=enabled || pulse!=pulseEnabled || gain!=brightness)
        Log::get().note("night vision: pulse stability %s; experimental Realistic nightvision %s, exterior brightness %.2fx (realistic only). AA-independent, live A/B.",pulse?"on":"off",on?"on":"off (original appearance)",gain);
    configured=true;enabled=on;pulseEnabled=pulse;brightness=gain;
    detail::g_nightVisionOn=variant()!=0;
}
bool nightVisionMatches(char kind,uint32_t count,uint32_t instances){
    constexpr const auto& claim = plugins::kManifest[plugins::kPluginCockpitVisuals]
        .claims[plugins::kClaimCockpitVisualsNightVision];
    const auto& pair = claim.shaderPairs[0];
    return variant()!=0 && !state.failed[variant()] && nightVisionShape(kind,count,instances) &&
        bindingShaderHash(BindSlot::Vs)==pair.vertexShaderHash &&
        bindingShaderHash(BindSlot::Ps)==pair.pixelShaderHash;
}
bool nightVisionClaimEligible(char kind,uint32_t count,uint32_t instances){
    (void)kind;
    (void)count;
    (void)instances;
    return variant()!=0 && !state.failed[variant()];
}
void nightVisionBegin(ID3D11DeviceContext* ctx){
    const unsigned mode=variant();
    if(!ctx || !mode || state.failed[mode] || state.engaged)return;
    if(ctx->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE)return;
    state.costSample=edvrPluginCostApiSampleContext(ctx)!=0;
    noteNvD3dCall<NvD3dCallSite::GetType>(plugin_cost::ApiClass::ReadQuery);
    // No readbacks or scene copies. Reject a
    // changed resource contract before compiling/binding the replacement.
    Ptr<ID3D11Buffer> camera,settings;noteNvD3dCall<NvD3dCallSite::PsGetCameraConstantBuffer>(plugin_cost::ApiClass::ReadQuery);ctx->PSGetConstantBuffers(1,1,&camera);noteNvD3dCall<NvD3dCallSite::PsGetSettingsConstantBuffer>(plugin_cost::ApiClass::ReadQuery);ctx->PSGetConstantBuffers(2,1,&settings);
    if(!camera || !settings)return;
    D3D11_BUFFER_DESC cd{},nd{};camera->GetDesc(&cd);settings->GetDesc(&nd);
    if(cd.ByteWidth<333*16 || nd.ByteWidth!=12*16)return;
    UINT w=0,h=0;
    for(UINT i=1;i<=2;++i){
        Ptr<ID3D11ShaderResourceView> srv;noteNvD3dCall<NvD3dCallSite::PsGetShaderResources>(plugin_cost::ApiClass::ReadQuery);ctx->PSGetShaderResources(i,1,&srv);if(!srv)return;
        D3D11_SHADER_RESOURCE_VIEW_DESC sd{};srv->GetDesc(&sd);
        if(sd.ViewDimension!=D3D11_SRV_DIMENSION_TEXTURE2D || sd.Texture2D.MostDetailedMip ||
           sd.Format!=(i==1?DXGI_FORMAT_R32_FLOAT:DXGI_FORMAT_R10G10B10A2_UNORM))return;
        Ptr<ID3D11Resource> res;srv->GetResource(&res);Ptr<ID3D11Texture2D> tex;if(FAILED(res.As(&tex)))return;
        D3D11_TEXTURE2D_DESC td{};tex->GetDesc(&td);
        if(td.ArraySize!=1 || td.SampleDesc.Count!=1)return;
        if(i==1){w=td.Width;h=td.Height;}else if(w!=td.Width || h!=td.Height)return;
    }
    D3D11_VIEWPORT vp{};UINT views=1;noteNvD3dCall<NvD3dCallSite::RsGetViewports>(plugin_cost::ApiClass::ReadQuery);ctx->RSGetViewports(&views,&vp);
    if(views!=1 || vp.TopLeftX || vp.TopLeftY || vp.Width!=w || vp.Height!=h)return;
    // Dual-source blending needs a single target 0. Refuse a changed MRT
    // or blend contract rather than sending a multiplier into another RT.
    ID3D11RenderTargetView* targets[8]{};noteNvD3dCall<NvD3dCallSite::BeginOmGetRenderTargets>(plugin_cost::ApiClass::ReadQuery);ctx->OMGetRenderTargets(8,targets,nullptr);
    bool single=targets[0]!=nullptr;
    for(UINT i=1;i<8;++i)if(targets[i])single=false;
    bool supported=false;
    if(single){
        D3D11_RENDER_TARGET_VIEW_DESC rd{};targets[0]->GetDesc(&rd);
        Ptr<ID3D11Resource> resource;targets[0]->GetResource(&resource);
        Ptr<ID3D11Texture2D> target;
        if(SUCCEEDED(resource.As(&target))){
            D3D11_TEXTURE2D_DESC td{};target->GetDesc(&td);
            supported=rd.ViewDimension==D3D11_RTV_DIMENSION_TEXTURE2D && !rd.Texture2D.MipSlice &&
                td.Width==w && td.Height==h && td.ArraySize==1 && td.SampleDesc.Count==1 &&
                (rd.Format==DXGI_FORMAT_R11G11B10_FLOAT || rd.Format==DXGI_FORMAT_R16G16B16A16_FLOAT ||
                 rd.Format==DXGI_FORMAT_R32G32B32A32_FLOAT);
        }
    }
    for(auto* target:targets)if(target)target->Release();
    if(!supported)return;
    Ptr<ID3D11BlendState> original;FLOAT factors[4];UINT mask;
    noteNvD3dCall<NvD3dCallSite::OmGetBlendState>(plugin_cost::ApiClass::ReadQuery);ctx->OMGetBlendState(&original,factors,&mask);if(!original)return;
    D3D11_BLEND_DESC bd{};original->GetDesc(&bd);const auto& rt=bd.RenderTarget[0];
    if(bd.AlphaToCoverageEnable || !rt.BlendEnable || rt.SrcBlend!=D3D11_BLEND_ONE ||
       rt.DestBlend!=D3D11_BLEND_INV_SRC_ALPHA || rt.BlendOp!=D3D11_BLEND_OP_ADD ||
       rt.RenderTargetWriteMask!=7)return;
    if(!state.shader[mode]){
        const void* bytecode = mode == 1 ? static_cast<const void*>(kNightVisionRealisticBytecode)
            : mode == 2 ? static_cast<const void*>(kNightVisionPulseBytecode)
            : static_cast<const void*>(kNightVisionRealisticPulseBytecode);
        const size_t bytecodeLen = mode == 1 ? sizeof(kNightVisionRealisticBytecode)
            : mode == 2 ? sizeof(kNightVisionPulseBytecode) : sizeof(kNightVisionRealisticPulseBytecode);
        state.shader[mode].Attach(shaderSwapCreatePs(ctx,bytecode,bytecodeLen,"night_vision","night vision"));
        if(!state.shader[mode]){state.failed[mode]=true;return;}
    }
    // Pulse-only retains the game's blend and stencil. It needs none of
    // the experimental mask, compute dispatch, brightness CB or extra SRV.
    if(enabled){
        if(!state.blend){
            Ptr<ID3D11Device> dev;noteNvD3dCall<NvD3dCallSite::ShaderGetDevice>(plugin_cost::ApiClass::ReadQuery);ctx->GetDevice(&dev);
            D3D11_BLEND_DESC replacement{};replacement.RenderTarget[0]=rt;
            replacement.RenderTarget[0].DestBlend=D3D11_BLEND_SRC1_COLOR;
            if(FAILED(dev->CreateBlendState(&replacement,&state.blend))){
                state.failed[mode]=true;Log::get().note("night vision stability: blend creation failed; retaining original draw.");return;
            }
        }
        if(!exteriorMask(ctx,w,h))return;
        if(!state.control){
            Ptr<ID3D11Device> dev;noteNvD3dCall<NvD3dCallSite::BlendGetDevice>(plugin_cost::ApiClass::ReadQuery);ctx->GetDevice(&dev);
            D3D11_BUFFER_DESC controlDesc{};controlDesc.ByteWidth=16;controlDesc.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
            if(FAILED(dev->CreateBuffer(&controlDesc,nullptr,&state.control))){
                state.failed[mode]=true;Log::get().note("night vision stability: control buffer creation failed; retaining original draw.");return;
            }
        }
        // Upload only on creation or a live setting change. Keep the game's
        // constants intact; this private slot is restored after the draw.
        if(state.uploadedBrightness!=brightness){
            const float value[4]={brightness,0,0,0};noteNvD3dCall<NvD3dCallSite::UpdateSubresource>(plugin_cost::ApiClass::Transfer);ctx->UpdateSubresource(state.control.Get(),0,nullptr,value,0,0);
            state.uploadedBrightness=brightness;
        }
        noteNvD3dCall<NvD3dCallSite::PsGetControlConstantBuffer>(plugin_cost::ApiClass::ReadQuery);ctx->PSGetConstantBuffers(3,1,&state.savedControl);noteNvD3dCall<NvD3dCallSite::PsSetControlConstantBuffer>(plugin_cost::ApiClass::State);ctx->PSSetConstantBuffers(3,1,state.control.GetAddressOf());
        noteNvD3dCall<NvD3dCallSite::PsGetExteriorShaderResources>(plugin_cost::ApiClass::ReadQuery);ctx->PSGetShaderResources(5,1,&state.savedExterior);noteNvD3dCall<NvD3dCallSite::PsSetExteriorShaderResources>(plugin_cost::ApiClass::State);ctx->PSSetShaderResources(5,1,state.exteriorView.GetAddressOf());
        state.savedBlend=original;for(UINT i=0;i<4;++i)state.blendFactors[i]=factors[i];state.sampleMask=mask;
        noteNvD3dCall<NvD3dCallSite::OmSetBlendState>(plugin_cost::ApiClass::State);ctx->OMSetBlendState(state.blend.Get(),factors,mask);
    }
    state.count=256;noteNvD3dCall<NvD3dCallSite::PsGetShader>(plugin_cost::ApiClass::ReadQuery);ctx->PSGetShader(&state.saved,state.classes,&state.count);
    noteNvD3dCall<NvD3dCallSite::PsSetReplacementShader>(plugin_cost::ApiClass::State);ctx->PSSetShader(state.shader[mode].Get(),nullptr,0);state.engaged=true;state.realisticDraw=enabled;
    if(!state.noted[mode]){state.noted[mode]=true;Log::get().note("night vision: engaged at %ux%u; pulse stability %s, appearance %s; original state restored after each matched draw.",w,h,pulseEnabled?"on":"off",enabled?"experimental realistic":"stock (no exterior-mask dispatch)");}
}
void nightVisionEnd(ID3D11DeviceContext* ctx){
    if(!state.engaged)return;
    if(state.realisticDraw){
        noteNvD3dCall<NvD3dCallSite::PsSetControlConstantBufferRestore>(plugin_cost::ApiClass::State);ctx->PSSetConstantBuffers(3,1,state.savedControl.GetAddressOf());state.savedControl.Reset();
        noteNvD3dCall<NvD3dCallSite::PsSetExteriorShaderResourcesRestore>(plugin_cost::ApiClass::State);ctx->PSSetShaderResources(5,1,state.savedExterior.GetAddressOf());state.savedExterior.Reset();
        noteNvD3dCall<NvD3dCallSite::OmSetBlendStateRestore>(plugin_cost::ApiClass::State);ctx->OMSetBlendState(state.savedBlend.Get(),state.blendFactors,state.sampleMask);state.savedBlend.Reset();
    }
    noteNvD3dCall<NvD3dCallSite::PsSetShaderRestore>(plugin_cost::ApiClass::State);ctx->PSSetShader(state.saved.Get(),state.classes,state.count);state.saved.Reset();
    for(UINT i=0;i<state.count;++i){state.classes[i]->Release();state.classes[i]=nullptr;}
    state.count=0;state.engaged=false;
    state.costSample=false;
}
void nightVisionShutdown(){state=State{};detail::g_nightVisionOn=false;}

namespace {
void pluginConfigure(void* config) {
    if (config) nightVisionConfigure(*static_cast<Config*>(config));
}
uint32_t pluginWantsDraws(void*) { return nightVisionWantsDraws() ? 1u : 0u; }
uint32_t pluginStartupHooksWanted(void* config) {
    if (!config) return 0;
    const auto& cfg = *static_cast<Config*>(config);
    return cfg.getBool("fix.night_vision_stability", true) ||
        cfg.getBool("experimental.night_vision_realistic", false) ? 1u : 0u;
}
uint32_t pluginClaimDraw(void*,const char* claimId,uint8_t kind,uint32_t count,uint32_t instances) {
    if (!claimId || std::strcmp(claimId,"night-vision") != 0) return kPluginClaimNone;
    return nightVisionClaimEligible(static_cast<char>(kind),count,instances)
        ? kPluginClaimNightVision : kPluginClaimNone;
}
uint32_t pluginClaimDrawObserved(void*, const char* claimId, uint8_t kind,
                                 uint32_t count, uint32_t instances,
                                 EdvrPluginClaimObservation* observation) {
    if (!claimId || std::strcmp(claimId, "night-vision") != 0)
        return kPluginClaimNone;
    (void)kind;
    (void)count;
    (void)instances;
    const unsigned mode = variant();
    const bool failed = mode != 0 && state.failed[mode];
    if (observation) {
        observation->mode = static_cast<uint8_t>(mode);
        observation->failedKnown = mode != 0 ? 1u : 0u;
        observation->failed = mode != 0 && failed ? 1u : 0u;
    }
    return mode != 0 && !failed ? kPluginClaimNightVision : kPluginClaimNone;
}
uint32_t pluginTraceMode(void*, uint8_t* mode) {
    if (!mode) return 0;
    *mode = static_cast<uint8_t>((enabled ? 1u : 0u) | (pulseEnabled ? 2u : 0u));
    return 1;
}
void pluginBegin(void*,const char* claimId,ID3D11DeviceContext* context) {
    if (claimId && std::strcmp(claimId,"night-vision") == 0) nightVisionBegin(context);
}
void pluginEnd(void*,const char* claimId,ID3D11DeviceContext* context) {
    if (claimId && std::strcmp(claimId,"night-vision") == 0) nightVisionEnd(context);
}
void pluginShutdown(void*) { nightVisionShutdown(); }

const char* const kCockpitVisualsClaimIds[] = {"night-vision"};
const EdvrPluginOps kCockpitVisualsOps = {
    sizeof(EdvrPluginOps),
    plugins::kPluginCockpitVisuals,
    "cockpit-visuals",
    "cockpit-visuals.night-vision",
    kCockpitVisualsClaimIds,
    1u,
    nullptr,
    &pluginConfigure,
    &pluginWantsDraws,
    &pluginStartupHooksWanted,
    &pluginClaimDraw,
    &pluginBegin,
    &pluginEnd,
    &pluginShutdown,
    &pluginClaimDrawObserved,
    &pluginTraceMode,
};
}

const EdvrPluginOps* cockpitVisualsPluginOps() { return &kCockpitVisualsOps; }
}
