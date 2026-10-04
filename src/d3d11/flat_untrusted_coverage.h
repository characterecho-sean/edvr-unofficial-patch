#pragma once

// Conservative original-fragment coverage, kept separately for at most two
// camera/resource domains until H identifies the world camera. World-camera
// and unrelated-depth buckets are discarded at that consumer. Each bucket is
// cleared once; later overdraw cannot erase an alternate fragment's mark.
#include "flat_overlay_layer.h"
#include "flat_mono_frame.h"
#include "flat_camera_phase.h"
#include "flat_runtime_model.h"
#include "../common/log.h"
#include <array>
#include <cstring>
#include <string>

namespace edvr {
struct FlatUntrustedNomination {
    bool candidate = false;
    bool preWorld = false;
    bool fullViewport = false;
    bool current = false;
    bool admissible() const { return candidate && fullViewport && current; }
};

// The runtime and its WARP regression use this one decision. A motion-source
// shader whitelist cannot determine colour ownership: unsupported original
// pixel shaders may draw the same camera domain before the world is named.
inline FlatUntrustedNomination flatUntrustedNomination(
    const FlatRuntimeDraw& draw,const void* namedDepth,
    const unsigned char* namedCamera,bool sceneExtent,bool enabled,
    uint32_t sequence,uint64_t frame,bool namesWorldSource=false) {
    FlatUntrustedNomination out{};
    const auto& k=draw.key;
    out.preWorld=!namedDepth;
    out.candidate=enabled && !namesWorldSource && k.format==23 && sceneExtent && k.color &&
        k.depth && k.dsv && k.camera &&
        (out.preWorld || (namedDepth==k.depth && namedCamera &&
                          std::memcmp(namedCamera,draw.camera,kFlatCameraBytes)!=0));
    if(out.candidate) {
        out.fullViewport=flat_mono_detail::fullViewport(k,k.width,k.height);
        out.current=flatRuntimeCameraCurrent(draw,sequence,frame);
    }
    return out;
}

inline bool flatUntrustedProvenCameraIndependent(const FlatShaderPairClassification& pair) {
    return pair.vs==FlatVsProjectionClass::InertNoCB &&
        pair.ps==FlatPsProjectionSafety::Clean;
}

struct FlatUntrustedObservedCamera {
    const void* depth = nullptr;
    const void* color = nullptr, *b1 = nullptr;
    const void* dsv = nullptr;
    uint64_t vs = 0, ps = 0, cameraHash = 0, writeEpoch = 0;
    uint32_t firstSeq = 0, writeSeq = 0, width = 0, height = 0, draws = 0;
    bool hasCamera = false;
    unsigned char camera[kFlatCameraBytes]{};
};

// The same aggregation used by H's consumer is exercised by the WARP
// regression. A missing camera is its own group and remains unaccountable.
inline FlatUntrustedObservedCamera* flatUntrustedObserveCamera(
    FlatUntrustedObservedCamera* entries,uint32_t capacity,uint32_t& used,
    const void* depth,const unsigned char* camera,bool* inserted=nullptr) {
    if(inserted)*inserted=false;
    for(uint32_t i=0;i<used;++i) {
        auto& prior=entries[i];
        if(prior.depth==depth && prior.hasCamera==(camera!=nullptr) &&
           (!camera || std::memcmp(prior.camera,camera,kFlatCameraBytes)==0)) {
            ++prior.draws;
            return &prior;
        }
    }
    if(used==capacity)return nullptr;
    auto& entry=entries[used++];
    entry={};entry.depth=depth;entry.hasCamera=camera!=nullptr;entry.draws=1;
    if(camera)std::memcpy(entry.camera,camera,kFlatCameraBytes);
    if(inserted)*inserted=true;
    return &entry;
}

struct FlatUntrustedDrawDiagnostic {
    uint64_t frame=0, vs=0, ps=0, cameraHash=0, writeEpoch=0, actualPs=0;
    uint32_t sequence=0, writeSeq=0, pending=0, completed=0;
    const void* color=nullptr, *depth=nullptr, *dsv=nullptr, *actualPsObject=nullptr;
    bool current=false, viewport=false, actualPsRead=false;
    const char* stage="none";
    std::string reason;
};

struct FlatUntrustedBucketDiagnostic {
    bool used=false, world=false, alternate=false, unrelatedDepth=false;
    bool dsvMatch=false, ready=false, cameraShape=false, phasePair=false;
    uint32_t pending=0, completed=0;
    uint64_t cameraHash=0;
    const void* color=nullptr, *depth=nullptr, *dsv=nullptr;
    unsigned char camera[kFlatCameraBytes]{};
    FlatUntrustedDrawDiagnostic firstFailure{};
};

struct FlatUntrustedQualification {
    const char* reason="not-checked";
    bool hIdentity=false, uniqueAlternate=false, dsvMatch=false;
    bool countPresent=false, ready=false, bucketValid=false;
    bool cameraShape=false, phasePair=false, recordCameraPresent=false;
    bool recordColor=false, recordExtent=false, frozenCamera=false;
    uint32_t expected=0, matching=0, first=0, last=0;
    uint64_t vs=0, ps=0;
    unsigned char recordCamera[kFlatCameraBytes]{};
};

class FlatUntrustedDiagnosticBudget {
    struct Signature { std::string stage,reason; } ordinary_[6];
    uint32_t ordinaryUsed_=0, emitted_=0, dropped_=0;
    uint64_t firstSupported_=0;
    bool secondSupported_=false;
public:
    bool take(uint64_t frame,bool supportedAlternateRefusal,
              const char* stage,const char* reason) {
        if(supportedAlternateRefusal) {
            if(!firstSupported_) {firstSupported_=frame;++emitted_;return true;}
            if(!secondSupported_ && frame>=firstSupported_+60) {
                secondSupported_=true;++emitted_;return true;
            }
            ++dropped_;return false;
        }
        if(!reason || !*reason)return false;
        for(uint32_t i=0;i<ordinaryUsed_;++i)
            if(ordinary_[i].stage==(stage?stage:"") && ordinary_[i].reason==reason) {
                ++dropped_;return false;
            }
        if(ordinaryUsed_==6) {++dropped_;return false;}
        ordinary_[ordinaryUsed_++]={stage?stage:"",reason};
        ++emitted_;return true;
    }
    uint32_t emitted() const {return emitted_;}
    uint32_t dropped() const {return dropped_;}
    bool firstSupported() const {return firstSupported_!=0;}
    bool secondSupported() const {return secondSupported_;}
};

class FlatUntrustedCoverage {
    template<class T> using Ptr = Microsoft::WRL::ComPtr<T>;
    static constexpr uint32_t kBuckets = 2;
    static constexpr uint32_t kMaxDraws = 128;
    struct Draw {
        uint64_t vs = 0, ps = 0;
        uint32_t sequence = 0;
        bool completed = false;
    };
    struct Bucket {
        FlatOverlayLayer layer;
        std::array<Draw,kMaxDraws> draws{};
        Ptr<ID3D11Texture2D> color, depth;
        Ptr<ID3D11DepthStencilView> dsv;
        unsigned char camera[kFlatCameraBytes]{};
        uint32_t width = 0, height = 0, count = 0;
        std::string failure;
        FlatUntrustedDrawDiagnostic nominee{}, firstFailure{};
        void reset() {
            layer.reset(); color.Reset(); depth.Reset(); dsv.Reset();
            width=height=count=0; failure.clear();nominee={};firstFailure={};
        }
        void beginFrame(uint64_t frame) {
            layer.beginFrame(frame); color.Reset(); depth.Reset(); dsv.Reset();
            width=height=count=0; failure.clear();nominee={};firstFailure={};
        }
    } buckets_[kBuckets];
    uint64_t frame_ = 0;
    uint32_t used_ = 0, planned_ = kBuckets, open_ = kBuckets, selected_ = kBuckets;
    bool consumerSeen_ = false;
    std::string globalFailure_, selectionFailure_;
    struct MaskSample {
        Ptr<ID3D11Texture2D> stage;
        uint64_t frame = 0;
        uint32_t width = 0, height = 0, polls = 0;
        bool reported = false;
    } samples_[2];
    static const char* sampleName(bool supportedAlternate) {
        return supportedAlternate?"supported-alternate":"unsupported-only";
    }
    void sampleUnavailable(uint32_t index,const char* reason) {
        auto& sample=samples_[index];
        if(sample.reported)return;
        Log::get().note("flat untrusted mask occupancy: category=%s frame=%llu status=unavailable reason=%s; one sample per category, no GPU wait",
            sampleName(index!=0),(unsigned long long)sample.frame,reason?reason:"unknown");
        sample.stage.Reset();sample.reported=true;
    }

    void recordFirstFailure(uint32_t index,const char* stage,const char* reason,
                            uint64_t actualPs=0,const void* actualPsObject=nullptr,
                            bool actualPsRead=false) {
        if(index>=used_ || !buckets_[index].firstFailure.reason.empty())return;
        auto& b=buckets_[index];
        b.firstFailure=b.nominee;
        b.firstFailure.stage=stage?stage:"unknown";
        b.firstFailure.reason=reason?reason:"untrusted-bucket-invalid";
        b.firstFailure.pending=b.count;
        for(uint32_t i=0;i<b.count;++i)if(b.draws[i].completed)++b.firstFailure.completed;
        b.firstFailure.actualPs=actualPs;
        b.firstFailure.actualPsObject=actualPsObject;
        b.firstFailure.actualPsRead=actualPsRead;
    }
    void bucketFailure(uint32_t index,const char* stage,const char* reason,
                       uint64_t actualPs=0,const void* actualPsObject=nullptr,
                       bool actualPsRead=false) {
        if(index>=used_ || !buckets_[index].failure.empty())return;
        recordFirstFailure(index,stage,reason,actualPs,actualPsObject,actualPsRead);
        buckets_[index].failure=reason?reason:"untrusted-bucket-invalid";
    }
    // Called only after H's authoritative camera exists. A failed world bucket
    // is immaterial, but two different non-world buckets cannot share one SRV.
    uint32_t alternateBucket(const void* depth,const void* dsv,
                             const unsigned char* worldBytes,float phaseX,float phaseY,
                             const char** reason) const {
        if(reason)*reason=nullptr;
        if(!worldBytes || !depth || !dsv || !globalFailure_.empty()) {
            if(reason)*reason=globalFailure_.empty()?"untrusted-H-identity":globalFailure_.c_str();
            return kBuckets;
        }
        float world[6][4]{};
        if(!flat_mono_detail::cameraShape(worldBytes,world)) {
            if(reason)*reason="untrusted-H-camera-shape";
            return kBuckets;
        }
        uint32_t chosen=kBuckets;
        for(uint32_t i=0;i<used_;++i) {
            const Bucket& b=buckets_[i];
            if(b.depth.Get()!=depth)continue;
            if(std::memcmp(b.camera,worldBytes,kFlatCameraBytes)==0)continue;
            if(chosen!=kBuckets) {
                if(reason)*reason="multiple-untrusted-camera-buckets";
                return kBuckets;
            }
            chosen=i;
        }
        if(chosen==kBuckets)return kBuckets;
        const Bucket& b=buckets_[chosen];
        if(b.dsv.Get()!=dsv || !b.count || !b.failure.empty() ||
           !b.layer.coverageReady(frame_,b.color.Get())) {
            if(reason)*reason=b.failure.empty()?"untrusted-bucket-incomplete":b.failure.c_str();
            return kBuckets;
        }
        float alternate[6][4]{};
        if(!flat_mono_detail::cameraShape(b.camera,alternate) ||
           !flatCameraCenteredPairAtPhase(world,alternate,phaseX,phaseY,b.width,b.height)) {
            if(reason)*reason="untrusted-camera-pose-or-phase";
            return kBuckets;
        }
        for(uint32_t i=0;i<b.count;++i)if(!b.draws[i].completed) {
            if(reason)*reason="untrusted-draw-incomplete";
            return kBuckets;
        }
        return chosen;
    }
public:
    void reset() {
        for(uint32_t i=0;i<2;++i)if(samples_[i].stage)
            sampleUnavailable(i,"resize-before-readback");
        for(auto& b:buckets_)b.reset();
        frame_=0;used_=0;planned_=open_=selected_=kBuckets;consumerSeen_=false;
        globalFailure_.clear();selectionFailure_.clear();
    }
    void beginFrame(uint64_t frame) {
        if(frame_==frame)return;
        for(auto& b:buckets_)b.beginFrame(frame);
        frame_=frame;used_=0;planned_=open_=selected_=kBuckets;consumerSeen_=false;
        globalFailure_.clear();selectionFailure_.clear();
    }
    void invalidate(const char* reason) {
        if(!consumerSeen_ && globalFailure_.empty())
            globalFailure_=reason?reason:"untrusted-coverage-invalid";
    }
    void noteMutation(ID3D11Resource* resource) {
        if(consumerSeen_ || !used_)return;
        if(!resource) {invalidate("untrusted-source-unknown-mutation");return;}
        for(uint32_t i=0;i<used_;++i)
            if(resource==buckets_[i].depth.Get() || resource==buckets_[i].color.Get())
                bucketFailure(i,"mutation","untrusted-source-explicit-mutation");
    }
    void consumer() { consumerSeen_=true; }
    bool active() const { return used_!=0; }
    bool finished() const { return consumerSeen_; }
    bool observesDepth(const void* depth) const {
        for(uint32_t i=0;i<used_;++i)if(buckets_[i].depth.Get()==depth)return true;
        return false;
    }
    bool plan(uint64_t frame,uint32_t sequence,ID3D11Texture2D* color,
               ID3D11Texture2D* depth,ID3D11DepthStencilView* dsv,
               uint64_t vs,uint64_t ps,const unsigned char* camera,bool admissible=true,
               const FlatUntrustedDrawDiagnostic* diagnostic=nullptr) {
        planned_=kBuckets;
        if(frame!=frame_ || consumerSeen_ || !color || !depth || !dsv || !camera ||
           !sequence || !globalFailure_.empty()) {
            invalidate("untrusted-source-identity");return false;
        }
        uint32_t index=0;
        for(;index<used_;++index) {
            const Bucket& b=buckets_[index];
            if(b.color.Get()==color && b.depth.Get()==depth && b.dsv.Get()==dsv &&
               std::memcmp(b.camera,camera,kFlatCameraBytes)==0)break;
        }
        const bool created=index==used_;
        if(created) {
            if(used_==kBuckets) {invalidate("untrusted-camera-bucket-cap");return false;}
            Bucket& b=buckets_[used_++];
            b.color=color;b.depth=depth;b.dsv=dsv;
            std::memcpy(b.camera,camera,sizeof(b.camera));
        }
        Bucket& b=buckets_[index];
        b.nominee=diagnostic?*diagnostic:FlatUntrustedDrawDiagnostic{};
        b.nominee.frame=frame;b.nominee.sequence=sequence;
        b.nominee.vs=vs;b.nominee.ps=ps;
        b.nominee.color=color;b.nominee.depth=depth;b.nominee.dsv=dsv;
        if(created) {
            D3D11_TEXTURE2D_DESC cd{},dd{};color->GetDesc(&cd);depth->GetDesc(&dd);
            b.width=cd.Width;b.height=cd.Height;
            if(!cd.Width || !cd.Height || cd.Width!=dd.Width || cd.Height!=dd.Height ||
                cd.SampleDesc.Count!=1 || dd.SampleDesc.Count!=1 ||
                uint64_t(cd.Width)*cd.Height>128ull*1024*1024)
                bucketFailure(index,"plan","untrusted-source-shape");
        }
        if(!vs || !ps)bucketFailure(index,"plan","untrusted-source-shader-identity");
        if(!admissible)bucketFailure(index,"plan","untrusted-alternate-unqualified");
        if(b.count>=kMaxDraws)bucketFailure(index,"plan","untrusted-bucket-draw-cap");
        if(!b.failure.empty())return false;
        Draw& draw=b.draws[b.count++];draw={};
        draw.vs=vs;draw.ps=ps;draw.sequence=sequence;
        planned_=index;
        return true;
    }
    void diagnosePlanShader(ID3D11DeviceContext* ctx,uint32_t sequence,
                            uint64_t (*shaderHash)(void*)) {
        if(!ctx || !shaderHash)return;
        for(uint32_t i=0;i<used_;++i) {
            auto& failure=buckets_[i].firstFailure;
            if(failure.sequence!=sequence || failure.actualPsRead ||
               std::strcmp(failure.stage,"plan")!=0 ||
               failure.reason!="untrusted-source-shader-identity")continue;
            Ptr<ID3D11PixelShader> actual;
            {FlatComputeInternalScope internal;ctx->PSGetShader(&actual,nullptr,nullptr);}
            failure.actualPsObject=actual.Get();
            failure.actualPs=shaderHash(actual.Get());
            failure.actualPsRead=true;
            return;
        }
    }
    bool beginDraw(ID3D11DeviceContext* ctx,uint64_t frame,
                   uint64_t (*shaderHash)(void*)=nullptr) {
        if(!ctx || frame!=frame_ || planned_>=used_ || open_!=kBuckets ||
           !globalFailure_.empty()) {
            if(planned_<used_)recordFirstFailure(planned_,"begin","untrusted-begin-precondition");
            return false;
        }
        Bucket& b=buckets_[planned_];
        if(!b.failure.empty())return false;
        const char* reason=nullptr;
        if(!b.layer.beginDraw(ctx,frame,b.color.Get(),b.dsv.Get(),&reason,true,true,true)) {
            Ptr<ID3D11PixelShader> originalPs;
            // Every failure inside beginDraw occurs before replacement or after
            // its restore, so this failure-only read sees the original binding.
            {FlatComputeInternalScope internal;ctx->PSGetShader(&originalPs,nullptr,nullptr);}
            bucketFailure(planned_,"begin",reason?reason:"untrusted-private-MRT-refused",
                shaderHash?shaderHash(originalPs.Get()):0,originalPs.Get(),true);
            return false;
        }
        open_=planned_;planned_=kBuckets;
        return true;
    }
    void endDraw(ID3D11DeviceContext* ctx,bool completed=true) {
        if(open_==kBuckets)return;
        Bucket& b=buckets_[open_];
        b.layer.endDraw(ctx);
        if(completed && ctx && !b.layer.refusal())b.draws[b.count-1].completed=true;
        else bucketFailure(open_,"end",b.layer.refusal()?b.layer.refusal():"untrusted-draw-incomplete");
        open_=kBuckets;
    }
    void abandon() {
        if(planned_<used_)bucketFailure(planned_,"abandon","untrusted-draw-scope-incomplete");
        planned_=kBuckets;
    }
    FlatUntrustedBucketDiagnostic diagnoseBucket(uint32_t index,const void* hDepth,
        const void* hDsv,const unsigned char* hCamera,float phaseX,float phaseY) const {
        FlatUntrustedBucketDiagnostic out{};
        if(index>=used_)return out;
        const Bucket& b=buckets_[index];
        out.used=true;out.color=b.color.Get();out.depth=b.depth.Get();out.dsv=b.dsv.Get();
        std::memcpy(out.camera,b.camera,kFlatCameraBytes);
        out.cameraHash=b.nominee.cameraHash;out.pending=b.count;
        for(uint32_t i=0;i<b.count;++i)if(b.draws[i].completed)++out.completed;
        out.unrelatedDepth=!hDepth || b.depth.Get()!=hDepth;
        out.world=!out.unrelatedDepth && hCamera &&
            std::memcmp(b.camera,hCamera,kFlatCameraBytes)==0;
        out.alternate=!out.unrelatedDepth && hCamera && !out.world;
        out.dsvMatch=hDsv && b.dsv.Get()==hDsv;
        out.ready=b.layer.coverageReady(frame_,b.color.Get());
        float world[6][4]{},alternate[6][4]{};
        out.cameraShape=hCamera && flat_mono_detail::cameraShape(hCamera,world) &&
            flat_mono_detail::cameraShape(b.camera,alternate);
        out.phasePair=out.cameraShape && flatCameraCenteredPairAtPhase(
            world,alternate,phaseX,phaseY,b.width,b.height);
        out.firstFailure=b.firstFailure;
        return out;
    }
    uint32_t bucketCount() const {return used_;}
    const char* globalFailure() const {return globalFailure_.empty()?nullptr:globalFailure_.c_str();}
    bool qualifies(const FlatContractRecord& record,const unsigned char* worldBytes,
                    float phaseX,float phaseY,FlatUntrustedQualification* diagnostic=nullptr) const {
        const auto& k=record.key;
        if(diagnostic) {
            *diagnostic={};
            diagnostic->hIdentity=worldBytes && k.depth && k.dsv;
            diagnostic->recordCameraPresent=k.camera!=nullptr;
            diagnostic->expected=record.draws;
            diagnostic->vs=k.vs;diagnostic->ps=k.ps;
            diagnostic->first=record.first;diagnostic->last=record.last;
            if(k.camera)std::memcpy(diagnostic->recordCamera,record.camera,kFlatCameraBytes);
            uint32_t candidate=kBuckets,found=0;
            for(uint32_t i=0;i<used_;++i)if(buckets_[i].depth.Get()==k.depth &&
                worldBytes && std::memcmp(buckets_[i].camera,worldBytes,kFlatCameraBytes)!=0) {
                candidate=i;++found;
            }
            diagnostic->uniqueAlternate=found==1;
            if(found==1) {
                const Bucket& b=buckets_[candidate];
                diagnostic->dsvMatch=b.dsv.Get()==k.dsv;
                diagnostic->countPresent=b.count!=0;
                diagnostic->ready=b.layer.coverageReady(frame_,b.color.Get());
                diagnostic->bucketValid=b.failure.empty();
                diagnostic->recordColor=b.color.Get()==k.color;
                diagnostic->recordExtent=b.width==k.width && b.height==k.height;
                diagnostic->frozenCamera=k.camera &&
                    std::memcmp(b.camera,record.camera,kFlatCameraBytes)==0;
                float world[6][4]{},alternate[6][4]{};
                diagnostic->cameraShape=flat_mono_detail::cameraShape(worldBytes,world) &&
                    flat_mono_detail::cameraShape(b.camera,alternate);
                diagnostic->phasePair=diagnostic->cameraShape &&
                    flatCameraCenteredPairAtPhase(world,alternate,phaseX,phaseY,b.width,b.height);
                for(uint32_t i=0;i<b.count;++i) {
                    const Draw& draw=b.draws[i];
                    if(draw.sequence>=record.first && draw.sequence<=record.last &&
                       draw.vs==k.vs && draw.ps==k.ps)++diagnostic->matching;
                }
            }
        }
        const char* reason=nullptr;
        const uint32_t index=alternateBucket(k.depth,k.dsv,worldBytes,phaseX,phaseY,&reason);
        if(index==kBuckets || !k.camera) {
            if(diagnostic)diagnostic->reason=reason?reason:
                (!k.camera?"record-camera-missing":"no-alternate-bucket");
            return false;
        }
        const Bucket& b=buckets_[index];
        if(b.color.Get()!=k.color || b.width!=k.width || b.height!=k.height ||
            std::memcmp(b.camera,record.camera,kFlatCameraBytes)!=0) {
            if(diagnostic)diagnostic->reason="record-color-extent-or-frozen-camera";
            return false;
        }
        uint32_t matching=0;
        for(uint32_t i=0;i<b.count;++i) {
            const Draw& draw=b.draws[i];
            if(draw.sequence>=record.first && draw.sequence<=record.last &&
               draw.vs==k.vs && draw.ps==k.ps)++matching;
        }
        if(diagnostic)diagnostic->reason=matching==record.draws?
            "qualified":"record-draw-receipts";
        return matching==record.draws;
    }
    // The runtime compares these original-game-draw receipts with every
    // observed native scene draw in a non-world camera/depth group.
    uint32_t completedDraws(const void* depth,const unsigned char* camera) const {
        if(!camera)return 0;
        uint32_t count=0;
        for(uint32_t i=0;i<used_;++i) {
            const Bucket& b=buckets_[i];
            if(b.depth.Get()!=depth || std::memcmp(b.camera,camera,kFlatCameraBytes)!=0)continue;
            for(uint32_t j=0;j<b.count;++j)if(b.draws[j].completed)++count;
        }
        return count;
    }
    bool select(const void* depth,const void* dsv,const unsigned char* worldBytes,
                float phaseX,float phaseY) {
        selected_=kBuckets;selectionFailure_.clear();
        const char* reason=nullptr;
        const uint32_t index=alternateBucket(depth,dsv,worldBytes,phaseX,phaseY,&reason);
        if(index==kBuckets) {
            if(reason)selectionFailure_=reason;
            return false;
        }
        selected_=index;
        return true;
    }
    bool mixed() const { return selected_<used_; }
    void sampleMask(ID3D11DeviceContext* ctx,bool supportedAlternate) {
        const uint32_t index=supportedAlternate?1u:0u;
        auto& sample=samples_[index];
        if(sample.reported || sample.stage)return;
        sample.frame=frame_;
        if(!ctx || !view()) {sampleUnavailable(index,"treated-coverage-unavailable");return;}
        FlatComputeInternalScope internal;
        Ptr<ID3D11Device> dev;ctx->GetDevice(&dev);
        ID3D11Texture2D* source=buckets_[selected_].layer.coverageTexture();
        if(!dev || !source) {sampleUnavailable(index,"source-or-device-unavailable");return;}
        D3D11_TEXTURE2D_DESC desc{};source->GetDesc(&desc);
        if(desc.Format!=DXGI_FORMAT_R8_UNORM || desc.MipLevels!=1 || desc.ArraySize!=1 ||
           desc.SampleDesc.Count!=1 || !desc.Width || !desc.Height) {
            sampleUnavailable(index,"coverage-shape-unavailable");return;
        }
        sample.width=desc.Width;sample.height=desc.Height;
        desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=0;
        desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;desc.MiscFlags=0;
        if(FAILED(dev->CreateTexture2D(&desc,nullptr,&sample.stage))) {
            sampleUnavailable(index,"staging-create-failed");return;
        }
        ctx->CopyResource(sample.stage.Get(),source);
        Log::get().note("flat untrusted mask occupancy: category=%s frame=%llu status=queued extent=%ux%u readback=nonblocking",
            sampleName(supportedAlternate),(unsigned long long)frame_,sample.width,sample.height);
    }
    void pollMask(ID3D11DeviceContext* ctx) {
        if(!ctx)return;
        for(uint32_t i=0;i<2;++i) {
            auto& sample=samples_[i];
            if(!sample.stage || sample.reported)continue;
            D3D11_MAPPED_SUBRESOURCE mapped{};
            FlatComputeInternalScope internal;
            const HRESULT hr=ctx->Map(sample.stage.Get(),0,D3D11_MAP_READ,
                                      D3D11_MAP_FLAG_DO_NOT_WAIT,&mapped);
            if(SUCCEEDED(hr)) {
                uint64_t marked=0;
                for(uint32_t y=0;y<sample.height;++y) {
                    const auto* row=static_cast<const uint8_t*>(mapped.pData)+size_t(y)*mapped.RowPitch;
                    for(uint32_t x=0;x<sample.width;++x)marked+=row[x]!=0;
                }
                ctx->Unmap(sample.stage.Get(),0);
                Log::get().note("flat untrusted mask occupancy: category=%s frame=%llu status=measured marked=%llu total=%llu fraction=%.6f; conservative original-fragment union",
                    sampleName(i!=0),(unsigned long long)sample.frame,
                    (unsigned long long)marked,
                    (unsigned long long)(uint64_t(sample.width)*sample.height),
                    double(marked)/double(uint64_t(sample.width)*sample.height));
                sample.stage.Reset();sample.reported=true;
            } else if(hr==DXGI_ERROR_WAS_STILL_DRAWING && ++sample.polls<120) {
                continue;
            } else sampleUnavailable(i,hr==DXGI_ERROR_WAS_STILL_DRAWING?
                "readback-timeout":"readback-map-failed");
        }
    }
    ID3D11ShaderResourceView* view() const {
        if(!globalFailure_.empty() || selected_>=used_)return nullptr;
        const Bucket& b=buckets_[selected_];
        return b.failure.empty() && b.layer.coverageReady(frame_,b.color.Get())?
            b.layer.coverageView():nullptr;
    }
    const char* failure() const {
        if(!globalFailure_.empty())return globalFailure_.c_str();
        if(!selectionFailure_.empty())return selectionFailure_.c_str();
        if(selected_<used_)return buckets_[selected_].failure.empty()?
            nullptr:buckets_[selected_].failure.c_str();
        if(!consumerSeen_)for(uint32_t i=0;i<used_;++i)
            if(!buckets_[i].failure.empty())return buckets_[i].failure.c_str();
        return nullptr;
    }
    uint32_t drawCount() const {
        uint32_t count=0;for(uint32_t i=0;i<used_;++i)count+=buckets_[i].count;
        return count;
    }
};

inline bool flatUntrustedObservationAccounted(
    const FlatUntrustedObservedCamera& observed,
    const FlatUntrustedCoverage& capture,uint32_t* completed=nullptr) {
    const uint32_t count=observed.hasCamera?
        capture.completedDraws(observed.depth,observed.camera):0;
    if(completed)*completed=count;
    return observed.hasCamera && count==observed.draws;
}
} // namespace edvr
