#pragma once
#include "flat_mono_resolve.h"
#include "flat_compute_readback.h"
#include "../common/config.h"
#include "../common/log.h"
#include <wrl/client.h>
#include <array>
#include <deque>
#include <vector>
#include <string>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <algorithm>

namespace edvr {
// Manual, owner-thread diagnostics. Full input planes, before any backend work.
// Only owned staging resources and copied numeric metadata survive capture().
class FlatResolveInputCapture {
    using Ptr = Microsoft::WRL::ComPtr<ID3D11Texture2D>;
    static constexpr uint64_t byteCap = 384ull << 20;
    static constexpr uint64_t chunkCap = 64ull << 20;
    struct Item {
        const char* name = nullptr;
        Microsoft::WRL::ComPtr<ID3D11Resource> stage;
        D3D11_TEXTURE2D_DESC desc{};
        D3D11_BUFFER_DESC buffer{};
        D3D11_SHADER_RESOURCE_VIEW_DESC view{};
        uint32_t row = 0;
        uint64_t bytes = 0;
        uint32_t crc = 0;
        bool present = false;
        bool isBuffer = false;
        const char* status = "absent";
        std::vector<std::string> files;
    };
    struct Sample {
        uint64_t frame = 0, queuedMs = 0;
        uint32_t ordinal = 0, renderW = 0, renderH = 0, outputW = 0, outputH = 0;
        FlatMonoResolveMode mode = FlatMonoResolveMode::Taa;
        bool hdr = false, reset = false;
        bool staticScene = false, steadyDetail = false;
        uint32_t slot = 0;
        float deltaMs = 0;
        float jitter[2]{}, rowsJitter[2]{}, previousJitter[2]{}, previousRowsJitter[2]{};
        float camera[6][4]{}, previousCamera[6][4]{};
        std::array<Item,9> items{};
        std::string manifest;
        const char* status = "queued";
    };
    std::deque<Sample> pending_;
    std::vector<std::string> manifests_;
    std::wstring directory_;
    uint64_t armFrame_ = 0, armMs_ = 0, lastFrame_ = ~0ull, reserved_ = 0;
    uint64_t copyOperations_ = 0;
    uint64_t budget_ = byteCap;
    uint32_t attempts_ = 0, serial_ = 0;
    bool active_ = false;

    static const char* modeName(FlatMonoResolveMode mode) {
        switch(mode) {
        case FlatMonoResolveMode::Dlaa: return "dlaa";
        case FlatMonoResolveMode::Dlss: return "dlss";
        case FlatMonoResolveMode::Fsr: return "fsr";
        default: return "taa";
        }
    }
    static std::string number(float value) {
        if(!std::isfinite(value))return "null";
        char text[48]{};std::snprintf(text,sizeof(text),"%.9g",value);return text;
    }
    static std::string cameraJson(const float (&camera)[6][4]) {
        std::string out="[";
        for(unsigned r=0;r<6;++r) {
            if(r)out+=",";out+="[";
            for(unsigned c=0;c<4;++c) {if(c)out+=",";out+=number(camera[r][c]);}
            out+="]";
        }
        return out+"]";
    }
    std::wstring path(const std::string& leaf) const {
        return directory_+L"\\"+std::wstring(leaf.begin(),leaf.end());
    }
    bool atomic(const std::wstring& name,const std::string& body) {
        FILE* file=nullptr;
        if(_wfopen_s(&file,(name+L".tmp").c_str(),L"wb") || !file)return false;
        bool ok=std::fwrite(body.data(),1,body.size(),file)==body.size();
        ok=std::fclose(file)==0 && ok;
        if(ok)ok=MoveFileExW((name+L".tmp").c_str(),name.c_str(),MOVEFILE_REPLACE_EXISTING)!=FALSE;
        return ok;
    }
    bool session(const char* status) {
        std::string body="{\"schema\":\"edvr-flat-resolve-input-session\",\"version\":1,\"stage\":\"prebackend\",\"backend_status\":\"not-run\",\"status\":\"";
        body+=status;body+="\",\"byte_cap\":"+std::to_string(budget_)+",\"reserved_bytes\":"+std::to_string(reserved_);
        body+=",\"attempts\":"+std::to_string(attempts_)+",\"pending\":"+std::to_string(pending_.size())+",\"samples\":[";
        for(size_t i=0;i<manifests_.size();++i) {if(i)body+=",";body+="\""+manifests_[i]+"\"";}
        body+="]}\n";
        const bool ok=atomic(directory_+L"\\manifest.json",body);
        if(!ok)Log::get().note("flat resolve inputs: session metadata publication failed status=%s directory=%ls",status,directory_.c_str());
        return ok;
    }
    bool manifest(const Sample& s) {
        std::string body="{\"schema\":\"edvr-flat-resolve-inputs\",\"version\":1,\"stage\":\"prebackend\",\"backend_status\":\"not-run\",\"status\":\"";
        body+=s.status;body+="\",\"complete\":";body+=std::string(s.status)=="complete"?"true":"false";
        body+=",\"frame\":"+std::to_string(s.frame)+",\"mode\":\""+modeName(s.mode)+"\",\"hdr\":"+(s.hdr?"true":"false");
        body+=",\"reset\":"+std::string(s.reset?"true":"false")+",\"render_size\":["+std::to_string(s.renderW)+","+std::to_string(s.renderH)+"]";
        body+=",\"static_scene\":"+std::string(s.staticScene?"true":"false")+",\"steady_detail\":"+(s.steadyDetail?"true":"false")+",\"slot\":"+std::to_string(s.slot)+",\"delta_ms\":"+number(s.deltaMs);
        body+=",\"output_size\":["+std::to_string(s.outputW)+","+std::to_string(s.outputH)+"]";
        body+=",\"jitter\":["+number(s.jitter[0])+","+number(s.jitter[1])+"],\"rows_jitter\":["+number(s.rowsJitter[0])+","+number(s.rowsJitter[1])+"],\"previous_jitter\":["+number(s.previousJitter[0])+","+number(s.previousJitter[1])+"]";
        body+=",\"previous_rows_jitter\":["+number(s.previousRowsJitter[0])+","+number(s.previousRowsJitter[1])+"]";
        body+=",\"camera\":"+cameraJson(s.camera)+",\"previous_camera\":"+cameraJson(s.previousCamera)+",\"textures\":[";
        for(unsigned i=0;i<s.items.size();++i) {
            if(i==6)break;
            const auto& item=s.items[i];if(i)body+=",";
            body+="{\"name\":\""+std::string(item.name)+"\",\"present\":"+(item.present?"true":"false")+",\"status\":\""+item.status+"\"";
            body+=",\"resource_format\":"+std::to_string(item.desc.Format)+",\"srv_format\":"+std::to_string(item.view.Format)+",\"srv_dimension\":"+std::to_string(item.view.ViewDimension);
            body+=",\"mip_levels\":"+std::to_string(item.desc.MipLevels)+",\"array_size\":"+std::to_string(item.desc.ArraySize)+",\"sample_count\":"+std::to_string(item.desc.SampleDesc.Count);
            body+=",\"most_detailed_mip\":"+std::to_string(item.view.Texture2D.MostDetailedMip)+",\"srv_mip_levels\":"+std::to_string(item.view.Texture2D.MipLevels);
            body+=",\"width\":"+std::to_string(item.desc.Width)+",\"height\":"+std::to_string(item.desc.Height)+",\"row_stride\":"+std::to_string(item.row)+",\"byte_size\":"+std::to_string(item.bytes)+",\"crc32\":"+std::to_string(item.crc)+",\"files\":[";
            for(size_t j=0;j<item.files.size();++j) {if(j)body+=",";body+="\""+item.files[j]+"\"";}
            body+="]}";
        }
        body+="],\"buffers\":[";
        for(unsigned i=6;i<s.items.size();++i) {
            const auto& item=s.items[i];if(i>6)body+=",";
            body+="{\"name\":\""+std::string(item.name)+"\",\"present\":"+(item.present?"true":"false")+",\"status\":\""+item.status+"\"";
            body+=",\"byte_size\":"+std::to_string(item.bytes)+",\"crc32\":"+std::to_string(item.crc)+",\"structure_stride\":"+std::to_string(item.buffer.StructureByteStride)+",\"misc_flags\":"+std::to_string(item.buffer.MiscFlags)+",\"bind_flags\":"+std::to_string(item.buffer.BindFlags);
            body+=",\"srv_format\":"+std::to_string(item.view.Format)+",\"srv_dimension\":"+std::to_string(item.view.ViewDimension)+",\"first_element\":"+std::to_string(item.view.Buffer.FirstElement)+",\"num_elements\":"+std::to_string(item.view.Buffer.NumElements)+",\"files\":[";
            for(size_t j=0;j<item.files.size();++j) {if(j)body+=",";body+="\""+item.files[j]+"\"";}
            body+="]}";
        }
        body+="]}\n";
        const bool ok=atomic(path(s.manifest),body);
        if(!ok)Log::get().note("flat resolve inputs: frame metadata publication failed frame=%llu directory=%ls",(unsigned long long)s.frame,directory_.c_str());
        return ok;
    }
    void finish(const char* status) {
        for(auto& sample:pending_) {sample.status=status;manifest(sample);}
        pending_.clear();active_=false;
        if(!directory_.empty())session(status);
    }
    bool writeItem(Item& item,const D3D11_MAPPED_SUBRESOURCE& mapped,const Sample& sample) {
        static const auto table=[] {
            std::array<uint32_t,256> out{};
            for(uint32_t i=0;i<256;++i) {uint32_t c=i;for(unsigned j=0;j<8;++j)c=(c>>1)^((c&1)?0xEDB88320u:0u);out[i]=c;}
            return out;
        }();
        uint32_t crc=~0u;
        uint64_t remaining=item.bytes, chunkBytes=0;
        FILE* file=nullptr;unsigned chunk=0;bool ok=true;
        struct CloseFile {FILE*& file;~CloseFile() {if(file)std::fclose(file);}} close{file};
        const UINT rows=item.isBuffer?1u:item.desc.Height;
        for(UINT y=0;y<rows && ok;++y) {
            const auto* row=static_cast<const uint8_t*>(mapped.pData)+size_t(y)*mapped.RowPitch;
            for(uint32_t x=0;x<item.row;++x)crc=table[(crc^row[x])&255]^(crc>>8);
            uint64_t at=0;
            while(at<item.row && ok) {
                if(!file) {
                    const std::string leaf="inputs_"+std::to_string(sample.frame)+"_"+std::to_string(sample.ordinal)+"_"+item.name+"_"+std::to_string(chunk++)+".bin";
                    if(_wfopen_s(&file,path(leaf).c_str(),L"wb") || !file) {ok=false;break;}
                    item.files.push_back(leaf);
                    chunkBytes=0;
                }
                const size_t n=size_t(std::min<uint64_t>(item.row-at,chunkCap-chunkBytes));
                ok=std::fwrite(row+at,1,n,file)==n;at+=n;chunkBytes+=n;remaining-=n;
                if(chunkBytes==chunkCap) {ok=std::fclose(file)==0 && ok;file=nullptr;}
            }
        }
        if(file) {ok=std::fclose(file)==0 && ok;file=nullptr;}
        item.status=ok && !remaining?"complete":"write-failed";
        item.crc=~crc;
        return ok && !remaining;
    }
public:
    // A smaller cap allows a focused fixture to exercise budget refusal
    // without allocating a production-sized image. It can never raise the cap.
    explicit FlatResolveInputCapture(uint64_t cap=byteCap):budget_(std::max<uint64_t>(1,std::min(cap,byteCap))) {}
    bool active() const {return active_;}
    uint64_t copyOperations() const {return copyOperations_;}
    const std::wstring& directory() const {return directory_;}
    void cancel() {if(active_ || !pending_.empty())finish("cancelled");}
    void arm(uint64_t frame) {
        cancel();directory_.clear();manifests_.clear();reserved_=0;attempts_=0;lastFrame_=~0ull;
        armFrame_=frame;armMs_=GetTickCount64();active_=true;
        const auto root=Config::get().logDir()+L"\\flat_pixels";
        directory_=root+L"\\resolve_inputs_"+std::to_wstring(armMs_)+L"_"+std::to_wstring(GetCurrentProcessId())+L"_"+std::to_wstring(++serial_);
        if(!ensureDirectory(Config::get().logDir()) || !ensureDirectory(root) || !ensureDirectory(directory_)) {
            active_=false;Log::get().note("flat resolve inputs: arm failed directory=%ls",directory_.c_str());return;
        }
        if(!session("armed")) {active_=false;return;}
        Log::get().note("flat resolve inputs: armed frame=%llu attempts=2 full-plane byte-cap=%llu stage=prebackend backend-status=not-run directory=%ls",(unsigned long long)frame,(unsigned long long)budget_,directory_.c_str());
    }
    void capture(ID3D11Device* device,ID3D11DeviceContext* context,const FlatMonoResolveFrame& frame) {
        if(!active_ || !frame.hdr || attempts_>=2 || frame.frame==lastFrame_)return;
        lastFrame_=frame.frame;
        Sample s;s.frame=frame.frame;s.queuedMs=GetTickCount64();s.ordinal=++attempts_;
        s.renderW=frame.renderWidth;s.renderH=frame.renderHeight;s.outputW=frame.outputWidth;s.outputH=frame.outputHeight;
        s.hdr=frame.hdr;s.reset=frame.reset;s.mode=frame.mode;
        s.staticScene=frame.staticScene;s.steadyDetail=frame.steadyDetail;s.slot=frame.slot;s.deltaMs=frame.deltaMs;
        s.jitter[0]=frame.jitterX;s.jitter[1]=frame.jitterY;s.rowsJitter[0]=frame.rowsJitterX;s.rowsJitter[1]=frame.rowsJitterY;
        s.previousJitter[0]=frame.previousJitterX;s.previousJitter[1]=frame.previousJitterY;
        s.previousRowsJitter[0]=frame.previousRowsJitterX;s.previousRowsJitter[1]=frame.previousRowsJitterY;
        std::memcpy(s.camera,frame.camera,sizeof(s.camera));std::memcpy(s.previousCamera,frame.previousCamera,sizeof(s.previousCamera));
        s.manifest="inputs_frame_"+std::to_string(s.frame)+"_"+std::to_string(s.ordinal)+".json";
        manifests_.push_back(s.manifest);
        ID3D11ShaderResourceView* views[]={frame.color,frame.depth,frame.cleanColor,frame.overlayCoverage,frame.untrustedCameraCoverage,frame.engine.slots};
        const char* names[]={"color","depth","clean_color","overlay_coverage","untrusted_coverage","slots","pool","scene_now","scene_previous"};
        std::array<Microsoft::WRL::ComPtr<ID3D11Resource>,9> sources{};uint64_t total=0;bool ok=device && context;
        FlatComputeInternalScope internal;
        Microsoft::WRL::ComPtr<ID3D11Device> contextDevice;
        if(context)context->GetDevice(&contextDevice);
        ok=ok && contextDevice.Get()==device;
        for(unsigned i=0;i<6;++i) {
            auto& item=s.items[i];item.name=names[i];item.present=views[i]!=nullptr;
            if(!views[i]) {if(i<2)ok=false;continue;}
            views[i]->GetDesc(&item.view);Microsoft::WRL::ComPtr<ID3D11Resource> resource;views[i]->GetResource(&resource);
            item.status="invalid-view";
            Ptr texture;
            if(!resource || FAILED(resource.As(&texture))) {ok=false;continue;}
            sources[i]=resource;
            texture->GetDesc(&item.desc);Microsoft::WRL::ComPtr<ID3D11Device> owner;texture->GetDevice(&owner);
            uint32_t bpp=i==5?(item.desc.Format==DXGI_FORMAT_R32G32B32A32_FLOAT?16u:8u):i>=3?1u:i==1?(item.desc.Format==DXGI_FORMAT_R32G8X24_TYPELESS?8u:4u):4u;
            const bool format=i==5?(item.desc.Format==DXGI_FORMAT_R32G32_FLOAT || item.desc.Format==DXGI_FORMAT_R32G32B32A32_FLOAT):i>=3?item.desc.Format==DXGI_FORMAT_R8_UNORM:i==1?(item.desc.Format==DXGI_FORMAT_R32G8X24_TYPELESS || item.desc.Format==DXGI_FORMAT_R32_TYPELESS || item.desc.Format==DXGI_FORMAT_R24G8_TYPELESS):item.desc.Format==DXGI_FORMAT_R11G11B10_FLOAT;
            const auto viewFormat=i==1?(item.desc.Format==DXGI_FORMAT_R32G8X24_TYPELESS?DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:item.desc.Format==DXGI_FORMAT_R24G8_TYPELESS?DXGI_FORMAT_R24_UNORM_X8_TYPELESS:DXGI_FORMAT_R32_FLOAT):item.desc.Format;
            if(owner.Get()!=device || !format || item.desc.Width!=s.renderW || item.desc.Height!=s.renderH ||
               item.desc.MipLevels!=1 || item.desc.ArraySize!=1 || item.desc.SampleDesc.Count!=1 ||
               item.view.Format!=viewFormat || item.view.ViewDimension!=D3D11_SRV_DIMENSION_TEXTURE2D || item.view.Texture2D.MostDetailedMip!=0 ||
               (item.view.Texture2D.MipLevels!=1 && item.view.Texture2D.MipLevels!=UINT(-1))) {ok=false;continue;}
            item.row=item.desc.Width*bpp;item.bytes=uint64_t(item.row)*item.desc.Height;total+=item.bytes;item.status="queued";
        }
        for(unsigned i=6;i<s.items.size();++i) {
            auto& item=s.items[i];item.name=names[i];item.isBuffer=true;
            Microsoft::WRL::ComPtr<ID3D11Buffer> buffer;
            if(i==6 && frame.engine.pool) {
                frame.engine.pool->GetDesc(&item.view);frame.engine.pool->GetResource(&sources[i]);
                if(sources[i])sources[i].As(&buffer);
                item.present=true;
            } else if(i==7 || i==8) {
                buffer=i==7?frame.engine.sceneNow:frame.engine.scenePrev;item.present=buffer!=nullptr;sources[i]=buffer;
            }
            if(!item.present)continue;
            item.status="invalid-view";
            if(!buffer) {ok=false;continue;}
            buffer->GetDesc(&item.buffer);const auto& d=item.buffer;
            Microsoft::WRL::ComPtr<ID3D11Device> owner;buffer->GetDevice(&owner);
            const bool viewOk=i==6?(item.view.ViewDimension==D3D11_SRV_DIMENSION_BUFFER && item.view.Format==DXGI_FORMAT_UNKNOWN &&
                (d.MiscFlags&D3D11_RESOURCE_MISC_BUFFER_STRUCTURED) && d.StructureByteStride && d.ByteWidth%d.StructureByteStride==0 &&
                item.view.Buffer.NumElements && uint64_t(item.view.Buffer.FirstElement)+item.view.Buffer.NumElements<=d.ByteWidth/d.StructureByteStride):
                (d.BindFlags&D3D11_BIND_CONSTANT_BUFFER) && d.ByteWidth>=276*16 && d.ByteWidth%16==0;
            if(owner.Get()!=device || !d.ByteWidth || !viewOk) {ok=false;continue;}
            item.row=d.ByteWidth;item.bytes=d.ByteWidth;total+=item.bytes;item.status="queued";
        }
        if(!ok || total>budget_-reserved_) {
            s.status=ok?"budget-cap":"invalid-input";
            for(auto& item:s.items)if(item.status==std::string("queued"))item.status=s.status;
            manifest(s);session(s.status);return;
        }
        for(unsigned i=0;i<s.items.size();++i) {
            auto& item=s.items[i];if(!item.present)continue;
            HRESULT created=E_FAIL;
            if(item.isBuffer) {
                auto desc=item.buffer;desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=desc.MiscFlags=desc.StructureByteStride=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
                Microsoft::WRL::ComPtr<ID3D11Buffer> buffer;created=device->CreateBuffer(&desc,nullptr,&buffer);item.stage=buffer;
            } else {
                auto desc=item.desc;desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=desc.MiscFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
                Ptr texture;created=device->CreateTexture2D(&desc,nullptr,&texture);item.stage=texture;
            }
            if(FAILED(created)) {
                s.status="staging-failed";for(auto& pending:s.items)if(pending.present && pending.status==std::string("queued"))pending.status=s.status;
                manifest(s);session(s.status);return;
            }
        }
        reserved_+=total;
        for(unsigned i=0;i<s.items.size();++i)if(s.items[i].present) {context->CopyResource(s.items[i].stage.Get(),sources[i].Get());++copyOperations_;}
        if(!manifest(s)) {session("metadata-write-failed");return;}
        pending_.push_back(std::move(s));session("queued");
    }
    void poll(ID3D11DeviceContext* context,uint64_t frame) {
        if(!active_)return;
        const auto now=GetTickCount64();
        if(frame<armFrame_ || frame-armFrame_>=900 || now-armMs_>=30000) {finish("expired-arm");return;}
        if(pending_.empty()) {if(attempts_>=2)finish("finished");return;}
        auto& s=pending_.front();
        if(frame<s.frame || frame-s.frame>=120 || now-s.queuedMs>=5000) {
            s.status=frame<s.frame?"frame-gap":"timeout";manifest(s);pending_.pop_front();session("partial");return;
        }
        if(!context)return;
        FlatComputeInternalScope internal;
        std::array<D3D11_MAPPED_SUBRESOURCE,9> maps{};std::array<bool,9> mapped{};HRESULT hr=S_OK;
        struct Unmap {
            ID3D11DeviceContext* context;Sample& sample;std::array<bool,9>& mapped;
            ~Unmap() {for(unsigned i=0;i<mapped.size();++i)if(mapped[i])context->Unmap(sample.items[i].stage.Get(),0);}
        } unmap{context,s,mapped};
        for(unsigned i=0;i<s.items.size();++i)if(s.items[i].present) {
            hr=context->Map(s.items[i].stage.Get(),0,D3D11_MAP_READ,D3D11_MAP_FLAG_DO_NOT_WAIT,&maps[i]);
            if(FAILED(hr))break;mapped[i]=true;
        }
        if(FAILED(hr)) {
            for(unsigned i=0;i<mapped.size();++i)if(mapped[i]) {context->Unmap(s.items[i].stage.Get(),0);mapped[i]=false;}
            if(hr==DXGI_ERROR_WAS_STILL_DRAWING)return;
            s.status="map-failed";manifest(s);pending_.pop_front();session("partial");return;
        }
        bool ok=true;
        for(unsigned i=0;i<s.items.size();++i)if(mapped[i]) {
            if(!s.items[i].isBuffer && maps[i].RowPitch<s.items[i].row) {s.items[i].status="map-failed";ok=false;}
            else ok=writeItem(s.items[i],maps[i],s) && ok;
            context->Unmap(s.items[i].stage.Get(),0);
            mapped[i]=false;
        }
        s.status=ok?"complete":"write-failed";
        const bool published=manifest(s);
        const auto completedFrame=s.frame;const auto completedMode=s.mode;const auto dataStatus=s.status;
        pending_.pop_front();
        const bool sessionPublished=session(published?(ok?"ready":"partial"):"metadata-write-failed");
        Log::get().note("flat resolve inputs: frame=%llu status=%s mode=%s stage=prebackend backend-status=not-run directory=%ls",(unsigned long long)completedFrame,published && sessionPublished?dataStatus:"metadata-write-failed",modeName(completedMode),directory_.c_str());
        if(attempts_>=2 && pending_.empty())finish("finished");
    }
};
} // namespace edvr
