#include "ui_deferred.h"
#include "luma_probe.h"
#include "ui_deferred_draw.h"
#include "ui_deferred_depth.h"
#include "ui_deferred_shaders.h"
#include "dxbc_fanout.h"
#include "binding_shadow.h"
#include "eye_tonemap_snapshot.h"
#include "exposure_fix.h"
#include "shader_swap.h"
#include "vscreen.h"
#include "../common/config.h"
#include "../common/log.h"
#include "../common/temporal_mode.h"
#include <d3d11_1.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <vector>

namespace edvr { namespace {
template<class T> using Ptr=Microsoft::WRL::ComPtr<T>;
thread_local bool inside=false;
thread_local bool routeHandledThisDraw=false;
struct Scope { bool before=inside; Scope(){inside=true;} ~Scope(){inside=before;} };
bool enabled=false;
bool failed=false,resetPending[2]{};
constexpr uint64_t kToneVsConstantExposure=0x642017A6FEDAE0E8ull;
constexpr uint64_t kPostToneVs=0x20F383BBAC05C031ull,kPostTonePs=0xDED8796049C7BB4Aull;
constexpr uint64_t kTailVs=0xA888D51024D9798Eull,kTailPs=0x015EF9349EC097E8ull;
constexpr size_t kTailDrawLimit=8;
Ptr<ID3D11DeviceContext> recorder;
struct Surface {
    Ptr<ID3D11Texture2D> tex;
    Ptr<ID3D11ShaderResourceView> srv;
    Ptr<ID3D11RenderTargetView> rtv;
    Ptr<ID3D11UnorderedAccessView> uav;
    bool ensure(ID3D11Device* dev,UINT w,UINT h,DXGI_FORMAT format,bool compute=false,bool render=false) {
        D3D11_TEXTURE2D_DESC d{};if(tex)tex->GetDesc(&d);
        const bool needRtv=!compute || render;
        if(tex && srv && (!compute || uav) && (!needRtv || rtv) && d.Width==w && d.Height==h && d.Format==format && bool(uav)==compute)return true;
        *this={};d={};d.Width=w;d.Height=h;d.MipLevels=d.ArraySize=d.SampleDesc.Count=1;
        d.Format=format;d.BindFlags=D3D11_BIND_SHADER_RESOURCE|(compute?D3D11_BIND_UNORDERED_ACCESS:0)|(needRtv?D3D11_BIND_RENDER_TARGET:0);
        return SUCCEEDED(dev->CreateTexture2D(&d,nullptr,&tex)) &&
            SUCCEEDED(dev->CreateShaderResourceView(tex.Get(),nullptr,&srv)) &&
            (!compute || SUCCEEDED(dev->CreateUnorderedAccessView(tex.Get(),nullptr,&uav))) &&
            (!needRtv || SUCCEEDED(dev->CreateRenderTargetView(tex.Get(),nullptr,&rtv)));
    }
};
struct DepthCopy {
    Ptr<ID3D11Texture2D> before,source;
    Ptr<ID3D11ShaderResourceView> depth,stencil;
    DXGI_FORMAT format=DXGI_FORMAT_UNKNOWN;
    bool seed(ID3D11Device* dev,ID3D11DeviceContext* ctx,ID3D11DepthStencilView* view) {
        if(!view)return false;
        Ptr<ID3D11Resource> r;view->GetResource(&r);Ptr<ID3D11Texture2D> t;if(FAILED(r.As(&t)))return false;
        D3D11_TEXTURE2D_DESC d{};t->GetDesc(&d);D3D11_DEPTH_STENCIL_VIEW_DESC vd{};view->GetDesc(&vd);
        if(d.MipLevels!=1 || d.ArraySize!=1 || d.SampleDesc.Count!=1 || vd.ViewDimension!=D3D11_DSV_DIMENSION_TEXTURE2D || vd.Texture2D.MipSlice)return false;
        DXGI_FORMAT df=DXGI_FORMAT_UNKNOWN,sf=DXGI_FORMAT_UNKNOWN,tf=DXGI_FORMAT_UNKNOWN;
        switch(vd.Format) {
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:tf=DXGI_FORMAT_R32G8X24_TYPELESS;df=DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;sf=DXGI_FORMAT_X32_TYPELESS_G8X24_UINT;break;
        case DXGI_FORMAT_D24_UNORM_S8_UINT:tf=DXGI_FORMAT_R24G8_TYPELESS;df=DXGI_FORMAT_R24_UNORM_X8_TYPELESS;sf=DXGI_FORMAT_X24_TYPELESS_G8_UINT;break;
        case DXGI_FORMAT_D32_FLOAT:tf=DXGI_FORMAT_R32_TYPELESS;df=DXGI_FORMAT_R32_FLOAT;break;
        default:return false;
        }
        if(source.Get()!=t.Get() || !before || !depth || (sf!=DXGI_FORMAT_UNKNOWN && !stencil)) {
            *this={};source=t;format=vd.Format;
            d.Format=tf;d.Usage=D3D11_USAGE_DEFAULT;d.CPUAccessFlags=d.MiscFlags=0;d.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_DEPTH_STENCIL;
            vd.Flags=0;
            D3D11_SHADER_RESOURCE_VIEW_DESC sd{};sd.Format=df;sd.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D;sd.Texture2D.MipLevels=1;
            if(FAILED(dev->CreateTexture2D(&d,nullptr,&before)) || FAILED(dev->CreateShaderResourceView(before.Get(),&sd,&depth)))return false;
            if(sf!=DXGI_FORMAT_UNKNOWN){sd.Format=sf;if(FAILED(dev->CreateShaderResourceView(before.Get(),&sd,&stencil)))return false;}
        }
        ctx->CopyResource(before.Get(),t.Get());return true;
    }
};
struct Material {
    Ptr<ID3D11PixelShader> original,fanout;
    Ptr<ID3D11BlendState> originalBlend,transmission,world;
    bool worldRole=false;
};
std::vector<Material> materials;
UiDeferredSnapshots snapshots;
struct ShaderMask {
    Ptr<ID3D11VertexShader> vs;
    Ptr<ID3D11PixelShader> ps;
    UiDeferredCaptureMask mask;
    bool valid=false;
};
std::vector<ShaderMask> masks;
constexpr uint64_t kCompactPanelVs=0x81216C77F90DEDD6ull,kCompactPanelPs=0xA2965EC2931A39C8ull;
const UiDeferredCaptureMask* reflect(ID3D11DeviceContext* ctx) {
    Ptr<ID3D11VertexShader> vs;Ptr<ID3D11PixelShader> ps;ctx->VSGetShader(&vs,nullptr,nullptr);ctx->PSGetShader(&ps,nullptr,nullptr);
    if(!vs || !ps)return nullptr;
    for(auto& m:masks)if(m.vs==vs && m.ps==ps)return m.valid?&m.mask:nullptr;
    if(masks.size()>=128)return nullptr;
    ShaderMask m;m.vs=vs;m.ps=ps;
    m.valid=uiDeferredReflect(vs.Get(),m.mask.cbVs,m.mask.srvVs) && uiDeferredReflect(ps.Get(),m.mask.cbPs,m.mask.srvPs);
    const bool compactPanel=bindingShaderHash(BindSlot::Vs)==kCompactPanelVs && bindingShaderHash(BindSlot::Ps)==kCompactPanelPs;
    if(compactPanel)m.valid=m.valid && uiDeferredPanelVertexInputs(vs.Get());m.mask.compactPanelIa=compactPanel && m.valid;
    masks.push_back(std::move(m));return masks.back().valid?&masks.back().mask:nullptr;
}
struct Draw {
    UiDeferredDraw packet;
    size_t material=0;
};
struct Eye {
    Ptr<ID3D11Texture2D> hdr;
    Ptr<ID3D11RenderTargetView> hdrTarget;
    DepthCopy depth;
    Surface cleanHdr,cleanLdr,cleanFinal;
    UiDeferredDraw tone,postTone;
    std::vector<std::unique_ptr<UiDeferredDraw>> tails;
    std::vector<std::unique_ptr<Draw>> draws;
    std::vector<Ptr<ID3D11Resource>> aliases;
    Ptr<ID3D11CommandList> output;
    uint64_t generation=0;
    UINT count=0,w=0,h=0,changedStencil=0;
    size_t bytes=0;
    bool complete=false,restored=false,aborted=false;
};
Eye eyes[2];
Ptr<ID3D11BlendState> savedBlend;
Ptr<ID3D11PixelShader> savedPs;
Ptr<ID3D11RenderTargetView> savedTarget;
Ptr<ID3D11DepthStencilView> savedDepth;
Ptr<ID3D11RenderTargetView> replayTarget;
enum class WorldDrawMode : uint8_t { None, Fanout, GlassPending, GlassBound };
WorldDrawMode worldDrawMode=WorldDrawMode::None;
float savedFactors[4]{};UINT savedMask=0;bool colourMuted=false;
uint64_t captured=0,applied=0,declined=0,glassReplays=0;
unsigned glassReplayReports=0;
bool noted=false,routeNoted=false;
struct DiagnosticCounters {
    uint64_t captures=0,toneObservations=0,toneAliases=0,aliasRemovals=0,prepares=0;
    uint64_t postToneCandidates=0,lateCompositeCandidates=0,boundaries=0;
    uint64_t postToneCaptures=0,tailCaptures=0;
} diagnostic;
uint64_t generation=1;
uint64_t diagnosticUntilGeneration=0;
unsigned toneReports=0,aliasReports=0,prepareReports=0,postToneReports=0,lateCompositeReports=0,boundaryReports=0;
unsigned routeCaptureReports=0;
struct CaptureFailure { uint64_t vs=0,ps=0;std::string reason;char stage=0;int slot=-1; };
std::vector<CaptureFailure> captureFailures;
static bool diagnosticWindow(){return diagnosticUntilGeneration && generation<=diagnosticUntilGeneration;}

// The sampled blit immediately after tone mapping is a draw, not a resource
// copy, so the alias tracker cannot say what produced its t0. Keep the last
// few eye-sized LDR draw targets alive and attach draw-wrapper facts to them.
// This is a rolling diagnostic only: no alias or replay state is changed.
struct WriterTrace {
    Ptr<ID3D11Resource> target;
    Ptr<ID3D11VertexShader> entryVsObject;
    Ptr<ID3D11PixelShader> entryPsObject;
    Ptr<ID3D11VertexShader> beforeVsObject;
    Ptr<ID3D11PixelShader> beforePsObject;
    uint64_t serial=0,frame=0;
    uint64_t entryVs=0,entryPs=0,actualEntryVs=0,actualEntryPs=0;
    uint64_t beforeVs=0,beforePs=0,actualBeforeVs=0,actualBeforePs=0;
    uint32_t ordinal=0,count=0,instances=0,verdict=0;
    char kind=0;
    bool reachedBeforeTone=false,originalIssued=false;
};
constexpr size_t kWriterTraceCount=24;
std::array<WriterTrace,kWriterTraceCount> writerTrace;
size_t writerTraceNext=0;
int writerTracePending=-1;
uint64_t writerTraceSerial=0;
uint32_t writerTraceOrdinal=0;
WriterTrace lastProducer;
WriterTrace lastIssuedProducer;
uint64_t producerMatches=0,producerMisses=0,writerTargetQueries=0,writerShaderQueries=0;
struct WriterTargetCache {
    uint32_t rtvGeneration=UINT32_MAX,dsvGeneration=UINT32_MAX;
    void* shadowRtv=nullptr;
    Ptr<ID3D11Resource> resource;
    bool candidate=false;
} writerTargetCache;
std::wstring writerShaderDir;
struct WriterShaderKey { uint64_t hash=0;wchar_t stage=0; };
std::array<WriterShaderKey,8> writerShaderKeys;
unsigned writerShaderCount=0,writerShaderReports=0;
bool sameIdentity(IUnknown*,IUnknown*);

void clearWriterTrace() {
    for(auto& w:writerTrace)w=WriterTrace{};
    writerTraceNext=0;writerTracePending=-1;writerTraceOrdinal=0;
    lastProducer=WriterTrace{};lastIssuedProducer=WriterTrace{};writerTargetCache=WriterTargetCache{};
}
void actualShaderHashes(ID3D11DeviceContext* ctx,WriterTrace& w,bool before) {
    Ptr<ID3D11VertexShader> vs;Ptr<ID3D11PixelShader> ps;
    ++writerShaderQueries;
    ctx->VSGetShader(&vs,nullptr,nullptr);ctx->PSGetShader(&ps,nullptr,nullptr);
    const uint64_t vh=vs?lookupShaderHash(vs.Get()):0,ph=ps?lookupShaderHash(ps.Get()):0;
    if(before){w.beforeVs=bindingShaderHash(BindSlot::Vs);w.beforePs=bindingShaderHash(BindSlot::Ps);w.actualBeforeVs=vh;w.actualBeforePs=ph;w.beforeVsObject=vs;w.beforePsObject=ps;}
    else {w.entryVsObject=vs;w.entryPsObject=ps;w.actualEntryVs=vh;w.actualEntryPs=ph;}
}
const WriterTrace* producerFor(ID3D11Resource* source,bool issuedOnly=false) {
    const WriterTrace* found=nullptr;
    for(const auto& w:writerTrace)if(w.target && source && (!issuedOnly || w.originalIssued) && sameIdentity(w.target.Get(),source) && (!found || w.serial>found->serial))found=&w;
    return found;
}
void writeProducerShader(const wchar_t* prefix,uint64_t hash,ID3D11DeviceChild* shader) noexcept {
    if(!hash || !shader || writerShaderDir.empty())return;const wchar_t stage=prefix[0];
    for(unsigned i=0;i<writerShaderCount;++i)if(writerShaderKeys[i].hash==hash && writerShaderKeys[i].stage==stage)return;
    if(writerShaderCount>=writerShaderKeys.size()){if(writerShaderReports++<12)Log::get().note("Deferred UI: retained producer shader export cap reached (%u files).",unsigned(writerShaderKeys.size()));return;}
    writerShaderKeys[writerShaderCount++]={hash,stage};
    try {
        UINT bytes=0;const HRESULT sized=shader->GetPrivateData(kDeferredBytes,&bytes,nullptr);
        if(FAILED(sized) || !bytes || bytes>1024*1024){if(writerShaderReports++<12)Log::get().note("Deferred UI: retained producer %ls %016llX bytecode unavailable (%08X, %u bytes).",prefix,static_cast<unsigned long long>(hash),unsigned(sized),bytes);return;}
        std::vector<uint8_t> data(bytes);const HRESULT read=shader->GetPrivateData(kDeferredBytes,&bytes,data.data());
        if(FAILED(read)){if(writerShaderReports++<12)Log::get().note("Deferred UI: retained producer %ls %016llX bytecode read failed (%08X).",prefix,static_cast<unsigned long long>(hash),unsigned(read));return;}
        CreateDirectoryW(writerShaderDir.c_str(),nullptr);wchar_t path[MAX_PATH];
        _snwprintf_s(path,_TRUNCATE,L"%s\\%s_%016llX.dxbc",writerShaderDir.c_str(),prefix,static_cast<unsigned long long>(hash));
        HANDLE file=CreateFileW(path,GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(file==INVALID_HANDLE_VALUE){const DWORD error=GetLastError();if(writerShaderReports++<12)Log::get().note("Deferred UI: retained producer shader %ls (%s, error %lu).",path,error==ERROR_FILE_EXISTS?"already present":"write open failed",error);return;}
        DWORD written=0;const bool ok=WriteFile(file,data.data(),bytes,&written,nullptr) && written==bytes;const DWORD error=ok?ERROR_SUCCESS:GetLastError();CloseHandle(file);
        if(writerShaderReports++<12)Log::get().note("Deferred UI: retained producer shader %ls (%s, %u/%u bytes, error %lu).",path,ok?"written":"write failed",written,bytes,error);
    } catch(...) {if(writerShaderReports++<12)Log::get().note("Deferred UI: retained producer %ls %016llX shader export allocation failed.",prefix,static_cast<unsigned long long>(hash));}
}
void writeProducerShaders(const WriterTrace* w) {
    if(!w)return;
    writeProducerShader(L"vs",w->actualEntryVs,w->entryVsObject.Get());
    writeProducerShader(L"ps",w->actualEntryPs,w->entryPsObject.Get());
    writeProducerShader(L"vs",w->actualBeforeVs,w->beforeVsObject.Get());
    writeProducerShader(L"ps",w->actualBeforePs,w->beforePsObject.Get());
}
void reportProducer(ID3D11Resource* source) {
    const auto* w=producerFor(source);const auto* issued=producerFor(source,true);
    if(!w){++producerMisses;lastProducer=WriterTrace{};lastIssuedProducer=WriterTrace{};Log::get().note(
        "Deferred UI: post-tone t0 producer not retained gen=%llu source=%p history=%u.",
        static_cast<unsigned long long>(generation),source,unsigned(kWriterTraceCount));return;}
    ++producerMatches;lastProducer=*w;lastIssuedProducer=issued?*issued:WriterTrace{};
    Log::get().note(
        "Deferred UI: post-tone t0 last observed draw attempt gen=%llu ordinal=%u target=%p entry=%016llX/%016llX actual=%016llX/%016llX before-tone=%u %016llX/%016llX actual=%016llX/%016llX draw=%c/%u/%u verdict=%u original-issued=%u; latest observed original-issued draw=%s gen=%llu ordinal=%u entry=%016llX/%016llX actual=%016llX/%016llX.",
        static_cast<unsigned long long>(w->frame),w->ordinal,w->target.Get(),
        w->entryVs,w->entryPs,w->actualEntryVs,w->actualEntryPs,unsigned(w->reachedBeforeTone),
        w->beforeVs,w->beforePs,w->actualBeforeVs,w->actualBeforePs,w->kind,w->count,w->instances,w->verdict,unsigned(w->originalIssued),
        issued?"retained":"none",static_cast<unsigned long long>(issued?issued->frame:0),issued?issued->ordinal:0,
        issued?issued->entryVs:0,issued?issued->entryPs:0,issued?issued->actualEntryVs:0,issued?issued->actualEntryPs:0);
    writeProducerShaders(w);if(issued!=w)writeProducerShaders(issued);
}
bool sameIdentity(IUnknown* a,IUnknown* b) {
    if(a==b)return true;if(!a || !b)return false;
    Ptr<IUnknown> x,y;return SUCCEEDED(a->QueryInterface(IID_PPV_ARGS(&x))) && SUCCEEDED(b->QueryInterface(IID_PPV_ARGS(&y))) && x==y;
}

void decline(Eye& e,const char* why) {
    ++declined;e.complete=false;e.aborted=true;
    if(!failed){failed=true;enabled=false;resetPending[0]=resetPending[1]=true;
        diagnosticUntilGeneration=generation+1;
        Log::get().note("Deferred UI: disabled until AA is switched Off and back on, or the game restarts. Existing UI handling resumes on following frames; temporal history restarts once per eye.");}
    static unsigned reports=0;
    if(reports++<12)Log::get().note("Deferred UI: original frame retained: %s (draws=%u, VS=%016llX PS=%016llX).",why,e.count,bindingShaderHash(BindSlot::Vs),bindingShaderHash(BindSlot::Ps));
}
bool context(ID3D11DeviceContext* ctx) {
    if(recorder)return true;
    Ptr<ID3D11Device> dev;ctx->GetDevice(&dev);
    return SUCCEEDED(dev->CreateDeferredContext(0,&recorder));
}
bool finish(Ptr<ID3D11CommandList>& list) {
    list.Reset();return SUCCEEDED(recorder->FinishCommandList(FALSE,&list));
}
void execute(ID3D11DeviceContext* ctx,ID3D11CommandList* list) {
    if(list)vScreenExecuteCommandListRaw(ctx,list,TRUE);
}
// Original game colour is never removed: failure can always use it.
void restore(ID3D11DeviceContext*,Eye& e) { e.restored=true; }

bool target(ID3D11DeviceContext* ctx,Ptr<ID3D11RenderTargetView>& rt,Ptr<ID3D11DepthStencilView>& ds,Ptr<ID3D11Texture2D>& tex) {
    ID3D11RenderTargetView* raw[8]{};ctx->OMGetRenderTargets(8,raw,&ds);rt.Attach(raw[0]);bool single=rt!=nullptr;
    for(UINT i=1;i<8;++i)if(raw[i]){single=false;raw[i]->Release();}
    if(!single)return false;
    Ptr<ID3D11Resource> r;rt->GetResource(&r);return SUCCEEDED(r.As(&tex));
}
size_t material(ID3D11Device* dev,ID3D11PixelShader* ps,ID3D11BlendState* blend,bool worldRole=false,std::string* failure=nullptr) {
    for(size_t i=0;i<materials.size();++i)if(materials[i].original.Get()==ps && materials[i].originalBlend.Get()==blend && materials[i].worldRole==worldRole)return i;
    auto fail=[&](const std::string& why){if(failure)*failure=why;return SIZE_MAX;};
    if(materials.size()>=128)return fail("material cache full");
    D3D11_BLEND_DESC bd{};
    for(auto& r:bd.RenderTarget){r.SrcBlend=r.SrcBlendAlpha=D3D11_BLEND_ONE;r.DestBlend=r.DestBlendAlpha=D3D11_BLEND_ZERO;r.BlendOp=r.BlendOpAlpha=D3D11_BLEND_OP_ADD;r.RenderTargetWriteMask=15;}
    if(blend)blend->GetDesc(&bd);
    Ptr<ID3D11BlendState1> b1;if(blend && SUCCEEDED(blend->QueryInterface(IID_PPV_ARGS(&b1)))){D3D11_BLEND_DESC1 d{};b1->GetDesc1(&d);if(d.RenderTarget[0].LogicOpEnable)return fail("render-target logic operation");}
    auto r=bd.RenderTarget[0];
    const bool sourceIndependent=r.SrcBlend==D3D11_BLEND_ZERO || r.SrcBlend==D3D11_BLEND_ONE || r.SrcBlend==D3D11_BLEND_SRC_COLOR || r.SrcBlend==D3D11_BLEND_INV_SRC_COLOR || r.SrcBlend==D3D11_BLEND_SRC_ALPHA || r.SrcBlend==D3D11_BLEND_INV_SRC_ALPHA || r.SrcBlend==D3D11_BLEND_BLEND_FACTOR || r.SrcBlend==D3D11_BLEND_INV_BLEND_FACTOR;
    // Only source-independent attenuation can be carried in one scalar.
    const bool uiBlendSupported=!bd.AlphaToCoverageEnable && (r.RenderTargetWriteMask&7)==7 &&
       (!r.BlendEnable || (sourceIndependent && r.BlendOp==D3D11_BLEND_OP_ADD &&
        (r.DestBlend==D3D11_BLEND_ZERO || r.DestBlend==D3D11_BLEND_ONE || r.DestBlend==D3D11_BLEND_INV_SRC_ALPHA) &&
        r.SrcBlend!=D3D11_BLEND_DEST_COLOR && r.SrcBlend!=D3D11_BLEND_INV_DEST_COLOR &&
        r.SrcBlend!=D3D11_BLEND_DEST_ALPHA && r.SrcBlend!=D3D11_BLEND_INV_DEST_ALPHA));
    const bool destinationAlphaWorld=r.BlendEnable && r.SrcBlend==D3D11_BLEND_ONE && r.DestBlend==D3D11_BLEND_DEST_ALPHA && r.BlendOp==D3D11_BLEND_OP_ADD &&
        r.SrcBlendAlpha==D3D11_BLEND_ONE && r.DestBlendAlpha==D3D11_BLEND_DEST_ALPHA && r.BlendOpAlpha==D3D11_BLEND_OP_ADD;
    if(worldRole) {
        if(bd.AlphaToCoverageEnable)return fail("world alpha-to-coverage");
        if((r.RenderTargetWriteMask&7)!=7)return fail("world RGB write mask incomplete");
        if(!uiBlendSupported && !destinationAlphaWorld)return fail("world blend unsupported (enabled="+std::to_string(r.BlendEnable)+
            ", colour="+std::to_string(r.SrcBlend)+"/"+std::to_string(r.DestBlend)+"/"+std::to_string(r.BlendOp)+
            ", alpha="+std::to_string(r.SrcBlendAlpha)+"/"+std::to_string(r.DestBlendAlpha)+"/"+std::to_string(r.BlendOpAlpha)+")");
    } else if(!uiBlendSupported)return fail("UI scalar transmission cannot represent blend");
    UINT n=0;ps->GetPrivateData(kDeferredBytes,&n,nullptr);if(!n || n>1024*1024)return fail("pixel shader bytecode unavailable");
    std::vector<BYTE> original(n),patched;std::string why;
    if(FAILED(ps->GetPrivateData(kDeferredBytes,&n,original.data())))return fail("pixel shader bytecode read failed");
    if(!uiColourFanout(original.data(),n,patched,why))return fail("pixel shader fanout: "+why);
    Material m;m.original=ps;m.originalBlend=blend;m.worldRole=worldRole;
    if(FAILED(dev->CreatePixelShader(patched.data(),patched.size(),nullptr,&m.fanout)))return fail("fanout pixel shader creation failed");
    if(worldRole) {
        auto world=bd;world.IndependentBlendEnable=TRUE;world.RenderTarget[1]=r;
        if(FAILED(dev->CreateBlendState(&world,&m.world)))return fail("world dual-target blend creation failed");
    } else {
        bd.IndependentBlendEnable=TRUE;bd.RenderTarget[1].RenderTargetWriteMask=0;
        auto& t=bd.RenderTarget[2];t={};t.BlendEnable=TRUE;t.SrcBlend=t.SrcBlendAlpha=D3D11_BLEND_ZERO;
        t.DestBlend=t.DestBlendAlpha=r.BlendEnable?r.DestBlend:D3D11_BLEND_ZERO;
        t.BlendOp=t.BlendOpAlpha=D3D11_BLEND_OP_ADD;t.RenderTargetWriteMask=D3D11_COLOR_WRITE_ENABLE_RED;
        if(FAILED(dev->CreateBlendState(&bd,&m.transmission)))return fail("UI transmission blend creation failed");
    }
    materials.push_back(std::move(m));return materials.size()-1;
}

struct Renderer {
    Surface hdr,transmission,mappedTransmission,base,ui,mappedBase,mappedUi,composite;
    edvr_deferred_depth::Seeder depth;
    bool depthReady=false,shaderTried=false;
    Ptr<ID3D11VertexShader> vs;
    Ptr<ID3D11PixelShader> seed;
    Ptr<ID3D11ComputeShader> combine;
    Ptr<ID3D11SamplerState> sampler;
    Ptr<ID3D11DepthStencilState> colourOnly;
    Ptr<ID3D11Buffer> params;
    bool ensure(ID3D11DeviceContext* ctx,UINT w,UINT h,DXGI_FORMAT fmt,bool sampled,bool tails) {
        Ptr<ID3D11Device> dev;ctx->GetDevice(&dev);
        if(!shaderTried){shaderTried=true;vs.Attach(shaderSwapCompileVs(ctx,kDeferredSeed,sizeof(kDeferredSeed)-1,"vs","UI seed",nullptr,"Deferred UI"));
            seed.Attach(shaderSwapCompilePs(ctx,kDeferredSeed,sizeof(kDeferredSeed)-1,"ps","UI seed",nullptr,"Deferred UI"));
            combine.Attach(shaderSwapCompileCs(ctx,kDeferredComposite,sizeof(kDeferredComposite)-1,"main","UI composite",nullptr,"Deferred UI"));}
        if(!vs || !seed || !combine)return false;
        if(!sampler){D3D11_SAMPLER_DESC d{};d.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;d.AddressU=d.AddressV=d.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;d.MaxLOD=D3D11_FLOAT32_MAX;
            if(FAILED(dev->CreateSamplerState(&d,&sampler)))return false;}
        if(!params){D3D11_BUFFER_DESC d{};d.ByteWidth=16;d.BindFlags=D3D11_BIND_CONSTANT_BUFFER;if(FAILED(dev->CreateBuffer(&d,nullptr,&params)))return false;}
        if(!colourOnly){D3D11_DEPTH_STENCIL_DESC d{};d.DepthEnable=FALSE;d.StencilEnable=FALSE;if(FAILED(dev->CreateDepthStencilState(&d,&colourOnly)))return false;}
        if(!depthReady){depth.init(dev.Get());depthReady=true;}
        const bool common=depth.ensure(dev.Get(),w,h,fmt) && hdr.ensure(dev.Get(),w,h,DXGI_FORMAT_R11G11B10_FLOAT) &&
            transmission.ensure(dev.Get(),w,h,DXGI_FORMAT_R32_FLOAT) && base.ensure(dev.Get(),w,h,DXGI_FORMAT_R8G8B8A8_UNORM) &&
            ui.ensure(dev.Get(),w,h,DXGI_FORMAT_R8G8B8A8_UNORM) && composite.ensure(dev.Get(),w,h,DXGI_FORMAT_R8G8B8A8_UNORM,true,tails);
        return common && (!sampled || (mappedBase.ensure(dev.Get(),w,h,DXGI_FORMAT_R8G8B8A8_UNORM) &&
            mappedUi.ensure(dev.Get(),w,h,DXGI_FORMAT_R8G8B8A8_UNORM) && mappedTransmission.ensure(dev.Get(),w,h,DXGI_FORMAT_R32_FLOAT)));
    }
} renderer;
} // namespace

bool uiDeferredInternal(){return inside;}
void uiDeferredRemember(ID3D11DeviceChild* shader,const void* data,size_t size,bool linked) {
    if(!shader || !data || linked || inside || size<32 || size>1024*1024)return;
    shader->SetPrivateData(kDeferredBytes,UINT(size),data);
}
void uiDeferredConfigure(Config& cfg) {
    const auto m=cfg.getString("fix.temporal_aa","off");
    const bool requested=temporalExternalEngine(m);
    if(!requested){failed=false;routeNoted=false;}
    // The fovea path (temporal_pass.cpp) now runs this same capture/replay
    // route too -- its own compose writes the texture this route reads and
    // replays into, so there is nothing here to stand down for a fovea.
    enabled=requested && !failed;
    writerShaderDir=cfg.logDir()+L"\\shaders";
}
void uiDeferredTraceDrawEnter(ID3D11DeviceContext* ctx,bool eyeSizedTarget,char kind,uint32_t count,uint32_t instances,
    uint32_t verdict,uint64_t originalVs,uint64_t originalPs) {
    writerTracePending=-1;
    if(!ctx || inside || (!enabled && !diagnosticWindow()))return;
    const uint32_t ordinal=++writerTraceOrdinal;
    if(!eyeSizedTarget)return;
    const uint32_t rg=bindingGeneration(BindSlot::Rtv0),dg=bindingGeneration(BindSlot::Dsv0);
    if(writerTargetCache.rtvGeneration!=rg || writerTargetCache.dsvGeneration!=dg) {
        writerTargetCache=WriterTargetCache{};writerTargetCache.rtvGeneration=rg;writerTargetCache.dsvGeneration=dg;
        writerTargetCache.shadowRtv=bindingGet(BindSlot::Rtv0);
        ResourceInfo info;
        if(writerTargetCache.shadowRtv && !bindingGet(BindSlot::Dsv0) && bindingResolve(writerTargetCache.shadowRtv,&info) &&
           info.isTexture2D && vScreenIsEyeSized(info.a,info.b) &&
           (info.fmt==DXGI_FORMAT_R8G8B8A8_TYPELESS || info.fmt==DXGI_FORMAT_R8G8B8A8_UNORM)) {
            ++writerTargetQueries;ID3D11RenderTargetView* raw[2]{};Ptr<ID3D11DepthStencilView> depth;
            ctx->OMGetRenderTargets(2,raw,&depth);Ptr<ID3D11RenderTargetView> target0,target1;
            target0.Attach(raw[0]);target1.Attach(raw[1]);
            D3D11_RENDER_TARGET_VIEW_DESC view{};if(target0)target0->GetDesc(&view);
            Ptr<ID3D11Resource> resource;if(target0)target0->GetResource(&resource);Ptr<ID3D11Texture2D> texture;
            if(target0 && !target1 && !depth && view.Format==DXGI_FORMAT_R8G8B8A8_UNORM &&
               view.ViewDimension==D3D11_RTV_DIMENSION_TEXTURE2D && !view.Texture2D.MipSlice && resource && SUCCEEDED(resource.As(&texture))) {
                D3D11_TEXTURE2D_DESC td{};texture->GetDesc(&td);
                if(td.ArraySize==1 && td.MipLevels==1 && td.SampleDesc.Count==1 &&
                   (td.Format==DXGI_FORMAT_R8G8B8A8_TYPELESS || td.Format==DXGI_FORMAT_R8G8B8A8_UNORM)) {
                    writerTargetCache.resource=resource;writerTargetCache.candidate=true;
                }
            }
        }
    }
    if(!writerTargetCache.candidate || !writerTargetCache.resource)return;
    const size_t slot=writerTraceNext++%writerTrace.size();auto& w=writerTrace[slot];w=WriterTrace{};
    w.target=writerTargetCache.resource;w.serial=++writerTraceSerial;w.frame=generation;w.ordinal=ordinal;
    w.entryVs=originalVs;w.entryPs=originalPs;w.kind=kind;w.count=count;w.instances=instances;w.verdict=verdict;
    actualShaderHashes(ctx,w,false);writerTracePending=static_cast<int>(slot);
}
void uiDeferredTraceBeforeTone(ID3D11DeviceContext* ctx) {
    if(!ctx || writerTracePending<0)return;auto& w=writerTrace[static_cast<size_t>(writerTracePending)];
    w.reachedBeforeTone=true;actualShaderHashes(ctx,w,true);
}
void uiDeferredTraceOriginalIssued() {
    if(writerTracePending>=0)writerTrace[static_cast<size_t>(writerTracePending)].originalIssued=true;
}
bool uiDeferredFallbackReset(int eye){if(eye<0 || eye>1)return false;bool reset=resetPending[eye];resetPending[eye]=false;return reset;}
UiDeferredEyeState uiDeferredEyeState(int eye){
    if(eye<0 || eye>1)return UiDeferredEyeState{false,false,0,0,false};
    const Eye& e=eyes[eye];
    return UiDeferredEyeState{enabled,e.postTone.ready(),static_cast<unsigned>(e.aliases.size()),e.count,e.complete};
}
static bool exactGlassReplayState(ID3D11DeviceContext* ctx,char kind,UINT instances,UINT verdict,
                                  bool countingQuery,ID3D11PixelShader* ps,ID3D11BlendState* blend,
                                  ID3D11DepthStencilView* depth,std::string& why) {
    auto fail=[&](const char* reason){why=reason;return false;};
    if(verdict)return fail("draw wrapper verdict is not kNone");
    if(kind!='X' || instances!=1)return fail("draw arguments differ from retained indexed-instanced shape");
    if(countingQuery)return fail("counting query interval is active");
    if(!depth || !blend)return fail("depth/stencil or blend state is absent");
    Ptr<ID3D11VertexShader> vs;ctx->VSGetShader(&vs,nullptr,nullptr);
    if(bindingShaderHash(BindSlot::Vs)!=kUiDeferredGlassVs || bindingShaderHash(BindSlot::Ps)!=kUiDeferredGlassPs ||
       !vs || lookupShaderHash(vs.Get())!=kUiDeferredGlassVs || lookupShaderHash(ps)!=kUiDeferredGlassPs)
        return fail("actual shader pair differs from retained glass pair");
    D3D11_PRIMITIVE_TOPOLOGY topology{};ctx->IAGetPrimitiveTopology(&topology);
    if(topology!=D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST)return fail("primitive topology differs from retained triangle list");
    Ptr<ID3D11GeometryShader> gs;Ptr<ID3D11HullShader> hs;Ptr<ID3D11DomainShader> ds;
    ctx->GSGetShader(&gs,nullptr,nullptr);ctx->HSGetShader(&hs,nullptr,nullptr);ctx->DSGetShader(&ds,nullptr,nullptr);
    if(gs || hs || ds)return fail("ancillary shader stage is active");
    ID3D11Buffer* so[4]{};ctx->SOGetTargets(4,so);bool hasSo=false;
    for(auto* buffer:so)if(buffer){hasSo=true;buffer->Release();}
    if(hasSo)return fail("stream-output target is active");
    Ptr<ID3D11Predicate> predicate;BOOL predicateValue=FALSE;ctx->GetPredication(&predicate,&predicateValue);
    if(predicate)return fail("predication is active");
    D3D11_BLEND_DESC bd{};blend->GetDesc(&bd);const auto& r=bd.RenderTarget[0];
    if(bd.AlphaToCoverageEnable || !r.BlendEnable || r.RenderTargetWriteMask!=15 ||
       r.SrcBlend!=D3D11_BLEND_ONE || r.DestBlend!=D3D11_BLEND_SRC1_COLOR || r.BlendOp!=D3D11_BLEND_OP_ADD ||
       r.SrcBlendAlpha!=D3D11_BLEND_ONE || r.DestBlendAlpha!=D3D11_BLEND_SRC1_ALPHA || r.BlendOpAlpha!=D3D11_BLEND_OP_ADD)
        return fail("blend equation differs from retained dual-source transmission");
    Ptr<ID3D11BlendState1> blend1;if(SUCCEEDED(blend->QueryInterface(IID_PPV_ARGS(&blend1)))) {
        D3D11_BLEND_DESC1 desc{};blend1->GetDesc1(&desc);if(desc.RenderTarget[0].LogicOpEnable)return fail("render-target logic operation is active");
    }
    Ptr<ID3D11DepthStencilState> state;UINT stencilRef=0;ctx->OMGetDepthStencilState(&state,&stencilRef);
    if(!state)return fail("depth/stencil state is absent");
    D3D11_DEPTH_STENCIL_DESC dd{};state->GetDesc(&dd);const auto& f=dd.FrontFace;const auto& b=dd.BackFace;
    if(!dd.DepthEnable || dd.DepthWriteMask!=D3D11_DEPTH_WRITE_MASK_ZERO || dd.DepthFunc!=D3D11_COMPARISON_GREATER_EQUAL ||
       !dd.StencilEnable || dd.StencilReadMask!=0 || dd.StencilWriteMask!=4 || stencilRef!=4 ||
       f.StencilFunc!=D3D11_COMPARISON_ALWAYS || f.StencilFailOp!=D3D11_STENCIL_OP_KEEP ||
       f.StencilDepthFailOp!=D3D11_STENCIL_OP_KEEP || f.StencilPassOp!=D3D11_STENCIL_OP_REPLACE ||
       b.StencilFunc!=D3D11_COMPARISON_ALWAYS || b.StencilFailOp!=D3D11_STENCIL_OP_KEEP ||
       b.StencilDepthFailOp!=D3D11_STENCIL_OP_KEEP || b.StencilPassOp!=D3D11_STENCIL_OP_KEEP)
        return fail("depth/stencil state differs from retained idempotent state");
    return true;
}
static bool begin(ID3D11DeviceContext* ctx,int eye,char kind,UINT count,UINT instances,UINT start,INT base,UINT first,
                  UINT verdict,bool countingQuery) {
    if(!enabled || inside || eye>1 || colourMuted)return false;
    if(eye<0) {
        // Elite interleaves opaque and translucent world draws with UI.
        // Mirror those world draws into the clean target in the same raster
        // pass. The original target and its depth/stencil remain unchanged.
        bool pending=false;for(auto& e:eyes)pending=pending || (e.count && !e.complete && !e.aborted);
        if(!pending)return false;
        Scope scope;Ptr<ID3D11RenderTargetView> rt;Ptr<ID3D11DepthStencilView> ds;Ptr<ID3D11Texture2D> tex;
        ID3D11RenderTargetView* bound[8]{};ctx->OMGetRenderTargets(8,bound,&ds);
        std::array<Ptr<ID3D11RenderTargetView>,8> retained;bool touches=false,multiple=false;
        for(UINT i=0;i<8;++i){retained[i].Attach(bound[i]);if(!bound[i])continue;multiple=multiple || i!=0;Ptr<ID3D11Resource> resource;bound[i]->GetResource(&resource);
            for(auto& e:eyes)if(e.count && !e.complete && !e.aborted && sameIdentity(e.hdr.Get(),resource.Get()))touches=true;}
        if(!touches)return false;
        if(!bound[0] || multiple){for(auto& e:eyes)if(e.count && !e.complete)decline(e,"interleaved MRT world draw");return false;}
        rt=retained[0];Ptr<ID3D11Resource> resource;rt->GetResource(&resource);if(FAILED(resource.As(&tex)))return false;
        for(auto& e:eyes)if(e.count && !e.complete && !e.aborted && e.hdr==tex) {
            Ptr<ID3D11Device> dev;ctx->GetDevice(&dev);Ptr<ID3D11PixelShader> ps;ctx->PSGetShader(&ps,nullptr,nullptr);
            if(!ps)return false; // A depth-only world draw cannot change colour.
            ID3D11UnorderedAccessView* uavs[8]{};ctx->OMGetRenderTargetsAndUnorderedAccessViews(0,nullptr,nullptr,0,8,uavs);bool hasUav=false;for(auto* u:uavs)if(u){hasUav=true;u->Release();}
            if(hasUav){decline(e,"interleaved world UAV writes");return false;}
            Ptr<ID3D11DepthStencilState> state;ctx->OMGetDepthStencilState(&state,nullptr);D3D11_DEPTH_STENCIL_DESC dd{};if(state)state->GetDesc(&dd);
            if(dd.StencilEnable && (dd.StencilReadMask&e.changedStencil) && (dd.FrontFace.StencilFunc!=D3D11_COMPARISON_ALWAYS || dd.BackFace.StencilFunc!=D3D11_COMPARISON_ALWAYS)){
                decline(e,"world draw reads stencil changed by UI");return false;}
            Ptr<ID3D11BlendState> blend;float factors[4]{};UINT sampleMask=0;ctx->OMGetBlendState(&blend,factors,&sampleMask);
            const bool glass=bindingShaderHash(BindSlot::Vs)==kUiDeferredGlassVs && bindingShaderHash(BindSlot::Ps)==kUiDeferredGlassPs;
            if(glass) {
                std::string why;
                if(sampleMask!=~0u)why="sample mask differs from retained full mask";
                else exactGlassReplayState(ctx,kind,instances,verdict,countingQuery,ps.Get(),blend.Get(),ds.Get(),why);
                if(!why.empty()){decline(e,("dual-source glass replay unsupported: "+why).c_str());return false;}
                savedTarget=rt;savedDepth=ds;savedPs=ps;savedBlend=blend;
                memcpy(savedFactors,factors,sizeof(savedFactors));savedMask=sampleMask;
                replayTarget=e.cleanHdr.rtv;worldDrawMode=WorldDrawMode::GlassPending;colourMuted=true;
                return false;
            }
            std::string why;auto index=ps?material(dev.Get(),ps.Get(),blend.Get(),true,&why):SIZE_MAX;
            if(index==SIZE_MAX){decline(e,why.empty()?"interleaved world pixel shader absent":why.c_str());return false;}
            savedTarget=rt;savedDepth=ds;savedPs=ps;ctx->OMGetBlendState(&savedBlend,savedFactors,&savedMask);
            ID3D11RenderTargetView* targets[2]={rt.Get(),e.cleanHdr.rtv.Get()};
            vScreenSetRenderTargetsRaw(ctx,2,targets,ds.Get());ctx->OMSetBlendState(materials[index].world.Get(),savedFactors,savedMask);ctx->PSSetShader(materials[index].fanout.Get(),nullptr,0);worldDrawMode=WorldDrawMode::Fanout;colourMuted=true;
            return false;
        }
        return false;
    }
    auto& e=eyes[eye];if(e.complete || e.aborted || e.restored)return false;
    Scope scope;
    Ptr<ID3D11RenderTargetView> rt;Ptr<ID3D11DepthStencilView> ds;Ptr<ID3D11Texture2D> tex;
    if(!target(ctx,rt,ds,tex))return false;
    D3D11_TEXTURE2D_DESC td{};tex->GetDesc(&td);
    if(td.Format!=DXGI_FORMAT_R11G11B10_FLOAT || td.SampleDesc.Count!=1 || td.MipLevels!=1 || td.ArraySize!=1)return false;
    D3D11_VIEWPORT viewport{};UINT viewportCount=1;ctx->RSGetViewports(&viewportCount,&viewport);
    if(viewportCount!=1 || viewport.TopLeftX!=0 || viewport.TopLeftY!=0 || viewport.Width!=td.Width || viewport.Height!=td.Height)return false;
    Ptr<ID3D11Device> dev;ctx->GetDevice(&dev);
    if(!context(ctx))return false;
    if(!e.count){
        if(!e.cleanHdr.ensure(dev.Get(),td.Width,td.Height,td.Format) || !e.cleanLdr.ensure(dev.Get(),td.Width,td.Height,DXGI_FORMAT_R8G8B8A8_UNORM) || !e.depth.seed(dev.Get(),ctx,ds.Get()))return false;
        e.hdr=tex;e.hdrTarget=rt;e.w=td.Width;e.h=td.Height;ctx->CopyResource(e.cleanHdr.tex.Get(),tex.Get());
    }else {Ptr<ID3D11Resource> depth;if(ds)ds->GetResource(&depth);if(e.hdr.Get()!=tex.Get() || !sameIdentity(e.depth.source.Get(),depth.Get()))return false;}
    if(e.count>=64)return false;
    if(e.draws.size()==e.count)e.draws.push_back(std::make_unique<Draw>());
    auto& d=*e.draws[e.count];
    const auto* mask=reflect(ctx);if(!mask)return false;
    if(!d.packet.capture(ctx,snapshots,kind,count,instances,start,base,first,*mask)){
        const auto vs=bindingShaderHash(BindSlot::Vs),ps=bindingShaderHash(BindSlot::Ps);const char* why=d.packet.failureReason();bool seen=false;
        const int slot=d.packet.failureResourceSlot();const char stage=d.packet.failureResourceStage();
        for(const auto& f:captureFailures)seen=seen || (f.vs==vs && f.ps==ps && f.reason==why && f.stage==stage && f.slot==slot);
        if(!seen && captureFailures.size()<32){captureFailures.push_back({vs,ps,why,stage,slot});
            if(slot>=0)Log::get().note("Deferred UI: draw capture refused VS=%016llX PS=%016llX: %s (stage=%c slot=%d allocated=%llu copied=%llu).",vs,ps,why,stage,slot,static_cast<unsigned long long>(snapshots.allocatedBytes()),static_cast<unsigned long long>(snapshots.copiedBytes()));
            else Log::get().note("Deferred UI: draw capture refused VS=%016llX PS=%016llX: %s (allocated=%llu copied=%llu).",vs,ps,why,static_cast<unsigned long long>(snapshots.allocatedBytes()),static_cast<unsigned long long>(snapshots.copiedBytes()));}
        return false;
    }
    D3D11_DEPTH_STENCIL_DESC depthState{};
    if(d.packet.depthState())d.packet.depthState()->GetDesc(&depthState);
    else {depthState.DepthEnable=TRUE;depthState.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ALL;}
    // Final world depth can seed native UI only if UI did not replace it.
    if(depthState.DepthEnable && depthState.DepthWriteMask==D3D11_DEPTH_WRITE_MASK_ALL)return false;
    if(depthState.StencilEnable)for(const auto& face:{depthState.FrontFace,depthState.BackFace}){
        if(face.StencilFailOp!=D3D11_STENCIL_OP_KEEP || face.StencilDepthFailOp!=D3D11_STENCIL_OP_KEEP)e.changedStencil|=depthState.StencilWriteMask;
        if(face.StencilPassOp!=D3D11_STENCIL_OP_KEEP)e.changedStencil|=depthState.StencilWriteMask &
            (face.StencilPassOp==D3D11_STENCIL_OP_REPLACE && face.StencilFunc==D3D11_COMPARISON_EQUAL?~depthState.StencilReadMask:255u);
    }
    d.material=material(dev.Get(),d.packet.originalPixelShader(),d.packet.blendState());
    if(d.material==SIZE_MAX)return false;
    if(!e.count)e.generation=generation;
    ++e.count;++captured;
    ++diagnostic.captures;
    if(captured<=12)Log::get().note("Deferred UI: captured %c draw, gen=%llu VS=%016llX PS=%016llX eye=%d %ux%u HDR=%p; depth=%u/%u stencil=%u read=%02X write=%02X ref=%u front=%u/%u/%u/%u back=%u/%u/%u/%u.",kind,
        static_cast<unsigned long long>(generation),bindingShaderHash(BindSlot::Vs),bindingShaderHash(BindSlot::Ps),eye,e.w,e.h,e.hdr.Get(),
        unsigned(depthState.DepthEnable),unsigned(depthState.DepthWriteMask),unsigned(depthState.StencilEnable),unsigned(depthState.StencilReadMask),unsigned(depthState.StencilWriteMask),unsigned(d.packet.stencilRef()),
        unsigned(depthState.FrontFace.StencilFunc),unsigned(depthState.FrontFace.StencilFailOp),unsigned(depthState.FrontFace.StencilDepthFailOp),unsigned(depthState.FrontFace.StencilPassOp),
        unsigned(depthState.BackFace.StencilFunc),unsigned(depthState.BackFace.StencilFailOp),unsigned(depthState.BackFace.StencilDepthFailOp),unsigned(depthState.BackFace.StencilPassOp));
    return true;
}
bool uiDeferredBegin(ID3D11DeviceContext* ctx,int eye,char kind,UINT count,UINT instances,UINT start,INT base,UINT first,UINT verdict,bool countingQuery) {
    if(routeHandledThisDraw){routeHandledThisDraw=false;return true;}
    bool ok=false;
    try { ok=begin(ctx,eye,kind,count,instances,start,base,first,verdict,countingQuery); }
    catch(...) {
        uiDeferredEnd(ctx);
        for(auto& e:eyes)if(e.count && !e.complete)decline(e,"draw allocation failed");
    }
    if(!ok && enabled && !inside && eye>=0 && eye<2 && !eyes[eye].aborted) {
        // An unsupported UI in the middle of the sequence must retain its
        // order relative to already captured UI, including the fallback.
        auto& e=eyes[eye];restore(ctx,e);decline(e,"UI state, resources or allocation unsupported");
    }
    return ok;
}
bool uiDeferredWorldReplayBegin(ID3D11DeviceContext* ctx) {
    if(!ctx || worldDrawMode!=WorldDrawMode::GlassPending || !replayTarget)return false;
    Scope scope;ID3D11RenderTargetView* target=replayTarget.Get();
    vScreenSetRenderTargetsRaw(ctx,1,&target,savedDepth.Get());worldDrawMode=WorldDrawMode::GlassBound;return true;
}
void uiDeferredEnd(ID3D11DeviceContext* ctx) {
    if(!colourMuted)return;Scope scope;vScreenSetRenderTargetsRaw(ctx,1,savedTarget.GetAddressOf(),savedDepth.Get());ctx->OMSetBlendState(savedBlend.Get(),savedFactors,savedMask);ctx->PSSetShader(savedPs.Get(),nullptr,0);
    if(worldDrawMode==WorldDrawMode::GlassBound){++glassReplays;if(glassReplayReports++<4)Log::get().note("Deferred UI: replayed dual-source glass into clean HDR gen=%llu VS=%016llX PS=%016llX total=%llu.",static_cast<unsigned long long>(generation),kUiDeferredGlassVs,kUiDeferredGlassPs,static_cast<unsigned long long>(glassReplays));}
    savedBlend.Reset();savedPs.Reset();savedTarget.Reset();savedDepth.Reset();replayTarget.Reset();worldDrawMode=WorldDrawMode::None;colourMuted=false;
}

static void beforeTone(ID3D11DeviceContext* ctx,char kind,UINT count,UINT instances,UINT start,INT base,UINT first) {
    if(inside)return;
    const auto vs=bindingShaderHash(BindSlot::Vs),ps=bindingShaderHash(BindSlot::Ps);
    if((vs!=EyeTonemapSnapshot::kVs && vs!=kToneVsConstantExposure) || ps!=EyeTonemapSnapshot::kPs)return;
    const bool observe=eyes[0].count || eyes[1].count || diagnosticWindow();
    const bool report=observe && toneReports<8;
    if(!enabled && !report){if(observe)++diagnostic.toneObservations;return;}
    Scope scope;Ptr<ID3D11ShaderResourceView> input;ctx->PSGetShaderResources(1,1,&input);
    Ptr<ID3D11Resource> source;if(input)input->GetResource(&source);
    if(observe) {
        ++diagnostic.toneObservations;
        if(report){++toneReports;Log::get().note(
            "Deferred UI: expected tone observed enabled=%u gen=%llu t1=%p; eye0 gen=%llu HDR=%p draws=%u complete=%u restored=%u aborted=%u, eye1 gen=%llu HDR=%p draws=%u complete=%u restored=%u aborted=%u.",
            unsigned(enabled),static_cast<unsigned long long>(generation),source.Get(),static_cast<unsigned long long>(eyes[0].generation),eyes[0].hdr.Get(),eyes[0].count,unsigned(eyes[0].complete),unsigned(eyes[0].restored),unsigned(eyes[0].aborted),
            static_cast<unsigned long long>(eyes[1].generation),eyes[1].hdr.Get(),eyes[1].count,unsigned(eyes[1].complete),unsigned(eyes[1].restored),unsigned(eyes[1].aborted));}
    }
    if(!enabled || !input)return;
    for(auto& e:eyes)if(e.count && sameIdentity(e.hdr.Get(),source.Get()) && !e.restored && !e.aborted) {
        Ptr<ID3D11RenderTargetView> rt;Ptr<ID3D11DepthStencilView> ds;Ptr<ID3D11Texture2D> tex;
        bool valid=kind=='N' && count==3 && instances==1 && target(ctx,rt,ds,tex);
        D3D11_TEXTURE2D_DESC td{};if(tex)tex->GetDesc(&td);
        valid=valid && td.Width==e.w && td.Height==e.h && td.MipLevels==1 && td.ArraySize==1 && td.SampleDesc.Count==1;
        D3D11_RENDER_TARGET_VIEW_DESC rd{};if(rt)rt->GetDesc(&rd);valid=valid && rd.Format==DXGI_FORMAT_R8G8B8A8_UNORM;
        Ptr<ID3D11DepthStencilState> dss;ctx->OMGetDepthStencilState(&dss,nullptr);D3D11_DEPTH_STENCIL_DESC dd{};if(dss)dss->GetDesc(&dd);else dd.DepthEnable=TRUE;
        Ptr<ID3D11BlendState> bs;ctx->OMGetBlendState(&bs,nullptr,nullptr);D3D11_BLEND_DESC bd{};if(bs)bs->GetDesc(&bd);
        Ptr<ID3D11RasterizerState> rs;ctx->RSGetState(&rs);D3D11_RASTERIZER_DESC rsd{};if(rs)rs->GetDesc(&rsd);else rsd.FillMode=D3D11_FILL_SOLID;
        D3D11_VIEWPORT vp{};UINT vn=1;ctx->RSGetViewports(&vn,&vp);
        valid=valid && !dd.DepthEnable && !dd.StencilEnable && !bd.AlphaToCoverageEnable && !bd.RenderTarget[0].BlendEnable && (!bs || (bd.RenderTarget[0].RenderTargetWriteMask&7)==7) && !rsd.ScissorEnable && rsd.FillMode==D3D11_FILL_SOLID && vn==1 && vp.TopLeftX==0 && vp.TopLeftY==0 && vp.Width==e.w && vp.Height==e.h;
        const auto* reflected=reflect(ctx);valid=valid && reflected;
        UiDeferredCaptureMask mask;if(reflected)mask=*reflected;mask.tone=true;
        if(valid)valid=e.tone.capture(ctx,snapshots,kind,count,instances,start,base,first,mask);
        if(valid){
            ctx->CopyResource(e.depth.before.Get(),e.depth.source.Get());recorder->ClearState();
            valid=e.tone.bind(recorder.Get(),e.tone.originalPixelShader(),e.cleanLdr.rtv.Get(),nullptr,nullptr,e.tone.originalViewport());
            auto* hdr=e.cleanHdr.srv.Get();recorder->PSSetShaderResources(1,1,&hdr);e.tone.draw(recorder.Get());
            Ptr<ID3D11CommandList> list;valid=valid && finish(list);if(valid)execute(ctx,list.Get());
        }
        // The game receives a complete colour image regardless of NGX success,
        // unsupported submit routes, cropped AA modes, or capture-only submits.
        restore(ctx,e);
        if(valid){e.complete=true;e.aliases.clear();e.aliases.push_back(tex);++diagnostic.toneAliases;
            if(aliasReports++<8)Log::get().note("Deferred UI: tone alias added enabled=%u gen=%llu eye=%u eye_gen=%llu tone=%p HDR=%p draws=%u.",
                unsigned(enabled),static_cast<unsigned long long>(generation),unsigned(&e-eyes),static_cast<unsigned long long>(e.generation),tex.Get(),e.hdr.Get(),e.count);}
        else decline(e,"tone pass unsupported");
        return;
    }
}
void uiDeferredBeforeTone(ID3D11DeviceContext* ctx,char kind,UINT count,UINT instances,UINT start,INT base,UINT first) {
    try { beforeTone(ctx,kind,count,instances,start,base,first); }
    catch(...) { for(auto& e:eyes)if(e.count && !e.restored){restore(ctx,e);decline(e,"tone allocation failed");} }
}

static ID3D11ShaderResourceView* prepare(ID3D11DeviceContext* ctx,ID3D11Texture2D* submitted,
    ID3D11ShaderResourceView* sceneDepth,ID3D11Texture2D* output,int eye,UINT w,UINT h,float jx,float jy) {
    if(!enabled || inside || eye<0 || eye>1 || !submitted || !output || !sceneDepth)return nullptr;
    ++diagnostic.prepares;
    if((eyes[0].count || eyes[1].count || eyes[0].complete || eyes[1].complete) && prepareReports++<8)
        Log::get().note("Deferred UI: prepare enabled=%u gen=%llu eye=%d submitted=%p output=%p; eye0 gen=%llu HDR=%p tone=%p sampled=%u tails=%u, eye1 gen=%llu HDR=%p tone=%p sampled=%u tails=%u; VS=%016llX PS=%016llX.",
            unsigned(enabled),static_cast<unsigned long long>(generation),eye,submitted,output,
            static_cast<unsigned long long>(eyes[0].generation),eyes[0].hdr.Get(),
            eyes[0].aliases.empty()?nullptr:eyes[0].aliases.front().Get(),unsigned(eyes[0].postTone.ready()),unsigned(eyes[0].tails.size()),
            static_cast<unsigned long long>(eyes[1].generation),eyes[1].hdr.Get(),
            eyes[1].aliases.empty()?nullptr:eyes[1].aliases.front().Get(),unsigned(eyes[1].postTone.ready()),unsigned(eyes[1].tails.size()),
            bindingShaderHash(BindSlot::Vs),bindingShaderHash(BindSlot::Ps));
    Eye* match=nullptr;
    for(auto& e:eyes)if(e.complete && e.w==w && e.h==h)for(const auto& alias:e.aliases)if(sameIdentity(alias.Get(),submitted)){if(match && match!=&e)return nullptr;match=&e;break;}
    if(!match){if(!routeNoted && (eyes[0].count || eyes[1].count)){routeNoted=true;Log::get().note("Deferred UI: submit route not matched: submitted=%p tone0=%p tone1=%p; original frame retained.",submitted,eyes[0].aliases.empty()?nullptr:eyes[0].aliases.front().Get(),eyes[1].aliases.empty()?nullptr:eyes[1].aliases.front().Get());decline(eyes[eye],"submitted colour has no complete matching replay");}return nullptr;}
    auto& e=*match;Scope scope;const bool sampled=e.postTone.ready();
    lumaProbeSample(ctx,e.cleanHdr.tex.Get(),eye,1);
    UINT stencilRead=0;bool needsDepth=false;
    for(UINT i=0;i<e.count;++i){D3D11_DEPTH_STENCIL_DESC dd{};e.draws[i]->packet.depthState()->GetDesc(&dd);needsDepth=needsDepth || dd.DepthEnable;
        if(dd.StencilEnable && (dd.FrontFace.StencilFunc!=D3D11_COMPARISON_ALWAYS || dd.BackFace.StencilFunc!=D3D11_COMPARISON_ALWAYS))stencilRead|=dd.StencilReadMask;}
    for(UINT i=0;i<e.count;++i){D3D11_DEPTH_STENCIL_DESC dd{};e.draws[i]->packet.depthState()->GetDesc(&dd);
        if(dd.StencilEnable && (dd.StencilWriteMask&stencilRead))for(const auto& face:{dd.FrontFace,dd.BackFace}){
            if(face.StencilFailOp!=D3D11_STENCIL_OP_KEEP || face.StencilDepthFailOp!=D3D11_STENCIL_OP_KEEP ||
               (face.StencilPassOp!=D3D11_STENCIL_OP_KEEP && !(face.StencilPassOp==D3D11_STENCIL_OP_REPLACE && face.StencilFunc==D3D11_COMPARISON_EQUAL && !(stencilRead&dd.StencilWriteMask&~dd.StencilReadMask)))){
                decline(e,"UI changes a stencil bit needed by another UI draw");return nullptr;}
        }}
    Ptr<ID3D11Device> dev;ctx->GetDevice(&dev);D3D11_TEXTURE2D_DESC od{};output->GetDesc(&od);
    auto scaledProjection=[&](const UiDeferredDraw& packet,bool removeJitter,D3D11_VIEWPORT& scaled,D3D11_RECT& sc){
        const float sx=float(od.Width)/e.w,sy=float(od.Height)/e.h;
        const float ox=removeJitter?-jx*sx:0,oy=removeJitter?-jy*sy:0;
        scaled=packet.originalViewport();scaled.TopLeftX=scaled.TopLeftX*sx+ox;scaled.TopLeftY=scaled.TopLeftY*sy+oy;scaled.Width*=sx;scaled.Height*=sy;
        sc=packet.originalScissor();sc.left=LONG(std::floor(sc.left*sx+ox));sc.right=LONG(std::ceil(sc.right*sx+ox));sc.top=LONG(std::floor(sc.top*sy+oy));sc.bottom=LONG(std::ceil(sc.bottom*sy+oy));
    };
    if(!renderer.ensure(ctx,od.Width,od.Height,e.depth.format,sampled,!e.tails.empty())){decline(e,"native output resources unavailable");return nullptr;}
    D3D11_SHADER_RESOURCE_VIEW_DESC sv{};sv.Format=DXGI_FORMAT_R8G8B8A8_UNORM;sv.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D;sv.Texture2D.MipLevels=1;
    Ptr<ID3D11ShaderResourceView> world;if(FAILED(dev->CreateShaderResourceView(output,&sv,&world)))return nullptr;
    auto* c=recorder.Get();c->ClearState();
    if(!renderer.depth.record(c,e.depth.depth.Get(),e.depth.stencil.Get(),w,h,jx,jy,stencilRead,needsDepth))return nullptr;
    c->ClearState();float p[4]={float(od.Width),float(od.Height),jx/w,jy/h};c->UpdateSubresource(renderer.params.Get(),0,nullptr,p,0,0);
    D3D11_VIEWPORT vp{0,0,float(od.Width),float(od.Height),0,1};c->RSSetViewports(1,&vp);
    c->OMSetRenderTargets(1,renderer.hdr.rtv.GetAddressOf(),nullptr);c->VSSetShader(renderer.vs.Get(),nullptr,0);c->PSSetShader(renderer.seed.Get(),nullptr,0);
    c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);c->PSSetConstantBuffers(0,1,renderer.params.GetAddressOf());c->PSSetShaderResources(0,1,e.cleanHdr.srv.GetAddressOf());c->PSSetSamplers(0,1,renderer.sampler.GetAddressOf());c->Draw(3,0);
    c->ClearState();
    if(!e.tone.bind(c,e.tone.originalPixelShader(),renderer.base.rtv.Get(),nullptr,nullptr,vp))return nullptr;
    c->PSSetShaderResources(1,1,renderer.hdr.srv.GetAddressOf());e.tone.draw(c);
    const float one[4]={1,1,1,1};c->ClearRenderTargetView(renderer.transmission.rtv.Get(),one);
    D3D11_VIEWPORT uiVp=vp;uiVp.TopLeftX=-jx*od.Width/w;uiVp.TopLeftY=-jy*od.Height/h;
    for(UINT i=0;i<e.count;++i){const auto& d=*e.draws[i];const auto& m=materials[d.material];c->ClearState();
        const auto originalVp=d.packet.originalViewport();uiVp.MinDepth=originalVp.MinDepth;uiVp.MaxDepth=originalVp.MaxDepth;
        auto sc=d.packet.originalScissor();sc.left=LONG(std::floor(sc.left*float(od.Width)/w+uiVp.TopLeftX));sc.right=LONG(std::ceil(sc.right*float(od.Width)/w+uiVp.TopLeftX));sc.top=LONG(std::floor(sc.top*float(od.Height)/h+uiVp.TopLeftY));sc.bottom=LONG(std::ceil(sc.bottom*float(od.Height)/h+uiVp.TopLeftY));
        if(!d.packet.bind(c,m.fanout.Get(),renderer.hdr.rtv.Get(),renderer.transmission.rtv.Get(),renderer.depth.view(),uiVp,&sc))return nullptr;
        d.packet.setBlend(c,m.transmission.Get());d.packet.draw(c);
    }
    c->ClearState();if(!e.tone.bind(c,e.tone.originalPixelShader(),renderer.ui.rtv.Get(),nullptr,nullptr,vp))return nullptr;
    c->PSSetShaderResources(1,1,renderer.hdr.srv.GetAddressOf());e.tone.draw(c);
    ID3D11ShaderResourceView* base=renderer.base.srv.Get();ID3D11ShaderResourceView* ui=renderer.ui.srv.Get();ID3D11ShaderResourceView* transmission=renderer.transmission.srv.Get();
    if(sampled){
        D3D11_VIEWPORT mappedVp{};D3D11_RECT mappedSc{};scaledProjection(e.postTone,false,mappedVp,mappedSc);
        const struct Mapping { Surface* source;Surface* target; } mappings[]={{&renderer.base,&renderer.mappedBase},{&renderer.ui,&renderer.mappedUi},{&renderer.transmission,&renderer.mappedTransmission}};
        for(const auto& mapping:mappings){c->ClearState();if(!e.postTone.bind(c,e.postTone.originalPixelShader(),mapping.target->rtv.Get(),nullptr,nullptr,mappedVp,&mappedSc)){decline(e,"native sampled mapping bind failed");return nullptr;}auto* source=mapping.source->srv.Get();c->PSSetShaderResources(0,1,&source);e.postTone.draw(c);}
        base=renderer.mappedBase.srv.Get();ui=renderer.mappedUi.srv.Get();transmission=renderer.mappedTransmission.srv.Get();
    }
    c->ClearState();ID3D11ShaderResourceView* views[4]={world.Get(),base,ui,transmission};
    c->CSSetShader(renderer.combine.Get(),nullptr,0);c->CSSetShaderResources(0,4,views);c->CSSetUnorderedAccessViews(0,1,renderer.composite.uav.GetAddressOf(),nullptr);c->CSSetConstantBuffers(0,1,renderer.params.GetAddressOf());c->Dispatch((od.Width+7)/8,(od.Height+7)/8,1);
    for(const auto& tail:e.tails){c->ClearState();D3D11_VIEWPORT tailVp{};D3D11_RECT tailSc{};scaledProjection(*tail,true,tailVp,tailSc);if(!tail->bind(c,tail->originalPixelShader(),renderer.composite.rtv.Get(),nullptr,nullptr,tailVp,&tailSc)){decline(e,"native terminal canvas bind failed");return nullptr;}c->OMSetDepthStencilState(renderer.colourOnly.Get(),0);tail->draw(c);}
    c->ClearState();c->CopyResource(output,renderer.composite.tex.Get());
    if(!finish(eyes[eye].output))return nullptr;
    return sampled?e.cleanFinal.srv.Get():e.cleanLdr.srv.Get();
}
ID3D11ShaderResourceView* uiDeferredPrepare(ID3D11DeviceContext* ctx,ID3D11Texture2D* submitted,
    ID3D11ShaderResourceView* sceneDepth,ID3D11Texture2D* output,int eye,UINT w,UINT h,float jx,float jy) {
    if(eye>=0 && eye<2)eyes[eye].output.Reset();
    try { return prepare(ctx,submitted,sceneDepth,output,eye,w,h,jx,jy); }
    catch(...) { if(eye>=0 && eye<2)decline(eyes[eye],"output replay allocation failed");return nullptr; }
}
void uiDeferredApply(ID3D11DeviceContext* ctx,int eye) {
    if(eye<0 || eye>1 || !eyes[eye].output)return;Scope scope;execute(ctx,eyes[eye].output.Get());eyes[eye].output.Reset();++applied;
    if(!noted){noted=true;Log::get().note("Deferred UI: active. DLSS receives world colour; captured UI draws run at output resolution after reconstruction, with original shaders, tone map and depth/stencil. No deferred UI motion/history pass.");}
}
static void removeAlias(ID3D11DeviceContext*,Eye& e,ID3D11Resource* r,const char* operation) {
    if(!e.complete || !r)return;
    const auto before=e.aliases.size();
    e.aliases.erase(std::remove_if(e.aliases.begin(),e.aliases.end(),[&](const auto& a){return a.Get()==r;}),e.aliases.end());
    if(e.aliases.size()!=before) {
        ++diagnostic.aliasRemovals;
        if(aliasReports++<8)Log::get().note("Deferred UI: tone alias removed enabled=%u gen=%llu eye=%u eye_gen=%llu op=%s target=%p VS=%016llX PS=%016llX remaining=%u.",
            unsigned(enabled),static_cast<unsigned long long>(generation),unsigned(&e-eyes),static_cast<unsigned long long>(e.generation),operation,r,bindingShaderHash(BindSlot::Vs),bindingShaderHash(BindSlot::Ps),unsigned(e.aliases.size()));
    }
    e.complete=!e.aliases.empty();
}
void uiDeferredResourceWrite(ID3D11DeviceContext* ctx,ID3D11Resource* r) {
    if(inside || !r)return;
    if(enabled || diagnosticWindow())for(size_t i=0;i<writerTrace.size();++i)if(writerTrace[i].target.Get()==r){writerTrace[i]=WriterTrace{};if(writerTracePending==static_cast<int>(i))writerTracePending=-1;}
    if(!enabled)return;
    snapshots.written(r);
    for(auto& e:eyes){
        if(!e.restored && e.count && (e.hdr.Get()==r || e.depth.source.Get()==r)){restore(ctx,e);decline(e,"scene overwritten before deferred replay");}
        removeAlias(ctx,e,r,"resource-write");
    }
}
void uiDeferredViewWrite(ID3D11DeviceContext* ctx,ID3D11View* v){if(inside || !enabled || !v)return;Ptr<ID3D11Resource> r;v->GetResource(&r);uiDeferredResourceWrite(ctx,r.Get());}
void uiDeferredCopy(ID3D11Resource* dst,ID3D11Resource* src,bool complete) {
    if(inside || !enabled || !complete || !dst || !src)return;
    for(auto& e:eyes)if(e.count && !e.complete && !e.aborted && sameIdentity(e.hdr.Get(),src))decline(e,"HDR copied to another world input before tone");
    for(auto& e:eyes)if(e.complete && e.aliases.size()<16){bool found=false;for(const auto& a:e.aliases)if(a.Get()==src){found=true;break;}if(found)e.aliases.emplace_back(dst);}
}
void uiDeferredCopyRegion(ID3D11Resource* dst,UINT dstSub,UINT x,UINT y,UINT z,ID3D11Resource* src,UINT srcSub,const D3D11_BOX* box) {
    if(inside || !enabled || !dst || !src || dstSub || srcSub || x || y || z)return;
    bool known=false;for(auto& e:eyes)if(e.complete)for(auto& a:e.aliases)known=known || a.Get()==src;
    if(!known)return;
    Ptr<ID3D11Texture2D> source,dest;if(FAILED(src->QueryInterface(IID_PPV_ARGS(&source))) || FAILED(dst->QueryInterface(IID_PPV_ARGS(&dest))))return;
    D3D11_TEXTURE2D_DESC s{},d{};source->GetDesc(&s);dest->GetDesc(&d);
    if(s.Width!=d.Width || s.Height!=d.Height || s.MipLevels!=1 || d.MipLevels!=1 || s.ArraySize!=1 || d.ArraySize!=1 || s.SampleDesc.Count!=1 || d.SampleDesc.Count!=1)return;
    if(box && (box->left || box->top || box->front || box->right!=s.Width || box->bottom!=s.Height || box->back!=1))return;
    uiDeferredCopy(dst,src,true);
}
static Eye* aliasEye(ID3D11Resource* resource) {
    Eye* match=nullptr;
    for(auto& e:eyes)if(e.complete)for(const auto& alias:e.aliases)if(sameIdentity(alias.Get(),resource)){
        if(match && match!=&e)return nullptr;match=&e;break;
    }
    return match;
}
static bool ldrTarget(ID3D11RenderTargetView* target,ID3D11Texture2D* texture,UINT w,UINT h) {
    if(!target || !texture)return false;
    D3D11_RENDER_TARGET_VIEW_DESC rd{};target->GetDesc(&rd);
    D3D11_TEXTURE2D_DESC td{};texture->GetDesc(&td);
    return rd.Format==DXGI_FORMAT_R8G8B8A8_UNORM && rd.ViewDimension==D3D11_RTV_DIMENSION_TEXTURE2D && !rd.Texture2D.MipSlice &&
        (td.Format==DXGI_FORMAT_R8G8B8A8_TYPELESS || td.Format==DXGI_FORMAT_R8G8B8A8_UNORM) &&
        td.Width==w && td.Height==h && td.MipLevels==1 && td.ArraySize==1 && td.SampleDesc.Count==1;
}
static bool unormTexture2d(ID3D11ShaderResourceView* view) {
    if(!view)return false;D3D11_SHADER_RESOURCE_VIEW_DESC d{};view->GetDesc(&d);
    return d.Format==DXGI_FORMAT_R8G8B8A8_UNORM && d.ViewDimension==D3D11_SRV_DIMENSION_TEXTURE2D && !d.Texture2D.MostDetailedMip && d.Texture2D.MipLevels==1;
}
static bool fullViewport(ID3D11DeviceContext* ctx,UINT w,UINT h) {
    D3D11_VIEWPORT vp{};UINT n=1;ctx->RSGetViewports(&n,&vp);
    return n==1 && vp.TopLeftX==0 && vp.TopLeftY==0 && vp.Width==w && vp.Height==h;
}
static bool postToneState(ID3D11DeviceContext* ctx) {
    D3D11_PRIMITIVE_TOPOLOGY topology{};ctx->IAGetPrimitiveTopology(&topology);
    Ptr<ID3D11DepthStencilState> depth;UINT stencil=0;ctx->OMGetDepthStencilState(&depth,&stencil);
    D3D11_DEPTH_STENCIL_DESC dd{};if(depth)depth->GetDesc(&dd);else dd.DepthEnable=TRUE;
    Ptr<ID3D11BlendState> blend;UINT sampleMask=0;ctx->OMGetBlendState(&blend,nullptr,&sampleMask);D3D11_BLEND_DESC bd{};if(blend)blend->GetDesc(&bd);
    Ptr<ID3D11BlendState1> blend1;if(blend)blend.As(&blend1);D3D11_BLEND_DESC1 bd1{};if(blend1)blend1->GetDesc1(&bd1);
    Ptr<ID3D11RasterizerState> raster;ctx->RSGetState(&raster);D3D11_RASTERIZER_DESC rd{};if(raster)raster->GetDesc(&rd);else rd.FillMode=D3D11_FILL_SOLID;
    return topology==D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP && !dd.DepthEnable && !dd.StencilEnable &&
        !bd.AlphaToCoverageEnable && sampleMask==~0u && (!blend1 || !bd1.RenderTarget[0].LogicOpEnable) && rd.FillMode==D3D11_FILL_SOLID && !rd.ScissorEnable &&
        (!blend || (!bd.RenderTarget[0].BlendEnable && bd.RenderTarget[0].RenderTargetWriteMask==15));
}
static bool tailState(ID3D11DeviceContext* ctx,ID3D11DepthStencilView* view) {
    D3D11_PRIMITIVE_TOPOLOGY topology{};ctx->IAGetPrimitiveTopology(&topology);
    Ptr<ID3D11DepthStencilState> depth;UINT stencil=0;ctx->OMGetDepthStencilState(&depth,&stencil);if(!depth || !view)return false;
    D3D11_DEPTH_STENCIL_DESC dd{};depth->GetDesc(&dd);D3D11_DEPTH_STENCIL_VIEW_DESC vd{};view->GetDesc(&vd);
    Ptr<ID3D11BlendState> blend;UINT sampleMask=0;ctx->OMGetBlendState(&blend,nullptr,&sampleMask);if(!blend)return false;D3D11_BLEND_DESC bd{};blend->GetDesc(&bd);const auto& b=bd.RenderTarget[0];
    Ptr<ID3D11BlendState1> blend1;blend.As(&blend1);D3D11_BLEND_DESC1 bd1{};if(blend1)blend1->GetDesc1(&bd1);
    const auto stencilFace=[](const D3D11_DEPTH_STENCILOP_DESC& f){return f.StencilFailOp==D3D11_STENCIL_OP_KEEP && f.StencilDepthFailOp==D3D11_STENCIL_OP_KEEP && f.StencilPassOp==D3D11_STENCIL_OP_REPLACE && f.StencilFunc==D3D11_COMPARISON_ALWAYS;};
    return topology==D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST && !dd.DepthEnable && dd.StencilEnable && dd.StencilReadMask==0 && dd.StencilWriteMask==4 && stencil==4 &&
        stencilFace(dd.FrontFace) && stencilFace(dd.BackFace) && vd.Format==DXGI_FORMAT_D32_FLOAT_S8X24_UINT && !vd.Flags && vd.ViewDimension==D3D11_DSV_DIMENSION_TEXTURE2D && !vd.Texture2D.MipSlice &&
        !bd.AlphaToCoverageEnable && sampleMask==~0u && (!blend1 || !bd1.RenderTarget[0].LogicOpEnable) && b.BlendEnable && b.SrcBlend==D3D11_BLEND_ONE && b.DestBlend==D3D11_BLEND_INV_SRC_ALPHA && b.BlendOp==D3D11_BLEND_OP_ADD &&
        b.SrcBlendAlpha==D3D11_BLEND_ONE && b.DestBlendAlpha==D3D11_BLEND_INV_SRC_ALPHA && b.BlendOpAlpha==D3D11_BLEND_OP_ADD && b.RenderTargetWriteMask==7;
}
static int captureRouteDrawImpl(ID3D11DeviceContext* ctx,char kind,UINT count,UINT instances,UINT start,INT base,UINT first) {
    const uint64_t vs=bindingShaderHash(BindSlot::Vs),ps=bindingShaderHash(BindSlot::Ps);
    const bool post=vs==kPostToneVs && ps==kPostTonePs,tail=vs==kTailVs && ps==kTailPs;
    if(!enabled || (!post && !tail))return 0;
    Ptr<ID3D11RenderTargetView> rt;Ptr<ID3D11DepthStencilView> ds;Ptr<ID3D11Texture2D> tex;
    if(!target(ctx,rt,ds,tex))return 0;
    Ptr<ID3D11ShaderResourceView> v0,v1;ctx->PSGetShaderResources(0,1,&v0);ctx->PSGetShaderResources(1,1,&v1);
    Ptr<ID3D11Resource> targetResource,s0,s1;rt->GetResource(&targetResource);if(v0)v0->GetResource(&s0);if(v1)v1->GetResource(&s1);
    Eye* e=post?aliasEye(s0.Get()):aliasEye(targetResource.Get());if(!e)return 0;
    auto reject=[&](const char* why){restore(ctx,*e);decline(*e,why);return -1;};
    if(sameIdentity(targetResource.Get(),s0.Get()) || (tail && sameIdentity(targetResource.Get(),s1.Get())))return reject("native route samples its render target");
    if(!ldrTarget(rt.Get(),tex.Get(),e->w,e->h) || !fullViewport(ctx,e->w,e->h))return reject(post?"post-tone target or viewport unsupported":"terminal canvas target or viewport unsupported");
    Scope scope;const auto* reflected=reflect(ctx);if(!reflected)return reject(post?"post-tone shader inputs unavailable":"terminal canvas shader inputs unavailable");
    if(post) {
        if(e->postTone.ready())return reject("second post-tone sampled draw unsupported");
        if(kind!='N' || count!=4 || instances!=1 || ds || !unormTexture2d(v0.Get()) || !postToneState(ctx))return reject("post-tone draw state unsupported");
        Ptr<ID3D11Device> dev;ctx->GetDevice(&dev);if(!e->cleanFinal.ensure(dev.Get(),e->w,e->h,DXGI_FORMAT_R8G8B8A8_UNORM))return reject("clean post-tone target unavailable");
        UiDeferredCaptureMask mask=*reflected;mask.postTone=true;e->postTone=UiDeferredDraw{};
        if(!e->postTone.capture(ctx,snapshots,kind,count,instances,start,base,first,mask))return reject("post-tone draw capture failed");
        recorder->ClearState();auto scissor=e->postTone.originalScissor();
        bool valid=e->postTone.bind(recorder.Get(),e->postTone.originalPixelShader(),e->cleanFinal.rtv.Get(),nullptr,nullptr,e->postTone.originalViewport(),&scissor);
        auto* clean=e->cleanLdr.srv.Get();recorder->PSSetShaderResources(0,1,&clean);e->postTone.draw(recorder.Get());
        Ptr<ID3D11CommandList> list;valid=valid && finish(list);if(!valid)return reject("clean post-tone replay failed");
        std::vector<Ptr<ID3D11Resource>> nextAliases;nextAliases.push_back(targetResource);execute(ctx,list.Get());
        snapshots.written(targetResource.Get());e->aliases.swap(nextAliases);++diagnostic.postToneCaptures;
        if(routeCaptureReports++<12)Log::get().note("Deferred UI: retained post-tone sampled draw gen=%llu eye=%u source=%p target=%p tails=%u.",static_cast<unsigned long long>(generation),unsigned(e-eyes),s0.Get(),targetResource.Get(),unsigned(e->tails.size()));
    } else {
        if(!e->postTone.ready())return reject("terminal canvas arrived before retained post-tone draw");
        if(kind!='X' || count!=6 || instances!=1 || !tailState(ctx,ds.Get()))return reject("terminal canvas draw state unsupported");
        if(e->tails.size()>=kTailDrawLimit)return reject("terminal canvas draw limit exceeded");
        UiDeferredCaptureMask mask=*reflected;mask.allowVolatileUavSrv=true;
        auto packet=std::make_unique<UiDeferredDraw>();if(!packet->capture(ctx,snapshots,kind,count,instances,start,base,first,mask))return reject("terminal canvas draw capture failed");
        e->tails.push_back(std::move(packet));snapshots.written(targetResource.Get());Ptr<ID3D11Resource> depthResource;ds->GetResource(&depthResource);snapshots.written(depthResource.Get());++diagnostic.tailCaptures;
        if(routeCaptureReports++<12)Log::get().note("Deferred UI: retained terminal canvas gen=%llu eye=%u target=%p tail=%u/%u.",static_cast<unsigned long long>(generation),unsigned(e-eyes),targetResource.Get(),unsigned(e->tails.size()),unsigned(kTailDrawLimit));
    }
    routeHandledThisDraw=true;return 1;
}
// The post-tone pixel shader under a vertex shader this file does not
// recognise (seen with EDHM chained, docs/edhm-black-cockpit-2026-09-15.md):
// the copy is never captured, so the post-tone mapping never joins the
// replay. Once per distinct vertex shader, four at most.
static void notePostToneVsMismatch(uint64_t vs,uint64_t ps) {
    static uint64_t seen[4]{};static int seenCount=0;
    for(int i=0;i<seenCount;++i)if(seen[i]==vs)return;
    if(seenCount>=4)return;
    seen[seenCount++]=vs;
    Log::get().note("Deferred UI: post-tone pixel shader %016llX bound with an unrecognised vertex shader %016llX; the post-tone copy is not captured, so DLSS receives the unmapped world colour.",
        static_cast<unsigned long long>(ps),static_cast<unsigned long long>(vs));
}
static int captureRouteDraw(ID3D11DeviceContext* ctx,char kind,UINT count,UINT instances,UINT start,INT base,UINT first) noexcept {
    try { return captureRouteDrawImpl(ctx,kind,count,instances,start,base,first); }
    catch(...) {
        routeHandledThisDraw=false;
        for(auto& e:eyes)if(e.complete && !e.aborted){restore(ctx,e);decline(e,"native route capture allocation failed");}
        return -1;
    }
}
void uiDeferredBeforeDraw(ID3D11DeviceContext* ctx,char kind,UINT count,UINT instances,UINT start,INT base,UINT first,uint32_t verdict) {
    routeHandledThisDraw=false;
    if(inside)return;
    const bool active=eyes[0].count || eyes[1].count;
    if(!active && !diagnosticWindow())return;
    if(enabled && active){ID3D11UnorderedAccessView* uavs[8]{};ctx->OMGetRenderTargetsAndUnorderedAccessViews(0,nullptr,nullptr,0,8,uavs);for(auto* v:uavs)if(v){uiDeferredViewWrite(ctx,v);v->Release();}}
    const uint64_t vs=bindingShaderHash(BindSlot::Vs),ps=bindingShaderHash(BindSlot::Ps);
    const bool postTone=vs==kPostToneVs && ps==kPostTonePs;
    const bool lateComposite=vs==kTailVs && ps==kTailPs;
    if(enabled && ps==kPostTonePs && vs!=kPostToneVs)notePostToneVsMismatch(vs,ps);
    auto& reports=postTone?postToneReports:lateCompositeReports;
    const bool diagnosticCandidate=postTone || lateComposite;
    const bool report=diagnosticCandidate && reports<8;
    if(!enabled && !report){
        if(postTone)++diagnostic.postToneCandidates;
        if(lateComposite)++diagnostic.lateCompositeCandidates;
        return;
    }
    ID3D11RenderTargetView* rawViews[8]{};Ptr<ID3D11DepthStencilView> depth;ctx->OMGetRenderTargets(8,rawViews,&depth);
    std::array<Ptr<ID3D11RenderTargetView>,8> views;for(UINT i=0;i<8;++i)views[i].Attach(rawViews[i]);
    if(postTone || lateComposite) {
        auto& observations=postTone?diagnostic.postToneCandidates:diagnostic.lateCompositeCandidates;
        ++observations;
        if(report){
            Ptr<ID3D11Resource> target,s0,s1;Ptr<ID3D11ShaderResourceView> v0,v1;
            if(views[0])views[0]->GetResource(&target);
            ctx->PSGetShaderResources(0,1,&v0);ctx->PSGetShaderResources(1,1,&v1);
            if(v0)v0->GetResource(&s0);if(v1)v1->GetResource(&s1);
            ++reports;Log::get().note("Deferred UI: %s candidate enabled=%u gen=%llu verdict=%u target=%p t0=%p t1=%p; eye0 gen=%llu HDR=%p tone=%p draws=%u, eye1 gen=%llu HDR=%p tone=%p draws=%u; VS=%016llX PS=%016llX.",
            postTone?"post-tone copy":"late composite",unsigned(enabled),static_cast<unsigned long long>(generation),verdict,target.Get(),s0.Get(),s1.Get(),
            static_cast<unsigned long long>(eyes[0].generation),eyes[0].hdr.Get(),eyes[0].aliases.empty()?nullptr:eyes[0].aliases.front().Get(),eyes[0].count,
            static_cast<unsigned long long>(eyes[1].generation),eyes[1].hdr.Get(),eyes[1].aliases.empty()?nullptr:eyes[1].aliases.front().Get(),eyes[1].count,
            vs,ps);if(postTone)reportProducer(s0.Get());}
    }
    if(!enabled)return;
    if(verdict && diagnosticCandidate && routeCaptureReports++<12)Log::get().note("Deferred UI: %s route capture refused gen=%llu verdict=%u; original fallback retained.",postTone?"post-tone":"terminal canvas",static_cast<unsigned long long>(generation),verdict);
    const int route=!verdict?captureRouteDraw(ctx,kind,count,instances,start,base,first):0;
    if(route)return;
    if(depth){Ptr<ID3D11Resource> r;depth->GetResource(&r);snapshots.written(r.Get());}
    for(auto& v:views)if(v){Ptr<ID3D11Resource> r;v->GetResource(&r);snapshots.written(r.Get());
        for(auto& e:eyes)removeAlias(ctx,e,r.Get(),"draw-target");
    }
}
void uiDeferredBeforeDispatch(ID3D11DeviceContext* ctx) {
    if(inside || !enabled || (!eyes[0].count && !eyes[1].count))return;
    ID3D11UnorderedAccessView* views[8]{};ctx->CSGetUnorderedAccessViews(0,8,views);
    for(auto* v:views)if(v){uiDeferredViewWrite(ctx,v);v->Release();}
}
void uiDeferredUnknownWrite(ID3D11DeviceContext* ctx){if(inside || !enabled)return;snapshots.unknownWrite();for(auto& e:eyes)if(e.count){restore(ctx,e);decline(e,"untracked command");}}
void uiDeferredFrameBoundary(ID3D11DeviceContext* ctx) {
    static uint64_t frames=0;
    if(enabled && ++frames%600==0)Log::get().note("Deferred UI: totals captured=%llu applied=%llu declined=%llu, snapshots copied=%.2f MiB allocated=%.2f MiB.",captured,applied,declined,snapshots.copiedBytes()/1048576.,snapshots.allocatedBytes()/1048576.);
    if((eyes[0].count || eyes[1].count || eyes[0].complete || eyes[1].complete || diagnosticWindow())) {
        ++diagnostic.boundaries;
        if(boundaryReports++<4)Log::get().note("Deferred UI: frame boundary enabled=%u gen=%llu; eye0 gen=%llu HDR=%p draws=%u complete=%u aliases=%u restored=%u aborted=%u, eye1 gen=%llu HDR=%p draws=%u complete=%u aliases=%u restored=%u aborted=%u.",
            unsigned(enabled),static_cast<unsigned long long>(generation),static_cast<unsigned long long>(eyes[0].generation),eyes[0].hdr.Get(),eyes[0].count,unsigned(eyes[0].complete),unsigned(eyes[0].aliases.size()),unsigned(eyes[0].restored),unsigned(eyes[0].aborted),
            static_cast<unsigned long long>(eyes[1].generation),eyes[1].hdr.Get(),eyes[1].count,unsigned(eyes[1].complete),unsigned(eyes[1].aliases.size()),unsigned(eyes[1].restored),unsigned(eyes[1].aborted));
    }
    for(auto& e:eyes){if(e.count && !e.restored)restore(ctx,e);for(UINT i=0;i<e.count;++i){e.draws[i]->packet=UiDeferredDraw{};}e.tone=UiDeferredDraw{};e.postTone=UiDeferredDraw{};e.tails.clear();e.count=e.changedStencil=0;e.bytes=0;e.complete=e.restored=e.aborted=false;e.aliases.clear();e.output.Reset();}
    snapshots.frameBoundary();
    ++generation;
    writerTraceOrdinal=0;writerTracePending=-1;
    if(!enabled && !diagnosticWindow())clearWriterTrace();
}
void uiDeferredShutdown(){for(auto& e:eyes)e=Eye{};renderer=Renderer{};materials.clear();masks.clear();captureFailures.clear();snapshots=UiDeferredSnapshots{};recorder.Reset();savedBlend.Reset();savedPs.Reset();savedTarget.Reset();savedDepth.Reset();replayTarget.Reset();worldDrawMode=WorldDrawMode::None;enabled=failed=resetPending[0]=resetPending[1]=colourMuted=noted=routeNoted=routeHandledThisDraw=false;captured=applied=declined=glassReplays=0;glassReplayReports=0;diagnostic={};generation=1;diagnosticUntilGeneration=0;toneReports=aliasReports=prepareReports=postToneReports=lateCompositeReports=boundaryReports=routeCaptureReports=0;clearWriterTrace();writerTraceSerial=producerMatches=producerMisses=writerTargetQueries=writerShaderQueries=0;writerShaderKeys={};writerShaderCount=writerShaderReports=0;writerShaderDir.clear();}
}
