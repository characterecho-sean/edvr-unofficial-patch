#pragma once
#include "flat_pixel_capture_policy.h"
#include "../common/config.h"
#include "../common/log.h"
#include <array>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#ifndef EDVR_VERSION_STRING
#define EDVR_VERSION_STRING "unversioned test build"
#endif

namespace edvr {
// Owner-thread only; no resource, file, or GPU operation until manual arm.
class FlatPixelCapture {
    struct Item {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> stage;
        D3D11_TEXTURE2D_DESC desc{};
        uint32_t stride=0;
        std::string filename;
    };
    FlatPixelCapturePolicy policy_;
    std::array<Item,6> items_{};
    std::wstring directory_;
    FlatMonoResolveFrame frame_{};
    unsigned serial_=0;
    static constexpr const char* names_[6]={"color","depth","motion","rejection","raw","final"};
    static std::wstring wide(const std::string& s) { return std::wstring(s.begin(),s.end()); }
    void release() { items_={};frame_={}; }
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
        std::array<D3D11_MAPPED_SUBRESOURCE,6> maps{};
        unsigned mapped=0;HRESULT hr=S_OK;
        for(;mapped<items_.size();++mapped) {
            hr=context->Map(items_[mapped].stage.Get(),0,D3D11_MAP_READ,D3D11_MAP_FLAG_DO_NOT_WAIT,&maps[mapped]);
            if(FAILED(hr))break;
        }
        if(mapped!=items_.size()) {
            for(unsigned i=0;i<mapped;++i)context->Unmap(items_[i].stage.Get(),0);
            if(hr!=DXGI_ERROR_WAS_STILL_DRAWING)fail("map-failed");
            return;
        }
        // Pack first, then unmap every resource before filesystem work. No GPU
        // flush or blocking Map; memory exists only for the armed diagnostic.
        std::array<std::vector<unsigned char>,6> payload;
        bool packed=true;
        try {
            for(unsigned i=0;i<items_.size();++i) {
                auto& item=items_[i];
                if(maps[i].RowPitch<item.stride) {packed=false;break;}
                payload[i].resize(size_t(item.stride)*item.desc.Height);
                for(uint32_t y=0;y<item.desc.Height;++y)
                    std::memcpy(payload[i].data()+size_t(y)*item.stride,
                        static_cast<const unsigned char*>(maps[i].pData)+size_t(y)*maps[i].RowPitch,item.stride);
            }
        } catch(...) {packed=false;}
        for(unsigned i=0;i<items_.size();++i)context->Unmap(items_[i].stage.Get(),0);
        if(!packed) {fail("pack-failed");return;}
        bool ok=true;
        for(unsigned i=0;i<items_.size() && ok;++i)ok=write(directory_+L"\\"+wide(items_[i].filename),payload[i].data(),payload[i].size());
        char header[1024]{};
        std::snprintf(header,sizeof(header),"{\n\"version\":1,\"frame_id\":%llu,\"mode\":\"%s\",\"configured_dlss_preset\":%u,\"reset\":%s,\"jitter\":[%.9g,%.9g],\"previous_jitter\":[%.9g,%.9g],\"render_width\":%u,\"render_height\":%u,\"output_width\":%u,\"output_height\":%u,\"binary_version\":\"%s\",\"binary_compiled\":\"%s %s\",\"textures\":[\n",
            static_cast<unsigned long long>(frame_.frame),frame_.mode==FlatMonoResolveMode::Dlaa?"dlaa":"dlss",frame_.configuredDlssPreset,frame_.reset?"true":"false",
            frame_.jitterX,frame_.jitterY,frame_.previousJitterX,frame_.previousJitterY,
            frame_.renderWidth,frame_.renderHeight,frame_.outputWidth,frame_.outputHeight,EDVR_VERSION_STRING,__DATE__,__TIME__);
        std::string manifest=header;
        for(unsigned i=0;i<items_.size();++i) {
            const auto& item=items_[i];char row[512]{};
            std::snprintf(row,sizeof(row),"%s{\"name\":\"%s\",\"filename\":\"%s\",\"dxgi_format\":%u,\"width\":%u,\"height\":%u,\"row_stride\":%u,\"byte_size\":%llu}",i?",\n":"",names_[i],item.filename.c_str(),unsigned(item.desc.Format),item.desc.Width,item.desc.Height,item.stride,static_cast<unsigned long long>(payload[i].size()));
            manifest+=row;
        }
        manifest+="\n]}\n";
        const auto manifestPath=directory_+L"\\frame_"+std::to_wstring(frame_.frame)+L".json";
        if(ok)ok=write(manifestPath+L".tmp",manifest.data(),manifest.size());
        if(ok)ok=MoveFileExW((manifestPath+L".tmp").c_str(),manifestPath.c_str(),0)!=FALSE;
        if(!ok) {fail("write-failed");return;}
        Log::get().note("flat pixels: completed frame=%llu textures=6 directory=%ls",static_cast<unsigned long long>(frame_.frame),directory_.c_str());
        policy_.finish(true);release();
        if(policy_.copied>=FlatPixelCapturePolicy::maxSamples)stop("complete");
    }
    void capture(ID3D11Device* device,ID3D11DeviceContext* context,const FlatMonoResolveFrame& frame,bool reset,ID3D11Texture2D* const* textures) {
        if(!policy_.due(frame.frame))return;
        const uint64_t now=GetTickCount64();
        if(policy_.expired(frame.frame,now)) {stop("expired-arm");return;}
        uint64_t bytes=0;
        for(unsigned i=0;i<items_.size();++i) {
            if(!textures[i]) {stop("failed-source");return;}
            textures[i]->GetDesc(&items_[i].desc);
            const auto& d=items_[i].desc;
            const uint32_t bpp=d.Format==DXGI_FORMAT_R8_UNORM?1:4;
            const bool format=d.Format==DXGI_FORMAT_R8_UNORM || d.Format==DXGI_FORMAT_R32_FLOAT ||
                d.Format==DXGI_FORMAT_R16G16_FLOAT || d.Format==DXGI_FORMAT_R8G8B8A8_UNORM || d.Format==DXGI_FORMAT_R8G8B8A8_TYPELESS;
            if(!format || !d.Width || !d.Height || d.Width>16384 || d.Height>16384 || d.MipLevels!=1 || d.ArraySize!=1 || d.SampleDesc.Count!=1) {stop("failed-source-format");return;}
            items_[i].stride=d.Width*bpp;bytes+=uint64_t(items_[i].stride)*d.Height;
        }
        if(!policy_.fits(bytes)) {stop("byte-cap");return;}
        if(!policy_.reserve(frame.frame,now,bytes))return;
        for(unsigned i=0;i<items_.size();++i) {
            auto& item=items_[i];auto d=item.desc;
            d.Usage=D3D11_USAGE_STAGING;d.BindFlags=d.MiscFlags=0;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
            if(FAILED(device->CreateTexture2D(&d,nullptr,item.stage.GetAddressOf()))) {fail("staging-create-failed");return;}
            item.filename="frame_"+std::to_string(frame.frame)+"_"+names_[i]+".bin";
        }
        frame_=frame;frame_.reset=reset;
        // Do not retain borrowed game pointers in diagnostic state.
        frame_.color=frame_.depth=nullptr;frame_.engine={};
        for(unsigned i=0;i<items_.size();++i)context->CopyResource(items_[i].stage.Get(),textures[i]);
        Log::get().note("flat pixels: copied frame=%llu textures=6 bytes=%llu sample=%u",static_cast<unsigned long long>(frame.frame),static_cast<unsigned long long>(bytes),policy_.copied);
    }
};
} // namespace edvr
