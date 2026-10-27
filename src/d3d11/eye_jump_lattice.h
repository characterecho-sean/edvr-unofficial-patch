#pragma once

#include <d3d11.h>
#include <wrl/client.h>
#include <cstdint>
#include <cstring>

namespace edvr { namespace eye_jump_lattice {
constexpr uint32_t Count=16, Stride=2;
constexpr uint64_t Budget=320ull*1024*1024;
constexpr DXGI_FORMAT SkinFormat=DXGI_FORMAT_R16G16B16A16_FLOAT;
constexpr DXGI_FORMAT MotionFormat=DXGI_FORMAT_R16G16_FLOAT;
constexpr DXGI_FORMAT SlotFormat=DXGI_FORMAT_R32G32_FLOAT;
constexpr DXGI_FORMAT PackedSkinFormat=DXGI_FORMAT_R32G32_UINT;
constexpr DXGI_FORMAT PackedMotionFormat=DXGI_FORMAT_R32_UINT;
constexpr DXGI_FORMAT PackedSlotFormat=DXGI_FORMAT_R32G32_UINT;

inline uint64_t rowBytes(uint32_t width,uint32_t height) noexcept {
    const uint64_t w=(uint64_t(width)+1)/2,h=(uint64_t(height)+1)/2;
    if(h && w>UINT64_MAX/h/20)return UINT64_MAX;
    return w*h*20;
}
inline bool runFits(uint32_t width,uint32_t height) noexcept {
    return width && height && rowBytes(width,height)<=Budget/Count;
}
inline const char* resultName(uint32_t result) noexcept {
    static const char* names[]={"not_reached","staged","missing_input","source_shape","source_format",
        "dimensions_changed","budget","shader_unavailable","source_bound_output","scratch_create_failed",
        "stage_create_failed","capture_fault","join_mismatch","map_not_ready","write_failed","written","native_inactive"};
    return result<sizeof(names)/sizeof(names[0])?names[result]:"invalid_status";
}
enum Result:uint32_t { NotReached,Staged,MissingInput,SourceShape,SourceFormat,DimensionsChanged,BudgetRefused,
                       ShaderUnavailable,SourceBoundOutput,ScratchCreateFailed,StageCreateFailed,CaptureFault,
                       JoinMismatch,MapNotReady,WriteFailed,Written,NativeInactive };
struct Row {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> skin,motion,slots;
    uint32_t sceneFrame=0,renderFrame=0,epoch=0,width=0,height=0,sampleWidth=0,sampleHeight=0;
    uint64_t sequence=0,bytes=0;
    uint32_t status=NotReached;
};
inline bool finalSequenceMatch(const Row& row,bool scheduled,uint32_t scene,uint32_t render,uint32_t epoch,
                               uint64_t leftSequence,uint64_t rightSequence,uint64_t& sequence) noexcept {
    sequence=0;
    if(row.status!=Staged||!scheduled||scene!=row.sceneFrame||render!=row.renderFrame||epoch!=row.epoch||
       !leftSequence||leftSequence!=rightSequence)return false;
    sequence=leftSequence;return true;
}

class Run {
    using Texture=Microsoft::WRL::ComPtr<ID3D11Texture2D>;
    using Uav=Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView>;
    Texture scratch_[3];
    Uav scratchUav_[3];
    uint32_t sourceWidth_=0,sourceHeight_=0,scratchWidth_=0,scratchHeight_=0,sampleWidth_=0,sampleHeight_=0;
    uint64_t reserved_=0;
    bool armed_=false;
    static bool inputTexture(ID3D11ShaderResourceView* view,DXGI_FORMAT format,uint32_t& width,uint32_t& height,
                             Microsoft::WRL::ComPtr<ID3D11Texture2D>& texture) {
        if(!view)return false;
        D3D11_SHADER_RESOURCE_VIEW_DESC vd{};view->GetDesc(&vd);
        if(vd.Format!=format||vd.ViewDimension!=D3D11_SRV_DIMENSION_TEXTURE2D||vd.Texture2D.MostDetailedMip!=0)return false;
        Microsoft::WRL::ComPtr<ID3D11Resource> resource;view->GetResource(&resource);
        if(!resource||FAILED(resource.As(&texture)))return false;
        D3D11_TEXTURE2D_DESC d{};texture->GetDesc(&d);
        if(d.Format!=format||d.MipLevels!=1||d.ArraySize!=1||d.SampleDesc.Count!=1||d.SampleDesc.Quality)return false;
        width=d.Width;height=d.Height;return true;
    }
    static bool sourceIsOutput(ID3D11DeviceContext* ctx,ID3D11Texture2D* a,ID3D11Texture2D* b,ID3D11Texture2D* c) {
        ID3D11RenderTargetView* rt[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};ID3D11DepthStencilView* ds=nullptr;
        ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT,rt,&ds);
        bool conflict=false;
        for(auto* view:rt)if(view){ID3D11Resource* resource=nullptr;view->GetResource(&resource);
            conflict=conflict||resource==a||resource==b||resource==c;if(resource)resource->Release();view->Release();}
        if(ds){ID3D11Resource* resource=nullptr;ds->GetResource(&resource);conflict=conflict||resource==a||resource==b||resource==c;
            if(resource)resource->Release();ds->Release();}
        return conflict;
    }
    bool ensureScratch(ID3D11DeviceContext* ctx,uint32_t width,uint32_t height) {
        if(scratchWidth_==width&&scratchHeight_==height&&scratch_[0]&&scratch_[1]&&scratch_[2])return true;
        for(auto& u:scratchUav_)u.Reset();for(auto& t:scratch_)t.Reset();
        ID3D11Device* raw=nullptr;ctx->GetDevice(&raw);if(!raw)return false;
        Microsoft::WRL::ComPtr<ID3D11Device> device;device.Attach(raw);
        const DXGI_FORMAT formats[3]={PackedSkinFormat,PackedMotionFormat,PackedSlotFormat};
        for(unsigned i=0;i<3;++i){
            D3D11_TEXTURE2D_DESC d{};d.Width=width;d.Height=height;d.MipLevels=d.ArraySize=d.SampleDesc.Count=1;
            d.Format=formats[i];d.Usage=D3D11_USAGE_DEFAULT;d.BindFlags=D3D11_BIND_UNORDERED_ACCESS;
            if(FAILED(device->CreateTexture2D(&d,nullptr,&scratch_[i]))||!scratch_[i])return false;
            if(FAILED(device->CreateUnorderedAccessView(scratch_[i].Get(),nullptr,&scratchUav_[i]))||!scratchUav_[i])return false;
        }
        scratchWidth_=width;scratchHeight_=height;return true;
    }
    static bool makeStaging(ID3D11DeviceContext* ctx,DXGI_FORMAT format,uint32_t width,uint32_t height,Texture& out) {
        ID3D11Device* raw=nullptr;ctx->GetDevice(&raw);if(!raw)return false;
        Microsoft::WRL::ComPtr<ID3D11Device> device;device.Attach(raw);
        D3D11_TEXTURE2D_DESC d{};d.Width=width;d.Height=height;d.MipLevels=d.ArraySize=d.SampleDesc.Count=1;
        d.Format=format;d.Usage=D3D11_USAGE_STAGING;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        return SUCCEEDED(device->CreateTexture2D(&d,nullptr,&out))&&bool(out);
    }
public:
    Row rows[Count]{};
    uint32_t count=0;
    uint64_t bytes=0;
    bool armed()const noexcept{return armed_;}
    void arm(){reset();armed_=true;}
    void refuse(uint32_t index,uint32_t scene,uint32_t render,uint32_t epoch,Result why) noexcept {
        if(index>=Count||!armed_)return;Row& row=rows[index];row.sceneFrame=scene;row.renderFrame=render;row.epoch=epoch;row.status=why;
        if(index>=count)count=index+1;
    }
    void reset(){for(auto& r:rows)r=Row{};for(auto& u:scratchUav_)u.Reset();for(auto& t:scratch_)t.Reset();
        sourceWidth_=sourceHeight_=scratchWidth_=scratchHeight_=sampleWidth_=sampleHeight_=count=0;reserved_=bytes=0;armed_=false;}

    Result stage(ID3D11DeviceContext* ctx,ID3D11ComputeShader* shader,uint32_t index,
                 uint32_t scene,uint32_t render,uint32_t epoch,bool engineBound,
                 ID3D11ShaderResourceView* skinView,ID3D11ShaderResourceView* motionView,ID3D11ShaderResourceView* slotView) {
        if(index>=Count||!armed_)return NotReached;
        Row& row=rows[index];row.sceneFrame=scene;row.renderFrame=render;row.epoch=epoch;
        if(index>=count)count=index+1;
        if(!ctx||!skinView||!motionView||!slotView){row.status=MissingInput;return MissingInput;}
        if(!engineBound){row.status=MissingInput;return MissingInput;}
        if(ctx->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE){row.status=SourceShape;return SourceShape;}
        Microsoft::WRL::ComPtr<ID3D11Texture2D> skinSource,motionSource,slotSource;
        uint32_t sw=0,sh=0,mw=0,mh=0,tw=0,th=0;
        if(!inputTexture(skinView,SkinFormat,sw,sh,skinSource)||!inputTexture(motionView,MotionFormat,mw,mh,motionSource)||
           !inputTexture(slotView,SlotFormat,tw,th,slotSource)){row.status=SourceFormat;return SourceFormat;}
        if(sw!=mw||sw!=tw||sh!=mh||sh!=th){row.status=SourceShape;return SourceShape;}
        if(!runFits(sw,sh)){row.status=BudgetRefused;return BudgetRefused;}
        if(sourceWidth_&&(sourceWidth_!=sw||sourceHeight_!=sh)){row.status=DimensionsChanged;return DimensionsChanged;}
        const uint32_t ow=static_cast<uint32_t>((uint64_t(sw)+1)/2),oh=static_cast<uint32_t>((uint64_t(sh)+1)/2);const uint64_t cost=rowBytes(sw,sh);
        if(cost>Budget-reserved_){row.status=BudgetRefused;return BudgetRefused;}
        if(!shader){row.status=ShaderUnavailable;return ShaderUnavailable;}
        if(sourceIsOutput(ctx,skinSource.Get(),motionSource.Get(),slotSource.Get())){row.status=SourceBoundOutput;return SourceBoundOutput;}
        if(!ensureScratch(ctx,ow,oh)){row.status=ScratchCreateFailed;return ScratchCreateFailed;}
        if(!makeStaging(ctx,PackedSkinFormat,ow,oh,row.skin)||!makeStaging(ctx,PackedMotionFormat,ow,oh,row.motion)||
           !makeStaging(ctx,PackedSlotFormat,ow,oh,row.slots)){row.status=StageCreateFailed;return StageCreateFailed;}
        row.width=sw;row.height=sh;row.sampleWidth=ow;row.sampleHeight=oh;row.bytes=cost;

        ID3D11ComputeShader* oldShader=nullptr;ID3D11ClassInstance* oldClasses[256]{};UINT oldClassCount=256;
        ctx->CSGetShader(&oldShader,oldClasses,&oldClassCount);
        ID3D11ShaderResourceView* oldSrvs[3]{};ctx->CSGetShaderResources(0,3,oldSrvs);
        ID3D11UnorderedAccessView* oldUavs[8]{};ctx->CSGetUnorderedAccessViews(0,8,oldUavs);
        ID3D11UnorderedAccessView* clearUavs[8]{};UINT keep[8];for(auto& n:keep)n=D3D11_KEEP_UNORDERED_ACCESS_VIEWS;
        ID3D11ShaderResourceView* inputs[3]={skinView,motionView,slotView};
        ID3D11UnorderedAccessView* outputs[3]={scratchUav_[0].Get(),scratchUav_[1].Get(),scratchUav_[2].Get()};
        ctx->CSSetUnorderedAccessViews(0,8,clearUavs,keep);
        ctx->CSSetShaderResources(0,3,inputs);ctx->CSSetUnorderedAccessViews(0,3,outputs,keep);ctx->CSSetShader(shader,nullptr,0);
        ctx->Dispatch((ow+7)/8,(oh+7)/8,1);
        ID3D11UnorderedAccessView* nullUavs[3]{};UINT nullKeep[3]={D3D11_KEEP_UNORDERED_ACCESS_VIEWS,D3D11_KEEP_UNORDERED_ACCESS_VIEWS,D3D11_KEEP_UNORDERED_ACCESS_VIEWS};
        ctx->CSSetUnorderedAccessViews(0,3,nullUavs,nullKeep);
        ctx->CopyResource(row.skin.Get(),scratch_[0].Get());ctx->CopyResource(row.motion.Get(),scratch_[1].Get());ctx->CopyResource(row.slots.Get(),scratch_[2].Get());
        ctx->CSSetShaderResources(0,3,oldSrvs);ctx->CSSetUnorderedAccessViews(0,8,oldUavs,keep);
        ctx->CSSetShader(oldShader,oldClasses,oldClassCount);
        if(oldShader)oldShader->Release();for(auto* c:oldClasses)if(c)c->Release();
        for(auto* v:oldSrvs)if(v)v->Release();for(auto* v:oldUavs)if(v)v->Release();
        if(!sourceWidth_){sourceWidth_=sw;sourceHeight_=sh;sampleWidth_=ow;sampleHeight_=oh;}
        reserved_+=cost;bytes=reserved_;row.status=Staged;if(index>=count)count=index+1;return Staged;
    }
};
} }
