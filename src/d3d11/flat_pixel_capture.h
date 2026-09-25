#pragma once
#include "flat_pixel_capture_policy.h"
#include "../common/config.h"
#include "../common/log.h"
#include <array>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#ifndef EDVR_VERSION_STRING
#define EDVR_VERSION_STRING "unversioned test build"
#endif

namespace edvr {
// Owner-thread only; no resource, file, or GPU operation until manual arm.
class FlatPixelCapture {
    struct Item {
        Microsoft::WRL::ComPtr<ID3D11Resource> stage;
        D3D11_TEXTURE2D_DESC desc{};
        D3D11_BUFFER_DESC buffer{};
        D3D11_SHADER_RESOURCE_VIEW_DESC view{};
        const char* name=nullptr;
        bool isBuffer=false,hasView=false;
        uint32_t stride=0;
        std::string filename;
    };
    FlatPixelCapturePolicy policy_;
    std::array<Item,10> items_{};
    unsigned used_=0;
    bool enginePresent_[4]={};
    std::wstring directory_;
    FlatMonoResolveFrame frame_{};
    unsigned serial_=0;
    static constexpr const char* names_[6]={"color","depth","motion","rejection","raw","final"};
    static std::wstring wide(const std::string& s) { return std::wstring(s.begin(),s.end()); }
    void release() { items_={};frame_={};used_=0;for(auto& present:enginePresent_)present=false; }
    static std::string cameraJson(const float (&camera)[6][4]) {
        for(const auto& row:camera)for(float value:row)if(!std::isfinite(value))return "null";
        std::string out="[";
        for(unsigned row=0;row<6;++row) {
            if(row)out+=",";out+="[";
            for(unsigned col=0;col<4;++col) {
                if(col)out+=",";char value[48]{};
                std::snprintf(value,sizeof(value),"%.9g",camera[row][col]);
                out+=value;
            }
            out+="]";
        }
        return out+"]";
    }
    void stop(const char* reason) {
        if(!policy_.active)return;
        Log::get().note("flat pixels: summary status=%s copied=%u completed=%u failed=%u bytes=%llu pending=%u directory=%ls",
            reason,policy_.copied,policy_.completed,policy_.failed,
            static_cast<unsigned long long>(policy_.bytes),policy_.pending?1u:0u,directory_.c_str());
        policy_.active=false;policy_.pending=false;release();
    }
    static bool write(const std::wstring& path,const void* data,size_t bytes) {
        FILE* file=nullptr;
        if(_wfopen_s(&file,path.c_str(),L"wb") || !file)return false;
        const bool ok=std::fwrite(data,1,bytes,file)==bytes;
        return std::fclose(file)==0 && ok;
    }
    void fail(const char* reason) {
        Log::get().note("flat pixels: failed frame=%llu reason=%s",
            static_cast<unsigned long long>(policy_.copyFrame),reason);
        policy_.finish(false);release();
        if(policy_.copied>=FlatPixelCapturePolicy::maxSamples)stop("complete-with-failures");
    }
public:
    bool active() const { return policy_.active; }
    const std::wstring& directory() const { return directory_; }
    void cancel() { stop("cancelled-resize-or-stop"); }
    void arm(uint64_t frame) {
        stop("rearmed");directory_.clear();policy_.arm(frame,GetTickCount64());
        SYSTEMTIME now{};GetSystemTime(&now);wchar_t leaf[128]{};
        _snwprintf_s(leaf,_TRUNCATE,L"%04u%02u%02u_%02u%02u%02u_%03u_%lu_%u",
            now.wYear,now.wMonth,now.wDay,now.wHour,now.wMinute,now.wSecond,now.wMilliseconds,
            GetCurrentProcessId(),++serial_);
        const auto root=Config::get().logDir()+L"\\flat_pixels";
        directory_=root+L"\\"+leaf;
        if(!ensureDirectory(Config::get().logDir()) || !ensureDirectory(root) || !ensureDirectory(directory_)) {
            stop("failed-directory");return;
        }
        Log::get().note("flat pixels: armed frame=%llu samples=4 spacing=15 byte-cap=%llu timeout-frames=900 timeout-ms=30000 directory=%ls; native matched DLSS/DLAA textures, no rendering changes",
            static_cast<unsigned long long>(frame),static_cast<unsigned long long>(FlatPixelCapturePolicy::maxBytes),directory_.c_str());
    }
    void poll(ID3D11DeviceContext* context,uint64_t frame) {
        if(!policy_.active)return;
        const uint64_t now=GetTickCount64();
        if(policy_.pendingExpired(frame,now))fail("expired-readback");
        if(!policy_.active)return;
        if(policy_.expired(frame,now)) {stop("expired-arm");return;}
        if(!policy_.pending || !context)return;
        std::array<D3D11_MAPPED_SUBRESOURCE,10> maps{};
        unsigned mapped=0;HRESULT hr=S_OK;
        for(;mapped<used_;++mapped) {
            hr=context->Map(items_[mapped].stage.Get(),0,D3D11_MAP_READ,D3D11_MAP_FLAG_DO_NOT_WAIT,&maps[mapped]);
            if(FAILED(hr))break;
        }
        if(mapped!=used_) {
            for(unsigned i=0;i<mapped;++i)context->Unmap(items_[i].stage.Get(),0);
            if(hr!=DXGI_ERROR_WAS_STILL_DRAWING)fail("map-failed");
            return;
        }
        // Pack first, then unmap every resource before filesystem work. No GPU
        // flush or blocking Map; memory exists only for the armed diagnostic.
        std::array<std::vector<unsigned char>,10> payload;
        bool packed=true;
        try {
            for(unsigned i=0;i<used_;++i) {
                auto& item=items_[i];
                if(item.isBuffer) {
                    payload[i].resize(item.buffer.ByteWidth);
                    std::memcpy(payload[i].data(),maps[i].pData,payload[i].size());continue;
                }
                if(maps[i].RowPitch<item.stride) {packed=false;break;}
                payload[i].resize(size_t(item.stride)*item.desc.Height);
                for(uint32_t y=0;y<item.desc.Height;++y)
                    std::memcpy(payload[i].data()+size_t(y)*item.stride,
                        static_cast<const unsigned char*>(maps[i].pData)+size_t(y)*maps[i].RowPitch,item.stride);
            }
        } catch(...) {packed=false;}
        for(unsigned i=0;i<used_;++i)context->Unmap(items_[i].stage.Get(),0);
        if(!packed) {fail("pack-failed");return;}
        bool ok=true;
        for(unsigned i=0;i<used_ && ok;++i)ok=write(directory_+L"\\"+wide(items_[i].filename),payload[i].data(),payload[i].size());
        char header[1024]{};
        std::snprintf(header,sizeof(header),"{\n\"version\":2,\"frame_id\":%llu,\"mode\":\"%s\",\"configured_dlss_preset\":%u,\"reset\":%s,\"jitter\":[%.9g,%.9g],\"previous_jitter\":[%.9g,%.9g],\"render_width\":%u,\"render_height\":%u,\"output_width\":%u,\"output_height\":%u,\"binary_version\":\"%s\",\"binary_compiled\":\"%s %s\",\"textures\":[\n",
            static_cast<unsigned long long>(frame_.frame),frame_.mode==FlatMonoResolveMode::Dlaa?"dlaa":"dlss",frame_.configuredDlssPreset,frame_.reset?"true":"false",
            frame_.jitterX,frame_.jitterY,frame_.previousJitterX,frame_.previousJitterY,
            frame_.renderWidth,frame_.renderHeight,frame_.outputWidth,frame_.outputHeight,EDVR_VERSION_STRING,__DATE__,__TIME__);
        std::string manifest=header;
        for(unsigned i=0;i<used_;++i) {
            const auto& item=items_[i];char row[512]{};
            if(item.isBuffer)continue;
            std::snprintf(row,sizeof(row),"%s{\"name\":\"%s\",\"filename\":\"%s\",\"dxgi_format\":%u,\"width\":%u,\"height\":%u,\"row_stride\":%u,\"byte_size\":%llu",i?",\n":"",item.name,item.filename.c_str(),unsigned(item.desc.Format),item.desc.Width,item.desc.Height,item.stride,static_cast<unsigned long long>(payload[i].size()));
            manifest+=row;
            if(item.hasView) {
                std::snprintf(row,sizeof(row),",\"srv_format\":%u,\"srv_dimension\":%u,\"most_detailed_mip\":%u,\"mip_levels\":%u",unsigned(item.view.Format),unsigned(item.view.ViewDimension),item.view.Texture2D.MostDetailedMip,item.view.Texture2D.MipLevels);
                manifest+=row;
            }
            manifest+="}";
        }
        manifest+="\n],\"buffers\":[";bool comma=false;
        for(unsigned i=0;i<used_;++i) {
            const auto& item=items_[i];if(!item.isBuffer)continue;char row[512]{};
            std::snprintf(row,sizeof(row),"%s{\"name\":\"%s\",\"filename\":\"%s\",\"byte_size\":%u,\"stride\":%u,\"misc_flags\":%u",comma?",":"",item.name,item.filename.c_str(),item.buffer.ByteWidth,item.buffer.StructureByteStride,item.buffer.MiscFlags);
            manifest+=row;comma=true;
            if(item.hasView) {
                std::snprintf(row,sizeof(row),",\"srv_format\":%u,\"srv_dimension\":%u,\"first_element\":%u,\"num_elements\":%u",unsigned(item.view.Format),unsigned(item.view.ViewDimension),item.view.Buffer.FirstElement,item.view.Buffer.NumElements);
                manifest+=row;
            }
            manifest+="}";
        }
        const bool complete=enginePresent_[0]&&enginePresent_[1]&&enginePresent_[2]&&enginePresent_[3];
        std::snprintf(header,sizeof(header),"],\"engine\":{\"complete\":%s,\"slots_present\":%s,\"pool_present\":%s,\"scene_now_present\":%s,\"scene_previous_present\":%s,\"status\":\"%s\"},\"camera\":",complete?"true":"false",enginePresent_[0]?"true":"false",enginePresent_[1]?"true":"false",enginePresent_[2]?"true":"false",enginePresent_[3]?"true":"false",complete?"complete":"absent-or-partial");
        manifest+=header;manifest+=cameraJson(frame_.camera);manifest+=",\"previous_camera\":";
        manifest+=cameraJson(frame_.previousCamera);manifest+="\n}\n";
        const auto manifestPath=directory_+L"\\frame_"+std::to_wstring(frame_.frame)+L".json";
        if(ok)ok=write(manifestPath+L".tmp",manifest.data(),manifest.size());
        if(ok)ok=MoveFileExW((manifestPath+L".tmp").c_str(),manifestPath.c_str(),0)!=FALSE;
        if(!ok) {fail("write-failed");return;}
        Log::get().note("flat pixels: completed frame=%llu resources=%u engine-complete=%u directory=%ls",static_cast<unsigned long long>(frame_.frame),used_,complete?1u:0u,directory_.c_str());
        policy_.finish(true);release();
        if(policy_.copied>=FlatPixelCapturePolicy::maxSamples)stop("complete");
    }
    void capture(ID3D11Device* device,ID3D11DeviceContext* context,const FlatMonoResolveFrame& frame,bool reset,ID3D11Texture2D* const* textures) {
        if(!policy_.due(frame.frame))return;
        const uint64_t now=GetTickCount64();
        if(policy_.expired(frame.frame,now)) {stop("expired-arm");return;}
        uint64_t bytes=0;
        std::array<Microsoft::WRL::ComPtr<ID3D11Resource>,10> sources;
        auto addTexture=[&](const char* name,ID3D11Texture2D* texture,const D3D11_SHADER_RESOURCE_VIEW_DESC* view=nullptr) {
            if(!texture || used_>=items_.size())return false;
            auto& item=items_[used_];texture->GetDesc(&item.desc);const auto& d=item.desc;
            const uint32_t bpp=d.Format==DXGI_FORMAT_R8_UNORM?1:(d.Format==DXGI_FORMAT_R32G32_FLOAT?8:4);
            const bool format=d.Format==DXGI_FORMAT_R8_UNORM || d.Format==DXGI_FORMAT_R32_FLOAT ||
                d.Format==DXGI_FORMAT_R16G16_FLOAT || d.Format==DXGI_FORMAT_R8G8B8A8_UNORM ||
                d.Format==DXGI_FORMAT_R8G8B8A8_TYPELESS || d.Format==DXGI_FORMAT_R32G32_FLOAT;
            if(!format || !d.Width || !d.Height || d.Width>16384 || d.Height>16384 || d.MipLevels!=1 || d.ArraySize!=1 || d.SampleDesc.Count!=1)return false;
            item.stride=d.Width*bpp;item.name=name;bytes+=uint64_t(item.stride)*d.Height;
            if(view) {item.hasView=true;item.view=*view;}
            sources[used_++]=texture;return true;
        };
        auto addBuffer=[&](const char* name,ID3D11Buffer* buffer,const D3D11_SHADER_RESOURCE_VIEW_DESC* view=nullptr) {
            if(!buffer || used_>=items_.size())return false;
            auto& item=items_[used_];buffer->GetDesc(&item.buffer);const auto& d=item.buffer;
            if(!d.ByteWidth)return false;
            if(view) {
                if(view->ViewDimension!=D3D11_SRV_DIMENSION_BUFFER || view->Format!=DXGI_FORMAT_UNKNOWN ||
                    !(d.MiscFlags&D3D11_RESOURCE_MISC_BUFFER_STRUCTURED) || !d.StructureByteStride ||
                    d.ByteWidth%d.StructureByteStride || !view->Buffer.NumElements ||
                    uint64_t(view->Buffer.FirstElement)+view->Buffer.NumElements>d.ByteWidth/d.StructureByteStride)return false;
                item.hasView=true;item.view=*view;
            } else if(!(d.BindFlags&D3D11_BIND_CONSTANT_BUFFER) || d.ByteWidth<276*16 || d.ByteWidth%16)return false;
            item.isBuffer=true;item.name=name;bytes+=d.ByteWidth;sources[used_++]=buffer;return true;
        };
        for(unsigned i=0;i<6;++i)if(!addTexture(names_[i],textures[i])) {stop("failed-source-format");return;}
        enginePresent_[0]=frame.engine.slots!=nullptr;enginePresent_[1]=frame.engine.pool!=nullptr;
        enginePresent_[2]=frame.engine.sceneNow!=nullptr;enginePresent_[3]=frame.engine.scenePrev!=nullptr;
        if(frame.engine.slots) {
            D3D11_SHADER_RESOURCE_VIEW_DESC view{};frame.engine.slots->GetDesc(&view);
            Microsoft::WRL::ComPtr<ID3D11Resource> resource;frame.engine.slots->GetResource(&resource);
            Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
            if(view.ViewDimension!=D3D11_SRV_DIMENSION_TEXTURE2D || view.Format!=DXGI_FORMAT_R32G32_FLOAT ||
                view.Texture2D.MostDetailedMip || (view.Texture2D.MipLevels!=1 && view.Texture2D.MipLevels!=UINT(-1)) ||
                !resource || FAILED(resource.As(&texture))) {stop("failed-slots-view");return;}
            D3D11_TEXTURE2D_DESC d{};texture->GetDesc(&d);
            if(d.Width!=frame.renderWidth || d.Height!=frame.renderHeight || d.Format!=DXGI_FORMAT_R32G32_FLOAT ||
                !addTexture("slots",texture.Get(),&view)) {stop("failed-slots-resource");return;}
        }
        if(frame.engine.pool) {
            D3D11_SHADER_RESOURCE_VIEW_DESC view{};frame.engine.pool->GetDesc(&view);
            Microsoft::WRL::ComPtr<ID3D11Resource> resource;frame.engine.pool->GetResource(&resource);
            Microsoft::WRL::ComPtr<ID3D11Buffer> buffer;
            if(!resource || FAILED(resource.As(&buffer)) || !addBuffer("pool",buffer.Get(),&view)) {stop("failed-pool-view");return;}
        }
        if(frame.engine.sceneNow && !addBuffer("scene_now",frame.engine.sceneNow)) {stop("failed-scene-now");return;}
        if(frame.engine.scenePrev && !addBuffer("scene_previous",frame.engine.scenePrev)) {stop("failed-scene-previous");return;}
        if(!policy_.fits(bytes)) {stop("byte-cap");return;}
        if(!policy_.reserve(frame.frame,now,bytes))return;
        for(unsigned i=0;i<used_;++i) {
            auto& item=items_[i];HRESULT hr;
            if(item.isBuffer) {
                auto d=item.buffer;d.Usage=D3D11_USAGE_STAGING;d.BindFlags=d.MiscFlags=d.StructureByteStride=0;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
                Microsoft::WRL::ComPtr<ID3D11Buffer> staging;hr=device->CreateBuffer(&d,nullptr,&staging);item.stage=staging;
            } else {
                auto d=item.desc;d.Usage=D3D11_USAGE_STAGING;d.BindFlags=d.MiscFlags=0;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
                Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;hr=device->CreateTexture2D(&d,nullptr,&staging);item.stage=staging;
            }
            if(FAILED(hr)) {fail("staging-create-failed");return;}
            item.filename="frame_"+std::to_string(frame.frame)+"_"+item.name+".bin";
        }
        frame_=frame;frame_.reset=reset;
        // Do not retain borrowed game pointers in diagnostic state.
        frame_.color=frame_.depth=nullptr;frame_.engine={};
        for(unsigned i=0;i<used_;++i)context->CopyResource(items_[i].stage.Get(),sources[i].Get());
        Log::get().note("flat pixels: copied frame=%llu resources=%u bytes=%llu sample=%u",static_cast<unsigned long long>(frame.frame),used_,static_cast<unsigned long long>(bytes),policy_.copied);
    }
};
} // namespace edvr
