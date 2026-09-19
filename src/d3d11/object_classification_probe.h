#pragma once
// Explicit, bounded diagnostic for the object-classification investigation.
// While armed it follows only resources nominated by eye-zero mesh draws.  It
// records hook-observed writes and takes GPU copies; CPU readback and file I/O
// happen later in write().  It never changes a live binding or draw.
#include "../common/game_call_probe.h"
#include "object_source_owner_probe.h"
#include <d3d11.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace edvr {

class ObjectClassificationProbe {
    template<class T> using Ptr=Microsoft::WRL::ComPtr<T>;
public:
    enum WriteKind:uint32_t {MapWrite=1,Update=3,CopyResource=4,CopyRegion=5,Unknown=6};
    struct Summary {
        bool active=false,sealed=false;
        uint32_t currentFrame=0,discoveryFrame=0,selectedFrame=0,sceneFrame=0;
        uint32_t resources=0,writes=0,draws=0,snapshots=0,blobs=0;
        uint64_t observedWrites=0;
        uint32_t stackSamples=0,resourceOverflow=0,writeOverflow=0,drawOverflow=0;
        uint32_t stackOverflow=0,snapshotOverflow=0,bufferBudgetDeclines=0;
        uint32_t textureBudgetDeclines=0,unsafeAssociations=0,copyFailures=0;
        uint32_t mapFailures=0,unmatchedWrites=0,unobservedSources=0;
        uint32_t ignoredEye=0,ignoredFrame=0,stageRejects=0,suppressed=0;
        uint32_t duplicateMaps=0,unmatchedUnmaps=0,pendingMapOverflow=0,openMapSnapshots=0;
        uint64_t foreignWrites=0;
        uint32_t sourceOwnerMaps=0,sourceOwnerAttempts=0,sourceOwnerComplete=0,sourceOwnerPartial=0;
        uint32_t sourceOwnerIdentityRejects=0,sourceOwnerOpcodeRejects=0,sourceOwnerUnwindFailures=0;
        uint32_t sourceOwnerResourceMatches=0,sourceOwnerReadFaults=0,sourceOwnerMetadataChanges=0;
        uint64_t sourceOwnerDescriptorOverflow=0,sourceOwnerCpuByteDeclines=0;
    };

private:
    static constexpr uint32_t kResourceCap=64,kWriteCap=512,kDrawCap=512,kStackCap=128;
    static constexpr uint64_t kBufferBudget=128ull*1024*1024;
    static constexpr uint64_t kTextureBudget=128ull*1024*1024;
    static constexpr uint32_t kBufferCap=16u*1024*1024;
    static constexpr uint32_t kNone=std::numeric_limits<uint32_t>::max();
    struct Resource {
        Ptr<ID3D11Resource> object;
        uint64_t identity=0;
        D3D11_RESOURCE_DIMENSION dimension=D3D11_RESOURCE_DIMENSION_UNKNOWN;
        D3D11_BUFFER_DESC buffer{};
        D3D11_TEXTURE2D_DESC texture{};
        uint64_t generation=0;
        bool pool=false,ids=false,auxiliary=false,watch=false,safe=true;
        bool unobservedPriorWrite=true,unmatched=true;
    };
    struct Stack {
        uint32_t captured=0,gameFrames=0;
        std::string rvas;
    };
    struct Event {
        uint32_t resource=kNone,kind=0,frame=0,subresource=0,stack=kNone,mapType=0;
        uint32_t completionStack=kNone,completionFrame=0;
        uint32_t sourceResource=kNone;
        uint64_t generation=0,context=0,sourceIdentity=0,sourceGeneration=0;
        uint64_t foreignEpoch=0;
        uint64_t first=0,end=~0ull;
        uint32_t sourceOwnerAttempt=kNone;
        bool mapped=false,complete=true,sourceMatched=false;
        std::string completionStatus="not_applicable";
    };
    struct PendingMap {
        ID3D11DeviceContext* context=nullptr;
        ID3D11Resource* resource=nullptr;
        uint32_t subresource=0,event=kNone;
        uint64_t generation=0;
        D3D11_MAP type=D3D11_MAP_READ;
        bool complete=false;
    };
    struct Blob {
        std::string name,status;
        Ptr<ID3D11Resource> stage;
        uint32_t sourceResource=kNone,meshFrame=0;
        uint64_t generation=0,foreignEpoch=0;
        uint32_t format=0,rowBytes=0,width=0,height=0,subresource=0;
        uint64_t reservedBytes=0,fileOffset=0,fileBytes=0;
        bool texture=false;
    };
    struct Snapshot {
        uint32_t resource=kNone,meshFrame=0;
        uint64_t generation=0,foreignEpoch=0;
        uint32_t blob=kNone;
    };
    struct Draw {
        uint32_t meshFrame=0,eye=0,firstRecord=0,instances=0,idByteOffset=0;
        uint32_t poolResource=kNone,idResource=kNone,poolSnapshot=kNone,idSnapshot=kNone;
        uint64_t poolGeneration=0,idGeneration=0;
        uint64_t vertexShaderHash=0;
        std::array<uint32_t,16> key{};
        bool keyPresent=false,poolWriteObserved=false,idWriteObserved=false,poolMatched=false,idMatched=false;
    };

    mutable std::recursive_mutex mutex_;
    std::atomic<bool> gateActive_{false};
    std::atomic<uint64_t> foreignWrites_{0},foreignEpoch_{0};
    bool active_=false,sealed_=false,busy_=false,missedWindow_=false,testIdentityOverride_=false;
    bool haveDiscovery_=false,haveSelected_=false;
    uint32_t currentFrame_=0,discoveryFrame_=0,selectedFrame_=0,sceneFrame_=0;
    uint64_t bufferBytes_=0,textureBytes_=0,observedWrites_=0;
    std::vector<Resource> resources_;
    std::vector<Stack> stacks_;
    std::vector<Event> events_;
    std::vector<PendingMap> pendingMaps_;
    std::vector<Blob> blobs_;
    std::vector<Snapshot> snapshots_;
    std::vector<Draw> draws_;
    ObjectSourceOwnerProbe sourceOwner_;
    Summary counters_{};

    struct Busy {
        ObjectClassificationProbe& owner;
        explicit Busy(ObjectClassificationProbe& p):owner(p){owner.busy_=true;}
        ~Busy(){owner.busy_=false;}
    };
    static uint64_t identity(const void* p) noexcept {return uint64_t(reinterpret_cast<uintptr_t>(p));}
    void clear(bool keepClock) {
        const uint32_t clock=keepClock?currentFrame_:0;
        active_=sealed_=busy_=missedWindow_=testIdentityOverride_=false;haveDiscovery_=haveSelected_=false;
        gateActive_.store(false,std::memory_order_release);
        foreignWrites_.store(0,std::memory_order_relaxed);foreignEpoch_.store(0,std::memory_order_relaxed);
        currentFrame_=clock;discoveryFrame_=selectedFrame_=sceneFrame_=0;
        bufferBytes_=textureBytes_=observedWrites_=0;
        resources_.clear();stacks_.clear();events_.clear();pendingMaps_.clear();
        blobs_.clear();snapshots_.clear();draws_.clear();counters_=Summary{};
        sourceOwner_.reset();
        resources_.reserve(kResourceCap);stacks_.reserve(kStackCap);events_.reserve(kWriteCap);
        pendingMaps_.reserve(kWriteCap);draws_.reserve(kDrawCap);
    }
    uint32_t findResource(ID3D11Resource* r) const noexcept {
        if(!r)return kNone;
        for(uint32_t i=0;i<resources_.size();++i)if(resources_[i].object.Get()==r)return i;
        return kNone;
    }
    bool nominated(uint32_t r) const noexcept {return r!=kNone&&(resources_[r].pool||resources_[r].ids||resources_[r].watch);}
    uint32_t addResource(ID3D11Resource* r,bool pool,bool ids,bool auxiliary=false,bool watch=false) {
        if(!r)return kNone;
        uint32_t found=findResource(r);
        if(found!=kNone) {
            resources_[found].pool=resources_[found].pool||pool;
            resources_[found].ids=resources_[found].ids||ids;
            resources_[found].auxiliary=resources_[found].auxiliary||auxiliary;
            resources_[found].watch=resources_[found].watch||(watch&&resources_[found].dimension==D3D11_RESOURCE_DIMENSION_BUFFER);
            return found;
        }
        if(resources_.size()>=kResourceCap){++counters_.resourceOverflow;return kNone;}
        Resource out;out.object=r;out.identity=identity(r);out.pool=pool;out.ids=ids;out.auxiliary=auxiliary;
        r->GetType(&out.dimension);
        Ptr<ID3D11Buffer> b;Ptr<ID3D11Texture2D> t;
        if(SUCCEEDED(r->QueryInterface(IID_PPV_ARGS(&b)))) {
            b->GetDesc(&out.buffer);
            out.watch=watch;
            out.safe=(out.buffer.BindFlags&(D3D11_BIND_UNORDERED_ACCESS|D3D11_BIND_STREAM_OUTPUT))==0;
        } else if(SUCCEEDED(r->QueryInterface(IID_PPV_ARGS(&t))))t->GetDesc(&out.texture);
        resources_.push_back(std::move(out));return uint32_t(resources_.size()-1);
    }
    uint32_t sampleStack() {
        if(stacks_.size()>=kStackCap){++counters_.stackOverflow;return kNone;}
        const GameCallStack s=captureGameCallStack();
        stacks_.push_back({s.captured,s.gameFrames,s.rvas});return uint32_t(stacks_.size()-1);
    }
    uint32_t appendEvent(uint32_t resource,uint32_t kind,ID3D11DeviceContext* context,
                         ID3D11Resource* source,uint32_t sub,uint64_t first,uint64_t end,
                         bool mapped,bool complete) {
        Resource& target=resources_[resource];
        ++target.generation;++observedWrites_;
        target.unobservedPriorWrite=false;
        if(kind==Unknown||kind==MapWrite){target.unmatched=true;if(kind==Unknown)++counters_.unmatchedWrites;}
        else target.unmatched=false;
        if(events_.size()>=kWriteCap){if(!target.unmatched)++counters_.unmatchedWrites;target.unmatched=true;++counters_.writeOverflow;return kNone;}
        Event e;e.resource=resource;e.kind=kind;e.frame=currentFrame_;e.generation=target.generation;
        e.foreignEpoch=foreignEpoch_.load(std::memory_order_acquire);
        e.context=identity(context);e.subresource=sub;e.first=first;e.end=end;e.mapped=mapped;e.complete=complete;
        if(mapped)e.completionStatus="open";
        e.stack=sampleStack();
        if(source) {
            e.sourceIdentity=identity(source);
            e.sourceResource=addResource(source,false,false,true,true);
            if(e.sourceResource!=kNone) {
                const Resource& src=resources_[e.sourceResource];e.sourceGeneration=src.generation;
                e.sourceMatched=!src.unmatched && !src.unobservedPriorWrite;
            }
            if((kind==CopyResource||kind==CopyRegion)&&!e.sourceMatched)resources_[resource].unmatched=true;
        }
        events_.push_back(e);return uint32_t(events_.size()-1);
    }
    bool openWriteMap(uint32_t resource) const noexcept {
        if(resource==kNone)return false;ID3D11Resource* object=resources_[resource].object.Get();
        for(const auto& p:pendingMaps_)if(p.resource==object&&p.type!=D3D11_MAP_READ)return true;
        return false;
    }
    static uint32_t bytesPerPixel(DXGI_FORMAT f) noexcept {
        switch(f) {
        case DXGI_FORMAT_R32G32B32A32_TYPELESS:case DXGI_FORMAT_R32G32B32A32_FLOAT:
        case DXGI_FORMAT_R32G32B32A32_UINT:case DXGI_FORMAT_R32G32B32A32_SINT:return 16;
        case DXGI_FORMAT_R16G16B16A16_TYPELESS:case DXGI_FORMAT_R16G16B16A16_FLOAT:
        case DXGI_FORMAT_R16G16B16A16_UNORM:case DXGI_FORMAT_R16G16B16A16_UINT:
        case DXGI_FORMAT_R16G16B16A16_SNORM:case DXGI_FORMAT_R16G16B16A16_SINT:
        case DXGI_FORMAT_R32G32_TYPELESS:case DXGI_FORMAT_R32G32_FLOAT:
        case DXGI_FORMAT_R32G32_UINT:case DXGI_FORMAT_R32G32_SINT:
        case DXGI_FORMAT_R32G8X24_TYPELESS:case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
        case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:case DXGI_FORMAT_X32_TYPELESS_G8X24_UINT:return 8;
        case DXGI_FORMAT_R10G10B10A2_TYPELESS:case DXGI_FORMAT_R10G10B10A2_UNORM:
        case DXGI_FORMAT_R10G10B10A2_UINT:case DXGI_FORMAT_R11G11B10_FLOAT:
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:case DXGI_FORMAT_R8G8B8A8_UINT:
        case DXGI_FORMAT_R8G8B8A8_SNORM:case DXGI_FORMAT_R8G8B8A8_SINT:
        case DXGI_FORMAT_R16G16_TYPELESS:case DXGI_FORMAT_R16G16_FLOAT:
        case DXGI_FORMAT_R16G16_UNORM:case DXGI_FORMAT_R16G16_UINT:
        case DXGI_FORMAT_R16G16_SNORM:case DXGI_FORMAT_R16G16_SINT:
        case DXGI_FORMAT_R32_TYPELESS:case DXGI_FORMAT_D32_FLOAT:case DXGI_FORMAT_R32_FLOAT:
        case DXGI_FORMAT_R32_UINT:case DXGI_FORMAT_R32_SINT:
        case DXGI_FORMAT_R24G8_TYPELESS:case DXGI_FORMAT_D24_UNORM_S8_UINT:
        case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:case DXGI_FORMAT_X24_TYPELESS_G8_UINT:return 4;
        case DXGI_FORMAT_R8G8_TYPELESS:case DXGI_FORMAT_R8G8_UNORM:case DXGI_FORMAT_R8G8_UINT:
        case DXGI_FORMAT_R8G8_SNORM:case DXGI_FORMAT_R8G8_SINT:
        case DXGI_FORMAT_R16_TYPELESS:case DXGI_FORMAT_R16_FLOAT:case DXGI_FORMAT_D16_UNORM:
        case DXGI_FORMAT_R16_UNORM:case DXGI_FORMAT_R16_UINT:case DXGI_FORMAT_R16_SNORM:
        case DXGI_FORMAT_R16_SINT:return 2;
        case DXGI_FORMAT_R8_TYPELESS:case DXGI_FORMAT_R8_UNORM:case DXGI_FORMAT_R8_UINT:
        case DXGI_FORMAT_R8_SNORM:case DXGI_FORMAT_R8_SINT:case DXGI_FORMAT_A8_UNORM:return 1;
        default:return 0;
        }
    }
    uint32_t captureBuffer(ID3D11DeviceContext* ctx,uint32_t resource,uint32_t frame,const char* name,bool requireSafe=true) {
        if(resource==kNone)return kNone;
        Resource& r=resources_[resource];
        const uint64_t foreignEpoch=foreignEpoch_.load(std::memory_order_acquire);
        for(uint32_t i=0;i<snapshots_.size();++i)if(snapshots_[i].resource==resource &&
            snapshots_[i].generation==r.generation && snapshots_[i].foreignEpoch==foreignEpoch &&
            snapshots_[i].meshFrame==frame && snapshots_[i].blob<blobs_.size() && blobs_[snapshots_[i].blob].stage)return i;
        Blob b;b.name=name;b.sourceResource=resource;b.meshFrame=frame;b.generation=r.generation;b.foreignEpoch=foreignEpoch;
        if(openWriteMap(resource)){b.status="mapped_write_open";++counters_.openMapSnapshots;}
        else if(requireSafe&&!r.safe){b.status="unsafe_uav_or_stream_output";++counters_.unsafeAssociations;}
        else if(r.dimension!=D3D11_RESOURCE_DIMENSION_BUFFER){b.status="not_buffer";}
        else if(!r.buffer.ByteWidth || r.buffer.ByteWidth>kBufferCap){b.status="buffer_size_cap";++counters_.snapshotOverflow;}
        else if(r.buffer.ByteWidth>kBufferBudget-bufferBytes_){b.status="buffer_budget";++counters_.bufferBudgetDeclines;}
        else {
            Ptr<ID3D11Device> dev;ctx->GetDevice(&dev);
            D3D11_BUFFER_DESC d{};d.ByteWidth=r.buffer.ByteWidth;d.Usage=D3D11_USAGE_STAGING;
            d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;Ptr<ID3D11Buffer> stage;
            const HRESULT hr=dev?dev->CreateBuffer(&d,nullptr,&stage):E_POINTER;
            if(FAILED(hr)){b.status="staging_create_failed";++counters_.copyFailures;}
            else {b.stage=stage;b.reservedBytes=d.ByteWidth;bufferBytes_+=d.ByteWidth;Busy own(*this);ctx->CopyResource(stage.Get(),r.object.Get());b.status="pending";}
        }
        blobs_.push_back(std::move(b));snapshots_.push_back({resource,frame,r.generation,foreignEpoch,uint32_t(blobs_.size()-1)});
        return uint32_t(snapshots_.size()-1);
    }
    uint32_t captureStageBuffer(ID3D11DeviceContext* ctx,ID3D11Buffer* source,uint32_t frame,
                                const char* name,uint32_t recordCount) {
        const uint32_t r=addResource(source,false,false,true);
        const uint32_t s=captureBuffer(ctx,r,frame,name,false);
        if(s!=kNone)blobs_[snapshots_[s].blob].subresource=recordCount;
        return s;
    }
    uint32_t captureTexture(ID3D11DeviceContext* ctx,ID3D11Resource* source,uint32_t frame,const char* name) {
        const uint32_t resource=addResource(source,false,false,true);
        Blob b;b.name=name;b.sourceResource=resource;b.meshFrame=frame;b.texture=true;
        if(resource==kNone)b.status=source?"resource_cap":"absent";
        else {
            Resource& r=resources_[resource];b.generation=r.generation;b.foreignEpoch=foreignEpoch_.load(std::memory_order_acquire);b.format=uint32_t(r.texture.Format);
            b.width=r.texture.Width;b.height=r.texture.Height;
            const uint32_t unit=bytesPerPixel(r.texture.Format);
            const uint64_t row=uint64_t(unit)*r.texture.Width,total=row*r.texture.Height;
            if(r.dimension!=D3D11_RESOURCE_DIMENSION_TEXTURE2D)b.status="not_texture2d";
            else if(r.texture.ArraySize!=1 || r.texture.SampleDesc.Count!=1 || !r.texture.Width || !r.texture.Height)b.status="texture_shape";
            else if(!unit || row>UINT32_MAX)b.status="unsupported_format";
            else if(total>kTextureBudget-textureBytes_) {b.status="texture_budget";++counters_.textureBudgetDeclines;}
            else {
                Ptr<ID3D11Device> dev;ctx->GetDevice(&dev);D3D11_TEXTURE2D_DESC d=r.texture;
                d.MipLevels=1;d.ArraySize=1;d.Usage=D3D11_USAGE_STAGING;d.BindFlags=0;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;d.MiscFlags=0;
                Ptr<ID3D11Texture2D> stage;const HRESULT hr=dev?dev->CreateTexture2D(&d,nullptr,&stage):E_POINTER;
                if(FAILED(hr)){b.status="staging_create_failed";++counters_.copyFailures;}
                else {b.stage=stage;b.rowBytes=uint32_t(row);b.reservedBytes=total;textureBytes_+=total;Busy own(*this);ctx->CopySubresourceRegion(stage.Get(),0,0,0,0,source,0,nullptr);b.status="pending";}
            }
        }
        blobs_.push_back(std::move(b));return uint32_t(blobs_.size()-1);
    }
    static std::string quote(const std::string& v) {
        std::string out="\"";char tmp[7]{};
        for(unsigned char c:v){if(c=='\"'||c=='\\')out+='\\';if(c<32){sprintf_s(tmp,"\\u%04x",unsigned(c));out+=tmp;}else out+=char(c);}
        out+='\"';return out;
    }
    static std::string narrow(const wchar_t* value) {
        if(!value)return {};
        const int n=WideCharToMultiByte(CP_UTF8,0,value,-1,nullptr,0,nullptr,nullptr);
        if(n<=1)return {};
        std::string out(size_t(n),'\0');WideCharToMultiByte(CP_UTF8,0,value,-1,&out[0],n,nullptr,nullptr);out.pop_back();return out;
    }
    static std::wstring path(const wchar_t* dir,const wchar_t* stem,const wchar_t* stamp,const wchar_t* ext) {
        std::wstring out=dir?dir:L"";if(!out.empty()&&out.back()!=L'\\'&&out.back()!=L'/')out+=L'\\';
        out+=stem;out+=stamp?stamp:L"";out+=ext;return out;
    }
    static const char* kindName(uint32_t k) noexcept {
        switch(k){case MapWrite:return "map";case Update:return "update";case CopyResource:return "copy_resource";
        case CopyRegion:return "copy_region";default:return "unknown";}
    }
    static void executableIdentity(uint32_t& timestamp,uint32_t& imageSize) noexcept {
        timestamp=imageSize=0;const uintptr_t base=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));if(!base)return;
        const auto* dos=reinterpret_cast<const IMAGE_DOS_HEADER*>(base);if(dos->e_magic!=IMAGE_DOS_SIGNATURE)return;
        const auto* nt=reinterpret_cast<const IMAGE_NT_HEADERS*>(base+dos->e_lfanew);if(nt->Signature!=IMAGE_NT_SIGNATURE)return;
        timestamp=nt->FileHeader.TimeDateStamp;imageSize=nt->OptionalHeader.SizeOfImage;
    }
    void writeResource(std::ostringstream& j,const Resource& r,uint32_t id) const {
        j<<"{\"id\":"<<id<<",\"identity\":\"0x"<<std::hex<<r.identity<<std::dec<<"\",\"dimension\":"<<uint32_t(r.dimension)
         <<",\"roles\":[";bool comma=false;if(r.pool){j<<"\"t33_pool\"";comma=true;}if(r.ids){if(comma)j<<',';j<<"\"ia_ids\"";comma=true;}if(r.auxiliary){if(comma)j<<',';j<<"\"provenance_source\"";comma=true;}if(r.watch){if(comma)j<<',';j<<"\"copy_source\"";}else if(r.auxiliary){if(comma)j<<',';j<<"\"staged_input\"";}
        j<<"],\"safe_association\":"<<(r.safe?"true":"false")<<",\"generation\":"<<r.generation
         <<",\"unobserved_prior_write\":"<<(r.unobservedPriorWrite?"true":"false")<<",\"unmatched\":"<<(r.unmatched?"true":"false");
        if(r.dimension==D3D11_RESOURCE_DIMENSION_BUFFER)j<<",\"buffer\":{\"bytes\":"<<r.buffer.ByteWidth<<",\"usage\":"<<r.buffer.Usage<<",\"bind_flags\":"<<r.buffer.BindFlags<<",\"cpu_access\":"<<r.buffer.CPUAccessFlags<<",\"misc_flags\":"<<r.buffer.MiscFlags<<",\"stride\":"<<r.buffer.StructureByteStride<<'}';
        else if(r.dimension==D3D11_RESOURCE_DIMENSION_TEXTURE2D)j<<",\"texture2d\":{\"width\":"<<r.texture.Width<<",\"height\":"<<r.texture.Height<<",\"mips\":"<<r.texture.MipLevels<<",\"array\":"<<r.texture.ArraySize<<",\"format\":"<<r.texture.Format<<",\"sample_count\":"<<r.texture.SampleDesc.Count<<",\"bind_flags\":"<<r.texture.BindFlags<<'}';
        j<<'}';
    }

public:
    void arm(){std::lock_guard<std::recursive_mutex> lock(mutex_);clear(true);active_=true;gateActive_.store(true,std::memory_order_release);}
    void arm(uint32_t meshFrame){std::lock_guard<std::recursive_mutex> lock(mutex_);clear(false);currentFrame_=meshFrame;active_=true;gateActive_.store(true,std::memory_order_release);}
    void reset(){std::lock_guard<std::recursive_mutex> lock(mutex_);clear(true);}
    bool active() const noexcept {return gateActive_.load(std::memory_order_acquire);}
    void finish(){std::lock_guard<std::recursive_mutex> lock(mutex_);active_=false;gateActive_.store(false,std::memory_order_release);}
    // Foreign/deferred contexts deliberately do not touch the resource ledger.
    // The epoch prevents reuse of a snapshot made before an unclassified write.
    void foreignWrite() noexcept {
        if(!gateActive_.load(std::memory_order_acquire))return;
        foreignWrites_.fetch_add(1,std::memory_order_relaxed);
        foreignEpoch_.fetch_add(1,std::memory_order_release);
    }
    void setFrame(uint32_t meshFrame){std::lock_guard<std::recursive_mutex> lock(mutex_);currentFrame_=meshFrame;}
    void noteDraw(ID3D11DeviceContext* ctx,uint32_t meshFrame,uint32_t eye,uint32_t firstRecord,
                  uint32_t instances,ID3D11Buffer* pool,ID3D11Buffer* ids,uint32_t idByteOffset,
                  const uint32_t* key16,uint64_t vertexShaderHash=0) {
        std::lock_guard<std::recursive_mutex> lock(mutex_);if(!active_)return;if(busy_){++counters_.suppressed;return;}
        currentFrame_=meshFrame;if(eye){++counters_.ignoredEye;return;}
        if(!haveDiscovery_){discoveryFrame_=meshFrame;haveDiscovery_=true;}
        const uint32_t poolId=addResource(pool,true,false),idsId=addResource(ids,false,true);
        // Three nomination frames cover Elite's rotating upload buffers before
        // the first captured frame.  Writes remain observed throughout.
        if(meshFrame<discoveryFrame_+3)return;
        if(meshFrame>discoveryFrame_+7){++counters_.ignoredFrame;if(!haveSelected_){missedWindow_=true;active_=false;gateActive_.store(false,std::memory_order_release);}return;}
        if(!haveSelected_){selectedFrame_=meshFrame;haveSelected_=true;}
        if(meshFrame!=selectedFrame_){++counters_.ignoredFrame;return;}
        if(draws_.size()>=kDrawCap){++counters_.drawOverflow;return;}
        Draw d;d.meshFrame=meshFrame;d.eye=eye;d.firstRecord=firstRecord;d.instances=instances;d.idByteOffset=idByteOffset;
        d.poolResource=poolId;d.idResource=idsId;d.vertexShaderHash=vertexShaderHash;if(key16){std::copy(key16,key16+16,d.key.begin());d.keyPresent=true;}
        if(poolId!=kNone){const auto& r=resources_[poolId];d.poolGeneration=r.generation;d.poolWriteObserved=!r.unobservedPriorWrite;d.poolMatched=d.poolWriteObserved&&!r.unmatched&&!openWriteMap(poolId);if(!d.poolWriteObserved)++counters_.unobservedSources;d.poolSnapshot=captureBuffer(ctx,poolId,meshFrame,"draw_pool");}
        if(idsId!=kNone){const auto& r=resources_[idsId];d.idGeneration=r.generation;d.idWriteObserved=!r.unobservedPriorWrite;d.idMatched=d.idWriteObserved&&!r.unmatched&&!openWriteMap(idsId);if(!d.idWriteObserved)++counters_.unobservedSources;d.idSnapshot=captureBuffer(ctx,idsId,meshFrame,"draw_ids");}
        draws_.push_back(std::move(d));
    }
    void noteMap(ID3D11DeviceContext* ctx,ID3D11Resource* resource,uint32_t subresource,
                 D3D11_MAP type,HRESULT result,bool hasData) {
        std::lock_guard<std::recursive_mutex> lock(mutex_);if(!active_)return;if(busy_){++counters_.suppressed;return;}
        if(FAILED(result)||!hasData)return;const uint32_t r=findResource(resource);if(!nominated(r))return;
        for(size_t n=pendingMaps_.size();n;--n){auto& old=pendingMaps_[n-1];if(old.context==ctx&&old.resource==resource&&old.subresource==subresource){
            if(old.event!=kNone&&old.event<events_.size())events_[old.event].completionStatus="duplicate_map";
            resources_[r].unmatched=true;++counters_.duplicateMaps;pendingMaps_.erase(pendingMaps_.begin()+n-1);break;}}
        if(type==D3D11_MAP_READ){if(pendingMaps_.size()<kWriteCap)pendingMaps_.push_back({ctx,resource,subresource,kNone,resources_[r].generation,type,false});else ++counters_.pendingMapOverflow;return;}
        const uint32_t e=appendEvent(r,MapWrite,ctx,nullptr,subresource,0,~0ull,true,false);
        if(e!=kNone&&e<events_.size())events_[e].mapType=uint32_t(type);
        if(e!=kNone&&e<events_.size()&&resources_[r].pool) {
            events_[e].sourceOwnerAttempt=sourceOwner_.captureMap(e,r,resources_[r].generation,
                currentFrame_,events_[e].foreignEpoch,resource,resources_[r].buffer.ByteWidth);
        }
        if(pendingMaps_.size()<kWriteCap)pendingMaps_.push_back({ctx,resource,subresource,e,resources_[r].generation,type,false});
        else {++counters_.pendingMapOverflow;resources_[r].unmatched=true;if(e!=kNone&&e<events_.size())events_[e].completionStatus="pending_map_cap";}
    }
    void noteUnmap(ID3D11DeviceContext* ctx,ID3D11Resource* resource,uint32_t subresource) {
        std::lock_guard<std::recursive_mutex> lock(mutex_);if(!active_)return;if(busy_){++counters_.suppressed;return;}
        for(size_t n=pendingMaps_.size();n;--n){auto p=pendingMaps_[n-1];if(p.context==ctx&&p.resource==resource&&p.subresource==subresource){
            pendingMaps_.erase(pendingMaps_.begin()+n-1);if(p.type==D3D11_MAP_READ)return;
            const uint32_t r=findResource(resource);if(p.event!=kNone&&p.event<events_.size()){
                auto& e=events_[p.event];e.complete=true;e.completionFrame=currentFrame_;e.completionStack=sampleStack();
                if(r!=kNone&&resources_[r].generation==p.generation){e.completionStatus="matched";resources_[r].unmatched=false;}
                else {e.completionStatus="generation_changed";if(r!=kNone)resources_[r].unmatched=true;++counters_.unmatchedWrites;}
            } else if(r!=kNone)resources_[r].unmatched=true;return;}}
        const uint32_t r=findResource(resource);if(nominated(r)){++counters_.unmatchedUnmaps;const uint32_t e=appendEvent(r,Unknown,ctx,nullptr,subresource,0,~0ull,false,true);if(e!=kNone&&e<events_.size())events_[e].completionStatus="unmatched_unmap";}
    }
    void noteWrite(ID3D11DeviceContext* ctx,ID3D11Resource* resource,uint32_t kind,
                   ID3D11Resource* source=nullptr,uint32_t subresource=0,uint64_t first=0,uint64_t end=~0ull) {
        std::lock_guard<std::recursive_mutex> lock(mutex_);if(!active_)return;if(busy_){++counters_.suppressed;return;}
        const uint32_t r=findResource(resource);if(!nominated(r))return;if(kind<Update||kind>Unknown)kind=Unknown;
        if(kind!=Unknown&&first==end)return;
        appendEvent(r,kind,ctx,source,subresource,first,end,false,true);
    }
    void unknownWrites() {
        std::lock_guard<std::recursive_mutex> lock(mutex_);if(!active_)return;if(busy_){++counters_.suppressed;return;}
        for(uint32_t i=0;i<resources_.size();++i)if(nominated(i))
            appendEvent(i,Unknown,nullptr,nullptr,0,0,~0ull,false,true);
    }
    void stage(ID3D11DeviceContext* ctx,uint32_t meshFrame,uint32_t eye,uint32_t sceneFrame,
               ID3D11Resource* sceneTexture,ID3D11Resource* coverageTexture,
               ID3D11Buffer* meshBuffer,uint32_t recordCount,ID3D11Texture2D* colour=nullptr) {
        std::lock_guard<std::recursive_mutex> lock(mutex_);if(!active_||busy_)return;
        if(!haveSelected_||meshFrame!=selectedFrame_||eye||sealed_){++counters_.stageRejects;return;}
        Ptr<ID3D11Predicate> predicate;BOOL predicateValue=FALSE;ctx->GetPredication(&predicate,&predicateValue);
        if(predicate)ctx->SetPredication(nullptr,FALSE);
        sceneFrame_=sceneFrame;captureTexture(ctx,sceneTexture,meshFrame,"scene_depth");
        captureTexture(ctx,coverageTexture,meshFrame,"mesh_coverage");
        captureStageBuffer(ctx,meshBuffer,meshFrame,"mesh_records",recordCount);
        if(colour)captureTexture(ctx,colour,meshFrame,"scene_colour");
        if(predicate)ctx->SetPredication(predicate.Get(),predicateValue);
        sealed_=true;active_=false;gateActive_.store(false,std::memory_order_release);
    }
    Summary summary() const {
        std::lock_guard<std::recursive_mutex> lock(mutex_);Summary s=counters_;s.active=active_;s.sealed=sealed_;
        s.currentFrame=currentFrame_;s.discoveryFrame=discoveryFrame_;s.selectedFrame=selectedFrame_;s.sceneFrame=sceneFrame_;
        s.resources=uint32_t(resources_.size());s.writes=uint32_t(events_.size());s.draws=uint32_t(draws_.size());
        s.snapshots=uint32_t(snapshots_.size());s.blobs=uint32_t(blobs_.size());s.observedWrites=observedWrites_;
        s.stackSamples=uint32_t(stacks_.size());s.foreignWrites=foreignWrites_.load(std::memory_order_acquire);
        const auto& o=sourceOwner_.summary();s.sourceOwnerMaps=o.mapsConsidered;s.sourceOwnerAttempts=o.attemptsStored;
        s.sourceOwnerComplete=o.completeAttempts;s.sourceOwnerPartial=o.partialAttempts;s.sourceOwnerIdentityRejects=o.identityRejects;
        s.sourceOwnerOpcodeRejects=o.opcodeRejects;s.sourceOwnerUnwindFailures=o.unwindFailures;s.sourceOwnerResourceMatches=o.resourceMatches;
        s.sourceOwnerReadFaults=o.readFaults;s.sourceOwnerMetadataChanges=o.metadataChanges;
        s.sourceOwnerDescriptorOverflow=o.descriptorOverflow;s.sourceOwnerCpuByteDeclines=o.cpuByteDeclines;return s;
    }
    uint32_t discoveryFrame() const{return summary().discoveryFrame;}
    uint32_t selectedFrame() const{return summary().selectedFrame;}
    uint32_t drawCount() const{return summary().draws;}
    uint32_t writeCount() const{return summary().writes;}
    uint64_t observedWriteCount() const{return summary().observedWrites;}
    bool sealed() const{return summary().sealed;}
    uint32_t captureSourceOwnerForTest(ID3D11Resource* resource,uintptr_t nestedOwner,uint32_t event=0) {
        std::lock_guard<std::recursive_mutex> lock(mutex_);const uint32_t r=findResource(resource);if(r==kNone)return kNone;
        const uint32_t attempt=sourceOwner_.captureSynthetic(event,r,resources_[r].generation,currentFrame_,
            foreignEpoch_.load(std::memory_order_acquire),resource,resources_[r].buffer.ByteWidth,nestedOwner);
        if(event<events_.size())events_[event].sourceOwnerAttempt=attempt;return attempt;
    }
    uint32_t noteMapSourceOwnerForTest(ID3D11DeviceContext* ctx,ID3D11Resource* resource,
                                       uint32_t subresource,D3D11_MAP type,uintptr_t nestedOwner) {
        std::lock_guard<std::recursive_mutex> lock(mutex_);const uint32_t r=findResource(resource);if(!nominated(r))return kNone;
        const uint32_t e=appendEvent(r,MapWrite,ctx,nullptr,subresource,0,~0ull,true,false);if(e==kNone)return kNone;
        events_[e].mapType=uint32_t(type);pendingMaps_.push_back({ctx,resource,subresource,e,resources_[r].generation,type,false});
        const uint32_t attempt=sourceOwner_.captureSynthetic(e,r,resources_[r].generation,currentFrame_,
            events_[e].foreignEpoch,resource,resources_[r].buffer.ByteWidth,nestedOwner);
        events_[e].sourceOwnerAttempt=attempt;return attempt;
    }
    void useExpectedExecutableIdentityForTest(){std::lock_guard<std::recursive_mutex> lock(mutex_);testIdentityOverride_=true;sourceOwner_.markSyntheticFixture();}
    const ObjectSourceOwnerProbe& sourceOwnerForTest() const noexcept{return sourceOwner_;}

    bool write(ID3D11DeviceContext* ctx,const wchar_t* directory,const wchar_t* stamp) {
        std::lock_guard<std::recursive_mutex> lock(mutex_);if(!ctx||!directory||!stamp)return false;
        const std::wstring binPath=path(directory,L"classification_",stamp,L".bin");
        const std::wstring jsonPath=path(directory,L"classification_",stamp,L".json");
        const std::string binName="classification_"+narrow(stamp)+".bin";
        // JSON is the publication marker.  Retire a previous marker before
        // touching its BIN so a failed rewrite cannot leave stale success
        // metadata pointing at new or partial bytes.
        if(!DeleteFileW(jsonPath.c_str())&&GetLastError()!=ERROR_FILE_NOT_FOUND)return false;
        FILE* bin=nullptr;const bool binOpened=_wfopen_s(&bin,binPath.c_str(),L"wb")==0&&bin;
        bool binOk=binOpened;uint64_t offset=0;Busy own(*this);
        for(auto& b:blobs_) {
            b.fileOffset=offset;b.fileBytes=0;if(!b.stage)continue;
            if(!binOpened){b.status="bin_open_failed";continue;}
            if(!binOk){b.status="bin_write_failed";continue;}
            D3D11_MAPPED_SUBRESOURCE m{};const HRESULT hr=ctx->Map(b.stage.Get(),0,D3D11_MAP_READ,0,&m);
            if(FAILED(hr)||!m.pData){b.status="readback_failed";++counters_.mapFailures;continue;}
            bool payloadOk=true,ioFailure=false;
            if(b.texture) {
                if(m.RowPitch<b.rowBytes){b.status="mapped_row_pitch";payloadOk=false;++counters_.mapFailures;}
                else for(uint32_t y=0;y<b.height;++y)if(fwrite(static_cast<const uint8_t*>(m.pData)+size_t(y)*m.RowPitch,1,b.rowBytes,bin)!=b.rowBytes){payloadOk=false;ioFailure=true;break;}
            } else if(fwrite(m.pData,1,size_t(b.reservedBytes),bin)!=b.reservedBytes){payloadOk=false;ioFailure=true;}
            ctx->Unmap(b.stage.Get(),0);
            if(payloadOk){b.fileBytes=b.reservedBytes;offset+=b.fileBytes;b.status="available";}
            else if(ioFailure){b.status="bin_write_failed";binOk=false;}
        }
        sourceOwner_.writeBinary(bin,offset,binOk);
        if(bin && fclose(bin)!=0)binOk=false;
        uint32_t peTimestamp=0,peImageSize=0;executableIdentity(peTimestamp,peImageSize);
        if(testIdentityOverride_){peTimestamp=ObjectSourceOwnerProbe::kExpectedTimestamp;peImageSize=ObjectSourceOwnerProbe::kExpectedImageSize;}
        const Summary s=summary();const bool cpuProvenance=s.foreignWrites==0;std::ostringstream j;
        j<<"{\n  \"schema\":\"edvr_object_classification_v2\",\n  \"binary\":"<<quote(binName)
         <<",\n  \"executable\":{\"pe_timestamp\":"<<peTimestamp<<",\"image_size\":"<<peImageSize<<"},\n"
         <<"  \"selection\":{\"discovery_mesh_frame\":"<<discoveryFrame_<<",\"selected_mesh_frame\":"<<selectedFrame_
         <<",\"scene_frame\":"<<sceneFrame_<<",\"has_discovery\":"<<(haveDiscovery_?"true":"false")<<",\"has_selection\":"<<(haveSelected_?"true":"false")<<",\"sealed\":"<<(sealed_?"true":"false")<<",\"missed_window\":"<<(missedWindow_?"true":"false")<<"},\n"
         <<"  \"limits\":{\"resources\":"<<kResourceCap<<",\"writes\":"<<kWriteCap<<",\"draws\":"<<kDrawCap<<",\"stacks\":"<<kStackCap
         <<",\"buffer_bytes\":"<<kBufferBudget<<",\"texture_bytes\":"<<kTextureBudget<<",\"single_buffer_bytes\":"<<kBufferCap<<"},\n"
         <<"  \"summary\":{\"status\":"<<quote(sealed_?"sealed":(missedWindow_?"selection_window_missed":(haveSelected_?"stage_missing":"no_selected_frame")))
         <<",\"resources\":"<<s.resources<<",\"events_stored\":"<<s.writes<<",\"writes_observed\":"<<s.observedWrites<<",\"draws\":"<<s.draws
         <<",\"snapshots\":"<<s.snapshots<<",\"blobs\":"<<s.blobs<<",\"stack_samples\":"<<s.stackSamples
         <<",\"resource_overflow\":"<<s.resourceOverflow<<",\"write_overflow\":"<<s.writeOverflow<<",\"draw_overflow\":"<<s.drawOverflow
         <<",\"stack_overflow\":"<<s.stackOverflow<<",\"snapshot_overflow\":"<<s.snapshotOverflow
         <<",\"buffer_budget_declines\":"<<s.bufferBudgetDeclines<<",\"texture_budget_declines\":"<<s.textureBudgetDeclines
         <<",\"unsafe_associations\":"<<s.unsafeAssociations<<",\"copy_failures\":"<<s.copyFailures<<",\"map_failures\":"<<s.mapFailures
         <<",\"unmatched_writes\":"<<s.unmatchedWrites<<",\"unobserved_sources\":"<<s.unobservedSources<<",\"foreign_writes\":"<<s.foreignWrites<<",\"cpu_provenance_available\":"<<(cpuProvenance?"true":"false")<<",\"ignored_eye\":"<<s.ignoredEye<<",\"ignored_frame\":"<<s.ignoredFrame
         <<",\"stage_rejects\":"<<s.stageRejects<<",\"duplicate_maps\":"<<s.duplicateMaps<<",\"unmatched_unmaps\":"<<s.unmatchedUnmaps<<",\"pending_map_overflow\":"<<s.pendingMapOverflow<<",\"open_map_snapshots\":"<<s.openMapSnapshots<<",\"suppressed_recursive\":"<<s.suppressed<<",\"binary_ok\":"<<(binOk?"true":"false")<<"},\n";
        j<<"  \"resources\":[";for(uint32_t i=0;i<resources_.size();++i){if(i)j<<',';writeResource(j,resources_[i],i);}j<<"],\n";
        j<<"  \"stacks\":[";for(uint32_t i=0;i<stacks_.size();++i){if(i)j<<',';const auto& x=stacks_[i];j<<"{\"id\":"<<i<<",\"captured\":"<<x.captured<<",\"game_frames\":"<<x.gameFrames<<",\"rvas\":"<<quote(x.rvas)<<'}';}j<<"],\n";
        j<<"  \"events\":[";for(uint32_t i=0;i<events_.size();++i){if(i)j<<',';const auto& e=events_[i];
            j<<"{\"id\":"<<i<<",\"resource\":"<<e.resource<<",\"kind\":"<<quote(kindName(e.kind))<<",\"kind_id\":"<<e.kind<<",\"mesh_frame\":"<<e.frame<<",\"generation\":"<<e.generation
             <<",\"context\":\"0x"<<std::hex<<e.context<<std::dec<<"\",\"source_resource\":";if(e.sourceResource==kNone)j<<"null";else j<<e.sourceResource;
            j<<",\"source_identity\":\"0x"<<std::hex<<e.sourceIdentity<<std::dec<<"\",\"source_generation\":"<<e.sourceGeneration<<",\"source_matched\":"<<(cpuProvenance&&e.sourceMatched?"true":"false")
             <<",\"foreign_epoch\":"<<e.foreignEpoch<<",\"subresource\":"<<e.subresource<<",\"map_type\":"<<e.mapType<<",\"first\":"<<e.first<<",\"end\":"<<e.end<<",\"mapped\":"<<(e.mapped?"true":"false")<<",\"complete\":"<<(e.complete?"true":"false")<<",\"completion_status\":"<<quote(e.completionStatus)<<",\"completion_frame\":"<<e.completionFrame<<",\"source_owner_attempt\":";
            if(e.sourceOwnerAttempt==kNone)j<<"null";else j<<e.sourceOwnerAttempt;j<<",\"stack\":";
            if(e.stack==kNone)j<<"null";else j<<e.stack;j<<",\"completion_stack\":";if(e.completionStack==kNone)j<<"null";else j<<e.completionStack;j<<'}';}j<<"],\n";
        j<<"  \"snapshots\":[";for(uint32_t i=0;i<snapshots_.size();++i){if(i)j<<',';const auto& x=snapshots_[i];j<<"{\"id\":"<<i<<",\"resource\":"<<x.resource<<",\"generation\":"<<x.generation<<",\"foreign_epoch\":"<<x.foreignEpoch<<",\"mesh_frame\":"<<x.meshFrame<<",\"blob\":"<<x.blob<<'}';}j<<"],\n";
        j<<"  \"draws\":[";for(uint32_t i=0;i<draws_.size();++i){if(i)j<<',';const auto& d=draws_[i];j<<"{\"id\":"<<i<<",\"mesh_frame\":"<<d.meshFrame<<",\"eye\":"<<d.eye<<",\"first_record\":"<<d.firstRecord<<",\"instances\":"<<d.instances<<",\"id_byte_offset\":"<<d.idByteOffset<<",\"vertex_shader_hash\":\"0x"<<std::hex<<d.vertexShaderHash<<std::dec<<"\",\"key_present\":"<<(d.keyPresent?"true":"false")<<",\"key16\":[";for(uint32_t k=0;k<16;++k){if(k)j<<',';j<<d.key[k];}j<<"],\"pool_resource\":";if(d.poolResource==kNone)j<<"null";else j<<d.poolResource;j<<",\"pool_generation\":"<<d.poolGeneration<<",\"pool_write_observed\":"<<(d.poolWriteObserved?"true":"false")<<",\"pool_matched\":"<<(cpuProvenance&&d.poolMatched?"true":"false")<<",\"pool_snapshot\":";if(d.poolSnapshot==kNone)j<<"null";else j<<d.poolSnapshot;j<<",\"id_resource\":";if(d.idResource==kNone)j<<"null";else j<<d.idResource;j<<",\"id_generation\":"<<d.idGeneration<<",\"id_write_observed\":"<<(d.idWriteObserved?"true":"false")<<",\"id_matched\":"<<(cpuProvenance&&d.idMatched?"true":"false")<<",\"id_snapshot\":";if(d.idSnapshot==kNone)j<<"null";else j<<d.idSnapshot;j<<'}';}j<<"],\n";
        j<<"  \"blobs\":[";for(uint32_t i=0;i<blobs_.size();++i){if(i)j<<',';const auto& b=blobs_[i];j<<"{\"id\":"<<i<<",\"name\":"<<quote(b.name)<<",\"status\":"<<quote(b.status)<<",\"source_resource\":";if(b.sourceResource==kNone)j<<"null";else j<<b.sourceResource;j<<",\"generation\":"<<b.generation<<",\"foreign_epoch\":"<<b.foreignEpoch<<",\"mesh_frame\":"<<b.meshFrame<<",\"subresource_or_record_count\":"<<b.subresource<<",\"texture\":"<<(b.texture?"true":"false")<<",\"format\":"<<b.format<<",\"width\":"<<b.width<<",\"height\":"<<b.height<<",\"row_bytes\":"<<b.rowBytes<<",\"offset\":"<<b.fileOffset<<",\"bytes\":"<<b.fileBytes<<'}';}j<<"],\n";
        sourceOwner_.writeJson(j);j<<"\n}\n";
        FILE* json=nullptr;bool jsonOk=_wfopen_s(&json,jsonPath.c_str(),L"wb")==0&&json;
        const std::string body=j.str();if(jsonOk)jsonOk=fwrite(body.data(),1,body.size(),json)==body.size();
        if(json&&fclose(json)!=0)jsonOk=false;return jsonOk&&binOk;
    }
};

inline ObjectClassificationProbe objectClassificationProbe;

} // namespace edvr
