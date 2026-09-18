#include "mesh_motion.h"
#include "mesh_motion_shader.h"
#include "vscreen.h"
#include "binding_shadow.h"
#include "depth_probe.h"
#include "shader_swap.h"
#include "gpu_interval.h"
#include "menu.h"
#include "native_benchmark_collector.h"
#include "perf_monitor.h"
#include "../common/log.h"
#include <wrl/client.h>
#include <unordered_map>
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>
namespace edvr { namespace mesh_motion_detail {
template<class T>using Ptr=Microsoft::WRL::ComPtr<T>;
constexpr unsigned maxRecords=512,stride=240;
constexpr unsigned maxInstanceBytes=1024*1024;
constexpr uint64_t materialVs=0xEB5234DB6ADB491Dull,faceVs=0xDE545DC8EE4FBB87ull;
constexpr uint64_t multiUvVs=0x61AE8EB05FDC18DDull,litMultiUvVs=0x66DE2CADB1F4AE6Bull,detailVs=0xAACFDCF2FB9AD809ull;
enum CoverageKind { Material,Face,MultiUv,LitMultiUv,Detail,CoverageCount };
CoverageKind coverageKind(uint64_t hash){
    switch(hash){case materialVs:return Material;case faceVs:return Face;case multiUvVs:return MultiUv;
    case litMultiUvVs:return LitMultiUv;case detailVs:return Detail;default:return CoverageCount;}
}
struct History {
    Ptr<ID3D11Buffer> buffer;
    Ptr<ID3D11ShaderResourceView> srv;
    Ptr<ID3D11UnorderedAccessView> uav;
    Ptr<IUnknown> sources[maxRecords][4];
    unsigned count=0;
};
struct Eye {
    Ptr<ID3D11Texture2D> scene,coverage;
    Ptr<ID3D11RenderTargetView> rtv;
    Ptr<ID3D11ShaderResourceView> srv;
    History history[2];
    unsigned write=0,width=0,height=0;
    bool cleared=false,matched=false;
} eyes[2];
bool enabled=false,failed=false,noted=false,capped=false;
Ptr<ID3D11ComputeShader> capture,match;
Ptr<ID3D11PixelShader> coverageShaders[CoverageCount];
Ptr<ID3D11Buffer> settings,instances,captureInputs;
Ptr<ID3D11ShaderResourceView> instanceView,captureInputView;
Ptr<ID3D11DepthStencilState> depthState;
Ptr<ID3D11BlendState> blendState;
GpuIntervals<16> drawGpu,matchGpu,captureGpu;
GpuIntervals<16> comparisonCopyGpu,comparisonCoverageGpu,comparisonFlushGpu;
unsigned frames=0,draws=0,captureBatches=0,capturedInstances=0;
bool diagnosticWindowHadComparison=false;
constexpr unsigned diagnosticWindowFrames=1800,capProbeBudget=32;
enum class FlushReason { InputChange,WriteOrMap,EyeConsumption,FrameBoundary,GpuWritable,Count };
enum class InputChange { Context,Scene,Ids,Pool,Output,Oversized,Count };
const char* flushReasonName(FlushReason reason){
    static const char* names[]={"input-change","write/map","eye-consumption","frame-boundary","gpu-writable"};
    return names[unsigned(reason)];
}
struct EyeDiagnostics {
    uint64_t candidateDraws=0,candidateInstances=0;
    uint64_t acceptedDraws=0,acceptedInstances=0;
    uint64_t capRejectedDraws=0,capRejectedInstances=0;
    uint64_t sampledCapDraws=0,sampledCapInstances=0;
    uint64_t eligibleCapRejectedDraws=0,eligibleCapRejectedInstances=0;
    uint64_t ineligibleCapSampleDraws=0,ineligibleCapSampleInstances=0;
    unsigned capProbes=0;
};
struct CapProbe {
    EyeDiagnostics* diagnostic=nullptr;unsigned instances=0;bool eligible=false;
    CapProbe(EyeDiagnostics* value,unsigned count):diagnostic(value),instances(count){if(diagnostic){++diagnostic->sampledCapDraws;diagnostic->sampledCapInstances+=instances;}}
    ~CapProbe(){if(diagnostic && !eligible){++diagnostic->ineligibleCapSampleDraws;diagnostic->ineligibleCapSampleInstances+=instances;}}
};
struct Diagnostics {
    EyeDiagnostics eye[2];
    uint64_t flushes[unsigned(FlushReason::Count)]{};
    uint64_t flushInstances=0;
    unsigned largestBatch=0;
    uint64_t drawCalls=0,flushCalls=0;
    double drawCpuMs=0,flushCpuMs=0;
    uint64_t drawCpuSamples=0,flushCpuSamples=0;
    uint64_t depthMetadataHits=0,depthMetadataFills=0;
    uint64_t inputChanges[unsigned(InputChange::Count)]{};
    uint64_t rangedIdCopies=0,rangedIdInstances=0;
    void clearWindow(){*this=Diagnostics{};}
} diagnostics;

enum class OversizedMode : uint8_t { Immediate, Packed };
enum class ComparisonStage : uint8_t { Idle, Armed, Settling, Running };
const char* comparisonPhaseName(unsigned phase){static const char* names[]={"A1","B","A2"};return phase<3?names[phase]:"?";}
const char* oversizedModeName(OversizedMode mode){return mode==OversizedMode::Packed?"packed-batch":"immediate";}
struct ComparisonMetrics {
    uint64_t entryCalls=0;
    uint64_t acceptedDraws=0,acceptedInstances=0,oversizedDraws=0,oversizedInstances=0;
    uint64_t idCopies=0,idCopyBytes=0,wholeCopies=0,rangedCopies=0;
    uint64_t batches=0,batchInstances=0,flushes[unsigned(FlushReason::Count)]{};
    double entryCpuMs=0,acceptedCpuMs=0,idCopyCpuMs=0,flushCpuMs=0;
    uint64_t entryCpuSamples=0,acceptedCpuSamples=0,idCopyCpuSamples=0,flushCpuSamples=0;
    uint64_t copyGpuSubmitted=0,coverageGpuSubmitted=0,flushGpuSubmitted=0;
    uint64_t flushGpuFrame=~uint64_t(0);
};
struct ComparisonState {
    ComparisonStage stage=ComparisonStage::Idle;
    OversizedMode mode=OversizedMode::Immediate;
    unsigned phase=0;
    bool cancelRequested=false,nativeSampling=false,measurement=false,everMeasured=false,haveReport=false;
    uint64_t epoch=0,disturbanceEpoch=0,settleUntilMs=0,phaseDeadlineMs=0,nativeWindow=0,nativeScope=0;
    const char* failure=nullptr;
    NativeBenchmarkReport report{};
    NativeBenchmarkMetadata baseline{};
    bool haveBaseline=false;
    ComparisonMetrics metrics{};
} comparison;

bool comparisonActive(){return comparison.stage!=ComparisonStage::Idle;}
void clearComparisonMetrics(){comparison.metrics={};}
bool sameBenchmarkMetadata(const NativeBenchmarkMetadata& a,const NativeBenchmarkMetadata& b){
    return !std::memcmp(a.inputWidth,b.inputWidth,sizeof(a.inputWidth)) &&
        !std::memcmp(a.inputHeight,b.inputHeight,sizeof(a.inputHeight)) &&
        !std::memcmp(a.outputWidth,b.outputWidth,sizeof(a.outputWidth)) &&
        !std::memcmp(a.outputHeight,b.outputHeight,sizeof(a.outputHeight)) &&
        a.refreshMilliHz==b.refreshMilliHz && !std::memcmp(a.gameFov,b.gameFov,sizeof(a.gameFov)) &&
        !std::memcmp(a.treatments,b.treatments,sizeof(a.treatments)) && a.featureEpoch==b.featureEpoch &&
        !std::strcmp(a.runtime,b.runtime) && !std::strcmp(a.headset,b.headset) &&
        !std::strcmp(a.aaMode,b.aaMode) && !std::strcmp(a.dlssMode,b.dlssMode) &&
        !std::strcmp(a.build,b.build);
}
double cpuTimestamp(){LARGE_INTEGER value;QueryPerformanceCounter(&value);static const double scale=[](){LARGE_INTEGER f;QueryPerformanceFrequency(&f);return 1000.0/double(f.QuadPart);}();return double(value.QuadPart)*scale;}
struct CpuSample {
    double* total=nullptr;uint64_t* samples=nullptr;double start=0;
    CpuSample(bool active,double& sum,uint64_t& count){if(active){total=&sum;samples=&count;start=cpuTimestamp();}}
    ~CpuSample(){if(total){*total+=cpuTimestamp()-start;++*samples;}}
};
struct DualCpuSample {
    double *firstTotal=nullptr,*secondTotal=nullptr;uint64_t *firstSamples=nullptr,*secondSamples=nullptr;double start=0;
    DualCpuSample(bool active,double& total,uint64_t& samples,double* alsoTotal=nullptr,uint64_t* alsoSamples=nullptr){if(active){firstTotal=&total;firstSamples=&samples;secondTotal=alsoTotal;secondSamples=alsoSamples;start=cpuTimestamp();}}
    ~DualCpuSample(){if(firstTotal){const double elapsed=cpuTimestamp()-start;*firstTotal+=elapsed;++*firstSamples;if(secondTotal&&secondSamples){*secondTotal+=elapsed;++*secondSamples;}}}
};
uint64_t pendingGpu(uint64_t submitted,const GpuIntervals<16>::Totals& totals){
    const uint64_t retired=uint64_t(totals.samples)+totals.invalid;
    return submitted>retired?submitted-retired:0;
}
void resetComparisonGpu(ID3D11DeviceContext* ctx){
    comparisonCopyGpu.reset(ctx);comparisonCoverageGpu.reset(ctx);comparisonFlushGpu.reset(ctx);
}
void abortComparison(ID3D11DeviceContext* ctx,const char* reason){
    const unsigned phase=comparison.phase;
    const bool sampled=comparison.everMeasured;
    if(sampled){
        const auto& m=comparison.metrics;const auto& copy=comparisonCopyGpu.totals;const auto& coverage=comparisonCoverageGpu.totals;const auto& flush=comparisonFlushGpu.totals;
        Log::get().note("motion comparison INCOMPLETE: phase %s, oversized draws %llu; CPU samples full/accepted/copy/flush %llu/%llu/%llu/%llu; GPU copy ready/submitted/skipped/invalid/pending %u/%llu/%u/%u/%llu, coverage %u/%llu/%u/%u/%llu, flush %u/%llu/%u/%u/%llu. These are instrument coverage only, not a comparison result.",comparisonPhaseName(phase),(unsigned long long)m.oversizedDraws,(unsigned long long)m.entryCpuSamples,(unsigned long long)m.acceptedCpuSamples,(unsigned long long)m.idCopyCpuSamples,(unsigned long long)m.flushCpuSamples,copy.samples,(unsigned long long)m.copyGpuSubmitted,copy.skipped,copy.invalid,(unsigned long long)pendingGpu(m.copyGpuSubmitted,copy),coverage.samples,(unsigned long long)m.coverageGpuSubmitted,coverage.skipped,coverage.invalid,(unsigned long long)pendingGpu(m.coverageGpuSubmitted,coverage),flush.samples,(unsigned long long)m.flushGpuSubmitted,flush.skipped,flush.invalid,(unsigned long long)pendingGpu(m.flushGpuSubmitted,flush));
    }
    comparison.mode=OversizedMode::Immediate;comparison.stage=ComparisonStage::Idle;
    comparison.cancelRequested=comparison.nativeSampling=comparison.measurement=comparison.everMeasured=comparison.haveReport=false;
    comparison.disturbanceEpoch=comparison.settleUntilMs=comparison.phaseDeadlineMs=comparison.nativeWindow=comparison.nativeScope=0;
    comparison.failure=nullptr;comparison.haveBaseline=false;clearComparisonMetrics();resetComparisonGpu(ctx);++comparison.epoch;
    Log::get().note("motion comparison: aborted in phase %s; %s%s",comparisonPhaseName(phase),reason?reason:"unknown reason",sampled?".":"; the 30-second sample never opened, so no zero-valued component result is reported.");
    menuNotify(reason&&std::strstr(reason,"cancelled")?"Motion comparison cancelled; normal capture restored.":"Motion comparison aborted; see the graphics log.");
}
void beginComparisonPhase(ID3D11DeviceContext* ctx,uint64_t nowMs){
    comparison.stage=ComparisonStage::Running;
    comparison.mode=comparison.phase==1?OversizedMode::Packed:OversizedMode::Immediate;
    comparison.cancelRequested=comparison.nativeSampling=comparison.measurement=comparison.everMeasured=comparison.haveReport=false;
    comparison.nativeWindow=comparison.nativeScope=0;comparison.failure=nullptr;comparison.phaseDeadlineMs=nowMs+50000;
    clearComparisonMetrics();resetComparisonGpu(ctx);++comparison.epoch;
    Log::get().note("motion comparison: phase %s started, oversized capture %s; native 2 s warmup, exact 30 s sample and 2 s drain must complete by %llu ms. Hold the landed view and settings unchanged.",comparisonPhaseName(comparison.phase),oversizedModeName(comparison.mode),(unsigned long long)comparison.phaseDeadlineMs);
}
void logComparisonMetrics(const NativeBenchmarkReport& report){
    const auto& m=comparison.metrics;const auto& copy=comparisonCopyGpu.totals;const auto& coverage=comparisonCoverageGpu.totals;const auto& flush=comparisonFlushGpu.totals;
    Log::get().note("motion comparison metrics: phase %s, mode %s, native window %llu scope %llu sample [%llu..%llu]; mesh hook entries %llu, accepted %llu draws/%llu instances, oversized %llu/%llu, ID copies %llu (%llu bytes; whole %llu ranged %llu), capture %llu batches/%llu instances.",
        comparisonPhaseName(comparison.phase),oversizedModeName(comparison.mode),(unsigned long long)report.window,(unsigned long long)report.scope,(unsigned long long)report.startedAtMs,(unsigned long long)report.sampleEndedAtMs,
        (unsigned long long)m.entryCalls,(unsigned long long)m.acceptedDraws,(unsigned long long)m.acceptedInstances,(unsigned long long)m.oversizedDraws,(unsigned long long)m.oversizedInstances,(unsigned long long)m.idCopies,(unsigned long long)m.idCopyBytes,(unsigned long long)m.wholeCopies,(unsigned long long)m.rangedCopies,(unsigned long long)m.batches,(unsigned long long)m.batchInstances);
    Log::get().note("motion comparison CPU: phase %s, sampled full mesh hook %.3f us (%llu samples, includes early returns and cap checks), accepted path %.3f us (%llu; nested explanatory span, not added to full hook; includes copy enqueue, setup, coverage and capture flush when caused by that draw), ID CopySubresourceRegion call %.3f us (%llu), capture flush %.3f us (%llu).",
        comparisonPhaseName(comparison.phase),m.entryCpuSamples?m.entryCpuMs*1000/m.entryCpuSamples:0,(unsigned long long)m.entryCpuSamples,m.acceptedCpuSamples?m.acceptedCpuMs*1000/m.acceptedCpuSamples:0,(unsigned long long)m.acceptedCpuSamples,m.idCopyCpuSamples?m.idCopyCpuMs*1000/m.idCopyCpuSamples:0,(unsigned long long)m.idCopyCpuSamples,m.flushCpuSamples?m.flushCpuMs*1000/m.flushCpuSamples:0,(unsigned long long)m.flushCpuSamples);
    Log::get().note("motion comparison GPU: phase %s, ID copy %.3f us (%u ready/%llu submitted, %u skipped, %u invalid, %llu pending), coverage reissue %.3f us (%u/%llu, %u skipped, %u invalid, %llu pending), capture dispatch %.3f us (%u/%llu, %u skipped, %u invalid, %llu pending); asynchronous command-stream intervals, not synchronized attribution to a CPU or native frame; no waits, flushes or readbacks.",
        comparisonPhaseName(comparison.phase),copy.samples?copy.ms*1000/copy.samples:0,copy.samples,(unsigned long long)m.copyGpuSubmitted,copy.skipped,copy.invalid,(unsigned long long)pendingGpu(m.copyGpuSubmitted,copy),coverage.samples?coverage.ms*1000/coverage.samples:0,coverage.samples,(unsigned long long)m.coverageGpuSubmitted,coverage.skipped,coverage.invalid,(unsigned long long)pendingGpu(m.coverageGpuSubmitted,coverage),flush.samples?flush.ms*1000/flush.samples:0,flush.samples,(unsigned long long)m.flushGpuSubmitted,flush.skipped,flush.invalid,(unsigned long long)pendingGpu(m.flushGpuSubmitted,flush));
    Log::get().note("motion comparison flushes: phase %s, input-change %llu, write/map %llu, eye-consumption %llu, frame-boundary %llu, gpu-writable %llu.",comparisonPhaseName(comparison.phase),(unsigned long long)m.flushes[unsigned(FlushReason::InputChange)],(unsigned long long)m.flushes[unsigned(FlushReason::WriteOrMap)],(unsigned long long)m.flushes[unsigned(FlushReason::EyeConsumption)],(unsigned long long)m.flushes[unsigned(FlushReason::FrameBoundary)],(unsigned long long)m.flushes[unsigned(FlushReason::GpuWritable)]);
}
struct IndexStamp { unsigned epoch=0,seen=0; };
struct GeometryStamp {
    unsigned epoch=0,seen=0;
    // The game's shared IB also receives transient geometry uploads. Its
    // draw slices are exact; a write elsewhere must not drop hull history.
    // Entries expire with the two-frame history, bounding this cache by
    // the per-eye draw cap. Vertex inputs remain conservative whole buffers.
    std::unordered_map<uint64_t,IndexStamp> indices;
};
std::unordered_map<ID3D11Resource*,GeometryStamp> watched;
unsigned geometryEpoch=0,geometryWrites=0,unknownWrites=0,rangeWrites=0,disjointIndices=0;
Ptr<ID3D11Buffer> dump,dumpPrevious;
unsigned dumpCount=0,dumpPreviousCount=0,dumpSceneFrame=~0u,dumpMeshFrame=0,dumpEye=0,dumpWidth=0,dumpHeight=0,dumpWriteSlot=0;
struct Settings { UINT info[4],key[16];float dimensions[4]; };
struct PendingCapture {
    Ptr<ID3D11DeviceContext> context;
    Ptr<ID3D11Buffer> scene;
    Ptr<ID3D11Buffer> ids;
    Ptr<ID3D11ShaderResourceView> pool;
    Ptr<ID3D11Resource> poolResource;
    Ptr<ID3D11UnorderedAccessView> output;
    Settings inputs[maxRecords];
    unsigned count=0;
    void clear(){count=0;context.Reset();scene.Reset();ids.Reset();pool.Reset();poolResource.Reset();output.Reset();}
} pending;
// Retaining both COM objects makes pointer reuse impossible during the frame.
// Only immutable texture metadata is cached; scene-pick and eye selection below
// stay live because other depth-probe callers can change the current pair.
struct DepthMetadata {
    Ptr<ID3D11DepthStencilView> view;
    Ptr<ID3D11Texture2D> texture;
    D3D11_TEXTURE2D_DESC desc{};
    void clear(){view.Reset();texture.Reset();desc={};}
    bool get(ID3D11DepthStencilView* bound,Ptr<ID3D11Texture2D>& out,D3D11_TEXTURE2D_DESC& outDesc){
        if(view.Get()==bound){out=texture;outDesc=desc;++diagnostics.depthMetadataHits;return true;}
        clear();Ptr<ID3D11Resource> resource;bound->GetResource(&resource);Ptr<ID3D11Texture2D> candidate;
        if(FAILED(resource.As(&candidate)))return false;
        candidate->GetDesc(&desc);view=bound;texture=candidate;out=candidate;outDesc=desc;++diagnostics.depthMetadataFills;return true;
    }
} depthMetadata;
struct ComputeState {
    ID3D11DeviceContext* ctx;
    Ptr<ID3D11ComputeShader> shader;
    ID3D11ClassInstance* classes[256]{};UINT count=256;
    ID3D11Buffer* cb[4]{};ID3D11ShaderResourceView* srv[4]{};Ptr<ID3D11UnorderedAccessView> uav;
    Ptr<ID3D11Predicate> predicate;BOOL predicateValue=FALSE;
    explicit ComputeState(ID3D11DeviceContext* c):ctx(c){ctx->CSGetShader(&shader,classes,&count);ctx->CSGetConstantBuffers(0,4,cb);ctx->CSGetShaderResources(0,4,srv);ctx->CSGetUnorderedAccessViews(0,1,&uav);ctx->GetPredication(&predicate,&predicateValue);ctx->SetPredication(nullptr,FALSE);}
    ~ComputeState(){
        ID3D11ShaderResourceView* none[4]{};ID3D11UnorderedAccessView* noUav=nullptr;
        ctx->CSSetShaderResources(0,4,none);ctx->CSSetUnorderedAccessViews(0,1,&noUav,nullptr);
        ctx->CSSetShader(shader.Get(),classes,count);ctx->CSSetConstantBuffers(0,4,cb);ctx->CSSetShaderResources(0,4,srv);
        UINT keep=~0u;ctx->CSSetUnorderedAccessViews(0,1,uav.GetAddressOf(),&keep);
        ctx->SetPredication(predicate.Get(),predicateValue);
        for(auto* p:cb)if(p)p->Release();for(auto* p:srv)if(p)p->Release();for(UINT i=0;i<count;++i)classes[i]->Release();
    }
};
bool uploadSettings(ID3D11DeviceContext* ctx,const Settings& data){
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if(FAILED(ctx->Map(settings.Get(),0,D3D11_MAP_WRITE_DISCARD,0,&mapped))){
        failed=true;pending.clear();Log::get().note("mesh motion: settings upload failed; existing motion retained.");return false;
    }
    std::memcpy(mapped.pData,&data,sizeof(data));ctx->Unmap(settings.Get(),0);return true;
}
void copyInstanceIds(ID3D11DeviceContext* ctx,UINT dst,ID3D11Buffer* ids,UINT first,UINT end,bool whole){
    D3D11_BOX box{first,0,0,end,1,1};
    if(!whole){++diagnostics.rangedIdCopies;diagnostics.rangedIdInstances+=(end-first)/8;}
    if(!comparison.measurement){ctx->CopySubresourceRegion(instances.Get(),0,dst,0,0,ids,0,&box);return;}
    auto& m=comparison.metrics;++m.idCopies;m.idCopyBytes+=end-first;if(whole)++m.wholeCopies;else ++m.rangedCopies;
    const bool sample=(m.idCopies&63u)==0;
    const bool gpu=sample && comparisonCopyGpu.begin(ctx);if(gpu)++m.copyGpuSubmitted;
    {CpuSample cpu(sample,m.idCopyCpuMs,m.idCopyCpuSamples);ctx->CopySubresourceRegion(instances.Get(),0,dst,0,0,ids,0,&box);}
    if(gpu)comparisonCopyGpu.end(ctx);
}
void flushCapture(FlushReason reason){
    if(!pending.count)return;
    const unsigned batchSize=pending.count;
    const bool sample=(++diagnostics.flushCalls&15u)==0;
    DualCpuSample cpu(sample,diagnostics.flushCpuMs,diagnostics.flushCpuSamples,
        comparison.measurement?&comparison.metrics.flushCpuMs:nullptr,
        comparison.measurement?&comparison.metrics.flushCpuSamples:nullptr);
    auto* ctx=pending.context.Get();
    {
        ComputeState saved(ctx);bool timed=false;
        if(comparison.measurement){
            if(frames%16==0 && comparison.metrics.flushGpuFrame!=frames){comparison.metrics.flushGpuFrame=frames;timed=comparisonFlushGpu.begin(ctx);if(timed)++comparison.metrics.flushGpuSubmitted;}
        }else timed=captureGpu.begin(ctx);
        Settings data{};data.info[2]=pending.count;
        D3D11_BOX box{0,0,0,UINT(pending.count*sizeof(Settings)),1,1};
        ctx->UpdateSubresource(captureInputs.Get(),0,&box,pending.inputs,0,0);
        if(!uploadSettings(ctx,data)){if(timed){if(comparison.measurement)comparisonFlushGpu.end(ctx);else captureGpu.end(ctx);}return;}
        ID3D11Buffer* cbs[4]={nullptr,pending.scene.Get(),nullptr,settings.Get()};
        ID3D11ShaderResourceView* srvs[4]={pending.pool.Get(),instanceView.Get(),nullptr,captureInputView.Get()};
        ctx->CSSetShader(capture.Get(),nullptr,0);ctx->CSSetConstantBuffers(0,4,cbs);
        ctx->CSSetShaderResources(0,4,srvs);ctx->CSSetUnorderedAccessViews(0,1,pending.output.GetAddressOf(),nullptr);
        ctx->Dispatch((pending.count+63)/64,1,1);
        if(timed){if(comparison.measurement)comparisonFlushGpu.end(ctx);else captureGpu.end(ctx);}
    }
    ++captureBatches;capturedInstances+=pending.count;
    ++diagnostics.flushes[unsigned(reason)];diagnostics.flushInstances+=batchSize;
    diagnostics.largestBatch=std::max(diagnostics.largestBatch,batchSize);
    if(comparison.measurement){++comparison.metrics.batches;comparison.metrics.batchInstances+=batchSize;++comparison.metrics.flushes[unsigned(reason)];}
    pending.clear();
}
bool prepare(ID3D11DeviceContext* ctx,ID3D11Device* dev){
    if(capture)return true;
    capture.Attach(shaderSwapCompileCs(ctx,kMeshMotionHlsl,sizeof(kMeshMotionHlsl)-1,"capture","mesh capture",nullptr,"mesh motion"));
    match.Attach(shaderSwapCompileCs(ctx,kMeshMotionHlsl,sizeof(kMeshMotionHlsl)-1,"match","mesh match",nullptr,"mesh motion"));
    const char* entries[CoverageCount]={"material","face","multiUv","litMultiUv","detail"};
    for(unsigned i=0;i<CoverageCount;++i){
        coverageShaders[i].Attach(shaderSwapCompilePs(ctx,kMeshCoverageHlsl,sizeof(kMeshCoverageHlsl)-1,entries[i],"mesh coverage",nullptr,"mesh motion"));
        if(!coverageShaders[i])return false;
    }
    D3D11_BUFFER_DESC b{};b.ByteWidth=sizeof(Settings);b.BindFlags=D3D11_BIND_CONSTANT_BUFFER;b.Usage=D3D11_USAGE_DYNAMIC;b.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;
    if(FAILED(dev->CreateBuffer(&b,nullptr,&settings)))return false;
    b={};b.ByteWidth=maxInstanceBytes;b.BindFlags=D3D11_BIND_SHADER_RESOURCE;b.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
    D3D11_SHADER_RESOURCE_VIEW_DESC s{};s.Format=DXGI_FORMAT_R32_TYPELESS;s.ViewDimension=D3D11_SRV_DIMENSION_BUFFEREX;s.BufferEx.NumElements=b.ByteWidth/4;s.BufferEx.Flags=D3D11_BUFFEREX_SRV_FLAG_RAW;
    if(FAILED(dev->CreateBuffer(&b,nullptr,&instances)) || FAILED(dev->CreateShaderResourceView(instances.Get(),&s,&instanceView)))return false;
    b.ByteWidth=maxRecords*sizeof(Settings);b.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;b.StructureByteStride=sizeof(Settings);
    if(FAILED(dev->CreateBuffer(&b,nullptr,&captureInputs)) || FAILED(dev->CreateShaderResourceView(captureInputs.Get(),nullptr,&captureInputView)))return false;
    D3D11_DEPTH_STENCIL_DESC d{};d.DepthEnable=TRUE;d.DepthFunc=D3D11_COMPARISON_EQUAL;d.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ZERO;
    D3D11_BLEND_DESC blend{};blend.RenderTarget[0].RenderTargetWriteMask=3;
    return capture && match && SUCCEEDED(dev->CreateDepthStencilState(&d,&depthState)) && SUCCEEDED(dev->CreateBlendState(&blend,&blendState));
}
bool createEye(ID3D11Device* dev,ID3D11Texture2D* source,Eye& e){
    D3D11_TEXTURE2D_DESC td{};source->GetDesc(&td);e=Eye{};e.scene=source;e.width=td.Width;e.height=td.Height;
    td={};td.Width=e.width;td.Height=e.height;td.MipLevels=td.ArraySize=td.SampleDesc.Count=1;
    td.Format=DXGI_FORMAT_R32G32_FLOAT;td.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
    if(FAILED(dev->CreateTexture2D(&td,nullptr,&e.coverage)) || FAILED(dev->CreateRenderTargetView(e.coverage.Get(),nullptr,&e.rtv)) || FAILED(dev->CreateShaderResourceView(e.coverage.Get(),nullptr,&e.srv)))return false;
    D3D11_BUFFER_DESC b{};b.ByteWidth=maxRecords*stride;b.StructureByteStride=stride;b.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;b.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS;
    for(auto& h:e.history)if(FAILED(dev->CreateBuffer(&b,nullptr,&h.buffer)) || FAILED(dev->CreateShaderResourceView(h.buffer.Get(),nullptr,&h.srv)) || FAILED(dev->CreateUnorderedAccessView(h.buffer.Get(),nullptr,&h.uav)))return false;
    return true;
}
void fail(){failed=true;Log::get().note("mesh motion: resource setup failed; existing camera/object motion retained.");}
const char* benchmarkAbortName(uint32_t reason){
    switch(reason){case kNativeBenchmarkScopeChanged:return "native benchmark scope changed (menu, setting or diagnostic activity)";case kNativeBenchmarkOverflow:return "native benchmark storage overflowed";case kNativeBenchmarkClockReversed:return "native benchmark clock reversed";case kNativeBenchmarkTransportLoss:return "native timing transport lost samples";default:return "native benchmark did not complete";}
}
void completeComparison(ID3D11DeviceContext* ctx){
    comparison.mode=OversizedMode::Immediate;comparison.stage=ComparisonStage::Idle;
    comparison.cancelRequested=comparison.nativeSampling=comparison.measurement=comparison.everMeasured=comparison.haveReport=false;
    comparison.disturbanceEpoch=comparison.settleUntilMs=comparison.phaseDeadlineMs=comparison.nativeWindow=comparison.nativeScope=0;
    comparison.failure=nullptr;comparison.haveBaseline=false;clearComparisonMetrics();resetComparisonGpu(ctx);++comparison.epoch;
    Log::get().note("motion comparison: A/B/A complete; normal oversized capture restored to the immediate path. The three native benchmark lines and phase-tagged component lines are the result.");
    menuNotify("Motion comparison complete; normal capture restored.");
}
void processComparisonBoundary(ID3D11DeviceContext* ctx){
    if(comparison.stage==ComparisonStage::Idle)return;
    if(comparison.cancelRequested){abortComparison(ctx,"cancelled from the Instruments page");return;}
    if(failed){abortComparison(ctx,"mesh capture resource or shader setup failed");return;}
    const uint64_t now=GetTickCount64();
    if(comparison.stage==ComparisonStage::Armed){
        if(menuOpen())return;
        comparison.stage=ComparisonStage::Settling;comparison.disturbanceEpoch=perfMonitorBenchmarkDisturbanceEpoch();comparison.settleUntilMs=now+10000;
        Log::get().note("motion comparison: menu closed; settling for 10 seconds before phase A1. Loading and this settling interval are excluded. Hold the landed view unchanged for about two minutes.");
        menuNotify("Motion comparison settles for 10 seconds; hold the view.");
        return;
    }
    if(perfMonitorBenchmarkDisturbanceEpoch()!=comparison.disturbanceEpoch){abortComparison(ctx,"menu, setting, census or eye-capture activity changed the benchmark environment");return;}
    if(comparison.stage==ComparisonStage::Settling){
        if(menuOpen()){abortComparison(ctx,"the settings menu reopened during settling");return;}
        if(now<comparison.settleUntilMs)return;
        beginComparisonPhase(ctx,now);return;
    }
    if(menuOpen()){abortComparison(ctx,"the settings menu reopened");return;}
    if(comparison.failure){abortComparison(ctx,comparison.failure);return;}
    if(now>=comparison.phaseDeadlineMs){abortComparison(ctx,"no matching completed native benchmark arrived within 50 seconds");return;}
    if(!comparison.haveReport)return;
    comparison.haveReport=false;const auto report=comparison.report;
    if(report.aborted || report.abortReason!=kNativeBenchmarkCompleted){abortComparison(ctx,benchmarkAbortName(report.abortReason));return;}
    if(!report.cpu.available || !report.gpu.available){abortComparison(ctx,"the native benchmark completed without both CPU and GPU samples");return;}
    if(!comparison.everMeasured){abortComparison(ctx,"the matching native sample completed but the component sampling gate never opened");return;}
    if(!comparison.metrics.oversizedDraws){abortComparison(ctx,"this view produced no eligible oversized mesh draws");return;}
    const auto& cm=comparison.metrics;const auto& copy=comparisonCopyGpu.totals;const auto& coverage=comparisonCoverageGpu.totals;const auto& flush=comparisonFlushGpu.totals;
    if(!cm.entryCpuSamples || !cm.acceptedCpuSamples || !cm.idCopyCpuSamples || !cm.flushCpuSamples || !copy.samples || !coverage.samples || !flush.samples){abortComparison(ctx,"one or more required mesh CPU/GPU component measurements were unavailable");return;}
    if(pendingGpu(cm.copyGpuSubmitted,copy) || pendingGpu(cm.coverageGpuSubmitted,coverage) || pendingGpu(cm.flushGpuSubmitted,flush)){abortComparison(ctx,"sampled GPU component queries remained pending after the native drain");return;}
    if(comparison.haveBaseline && !sameBenchmarkMetadata(comparison.baseline,report.metadata)){abortComparison(ctx,"runtime, headset, render size, refresh, FOV or treatment metadata changed between phases");return;}
    if(!comparison.haveBaseline){comparison.baseline=report.metadata;comparison.haveBaseline=true;}
    logComparisonMetrics(report);
    Log::get().note("motion comparison: phase %s accepted native window %llu scope %llu; advancing only after its completed report.",comparisonPhaseName(comparison.phase),(unsigned long long)report.window,(unsigned long long)report.scope);
    if(comparison.phase==2){completeComparison(ctx);return;}
    ++comparison.phase;beginComparisonPhase(ctx,now);
}
} // namespace mesh_motion_detail
void meshMotionConfigure(bool on){using namespace mesh_motion_detail;if(on!=enabled){if(!on&&comparisonActive()){Log::get().note("motion comparison: aborted because mesh motion was disabled; normal immediate mode will be used when it is enabled again.");menuNotify("Motion comparison aborted: mesh motion was disabled.");}meshMotionShutdown();enabled=on;if(on)Log::get().note("mesh motion diagnostics: 1800-frame windows active; draw CPU sampled 1/256 calls, capture flush CPU sampled 1/16 batches, capped eligibility probed for at most 32 draws per eye in one frame per window (bounded sample only; no population estimate).");}}
void meshMotionRequestComparison(){
    using namespace mesh_motion_detail;
    if(!enabled){Log::get().note("motion comparison: refused because mesh motion is disabled.");menuNotify("Motion comparison unavailable: mesh motion is off.");return;}
    if(failed){Log::get().note("motion comparison: refused because mesh capture resource or shader setup has failed.");menuNotify("Motion comparison unavailable: mesh capture failed.");return;}
    if(comparisonActive()){
        if(!comparison.cancelRequested){comparison.cancelRequested=true;Log::get().note("motion comparison: cancellation requested; the frame boundary will restore immediate oversized capture.");menuNotify("Motion comparison cancellation requested.");}
        return;
    }
    comparison.stage=ComparisonStage::Armed;comparison.mode=OversizedMode::Immediate;comparison.phase=0;
    comparison.cancelRequested=comparison.nativeSampling=comparison.measurement=comparison.everMeasured=comparison.haveReport=false;
    comparison.disturbanceEpoch=comparison.settleUntilMs=comparison.phaseDeadlineMs=comparison.nativeWindow=comparison.nativeScope=0;comparison.failure=nullptr;comparison.haveBaseline=false;clearComparisonMetrics();
    Log::get().note("motion comparison: armed A1 immediate / B packed-batch / A2 immediate. Close the menu, then hold one landed view and all settings unchanged for about two minutes. Ten seconds of settling, native warmups and drains are excluded.");
    Log::get().note("motion comparison instrumentation: full hook CPU reuses the existing 1/256 clock; accepted path, ID-copy CPU/GPU and coverage GPU sample fixed accepted/copy-call schedules; capture GPU samples at most the first flush in one of 16 frames. The common schedule replaces the rolling per-batch capture query only during each exact sample.");
    menuNotify("Comparison armed. Close the menu and hold the landed view.");
}
uint64_t meshMotionComparisonScopeEpoch(){using namespace mesh_motion_detail;return comparison.epoch;}
void meshMotionComparisonNativeSampling(bool active,uint64_t window,uint64_t scope){
    using namespace mesh_motion_detail;if(comparison.stage!=ComparisonStage::Running || comparison.haveReport)return;
    if(!window || !scope){comparison.failure="native benchmark scope or window identity was unavailable";comparison.measurement=false;return;}
    // An abort is delivered as inactive(old window,new scope), followed by a
    // report carrying (old window,old scope). Associate only the first active
    // sampling tuple; never manufacture a mixed identity from that abort tick.
    if(!comparison.nativeScope){if(!active)return;comparison.nativeScope=scope;comparison.nativeWindow=window;}
    else if(scope!=comparison.nativeScope || window!=comparison.nativeWindow){comparison.failure="native benchmark scope changed during a comparison phase";comparison.measurement=false;return;}
    comparison.nativeSampling=active;comparison.measurement=active;if(active)comparison.everMeasured=true;
}
void meshMotionComparisonNativeReport(const NativeBenchmarkReport& report){
    using namespace mesh_motion_detail;if(comparison.stage!=ComparisonStage::Running || comparison.haveReport)return;
    // Epoch changes intentionally retire an older collector window. Only the
    // scope/window adopted by this phase may advance or abort it.
    if(!comparison.nativeScope || report.scope!=comparison.nativeScope || report.window!=comparison.nativeWindow)return;
    comparison.report=report;comparison.haveReport=true;comparison.measurement=comparison.nativeSampling=false;
}
void meshMotionDraw(ID3D11DeviceContext* ctx,PanelCurveDrawFn issue,unsigned count,unsigned n,unsigned start,int base,unsigned startInstance,uint64_t hash){
    using namespace mesh_motion_detail;
    if(comparison.measurement)++comparison.metrics.entryCalls;
    const bool entrySample=(++diagnostics.drawCalls&255u)==0;
    DualCpuSample cpu(entrySample,diagnostics.drawCpuMs,diagnostics.drawCpuSamples,
        comparison.measurement?&comparison.metrics.entryCpuMs:nullptr,
        comparison.measurement?&comparison.metrics.entryCpuSamples:nullptr);
    const auto kind=coverageKind(hash);
    if(!enabled || failed || kind==CoverageCount || !ctx || !issue || !n || n>64 || !count || count%3 || ctx->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE)return;
    auto* bound=static_cast<ID3D11DepthStencilView*>(bindingGet(BindSlot::Dsv0));if(!bound)return;
    Ptr<ID3D11Texture2D> scene;D3D11_TEXTURE2D_DESC td{};if(!depthMetadata.get(bound,scene,td) || !depthProbeIsSceneDepth(scene.Get()))return;
    if(td.ArraySize!=1 || td.SampleDesc.Count!=1)return;
    int eye=-1;for(int i=0;i<2;++i){ID3D11Texture2D* s=nullptr;uint32_t fmt=0;if(depthProbeSceneDepthFormat(td.Width,td.Height,i,&s,&fmt) && s==scene.Get()){eye=i;break;}}
    if(eye<0)return;
    Eye& e=eyes[eye];if(e.matched)return; // no history mutation after this eye is consumed
    const bool overCap=e.scene==scene && e.history[e.write].count+n>maxRecords;
    bool probeCap=false;
    if(overCap){
        auto& d=diagnostics.eye[eye];
        ++d.capRejectedDraws;d.capRejectedInstances+=n;
        probeCap=frames%diagnosticWindowFrames==0 && d.capProbes<capProbeBudget;
        if(probeCap)++d.capProbes;
        else {
            if(!capped){capped=true;Log::get().note("mesh motion: 512 instances per eye reached; excess geometry retains existing motion.");}
            return;
        }
    }
    CapProbe capProbe(probeCap?&diagnostics.eye[eye]:nullptr,n);
    Ptr<ID3D11DepthStencilState> ds;UINT ref=0;ctx->OMGetDepthStencilState(&ds,&ref);
    D3D11_DEPTH_STENCIL_DESC dd{};if(ds)ds->GetDesc(&dd);else {dd.DepthEnable=TRUE;dd.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ALL;}
    if(!dd.DepthEnable || dd.DepthWriteMask!=D3D11_DEPTH_WRITE_MASK_ALL)return;
    Ptr<ID3D11BlendState> blend;FLOAT factors[4]{};UINT mask=0;ctx->OMGetBlendState(&blend,factors,&mask);
    D3D11_BLEND_DESC bd{};if(blend)blend->GetDesc(&bd);if(bd.AlphaToCoverageEnable || bd.RenderTarget[0].BlendEnable)return;
    UINT nv=1;D3D11_VIEWPORT vp{};ctx->RSGetViewports(&nv,&vp);
    if(nv!=1 || vp.TopLeftX || vp.TopLeftY || vp.Width!=td.Width || vp.Height!=td.Height || vp.MinDepth!=0 || vp.MaxDepth!=1)return;
    D3D11_PRIMITIVE_TOPOLOGY topology;ctx->IAGetPrimitiveTopology(&topology);if(topology!=D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST)return;
    Ptr<ID3D11GeometryShader> gs;Ptr<ID3D11HullShader> hs;Ptr<ID3D11DomainShader> dom;ctx->GSGetShader(&gs,nullptr,nullptr);ctx->HSGetShader(&hs,nullptr,nullptr);ctx->DSGetShader(&dom,nullptr,nullptr);if(gs || hs || dom)return;
    Ptr<ID3D11Predicate> predicate;BOOL pred=FALSE;ctx->GetPredication(&predicate,&pred);if(predicate)return;
    ID3D11Buffer* so[4]{};ctx->SOGetTargets(4,so);bool busy=false;for(auto* p:so)if(p){busy=true;p->Release();}if(busy)return;
    ID3D11UnorderedAccessView* om[8]{};ctx->OMGetRenderTargetsAndUnorderedAccessViews(0,nullptr,nullptr,0,8,om);for(auto* p:om)if(p){busy=true;p->Release();}if(busy)return;
    Ptr<ID3D11Buffer> vb,ids,ib,cb;UINT vertexStride=0,offset=0,idStride=0,idOffset=0,indexOffset=0;DXGI_FORMAT format{};
    ctx->IAGetVertexBuffers(0,1,&ids,&idStride,&idOffset);ctx->IAGetVertexBuffers(1,1,&vb,&vertexStride,&offset);ctx->IAGetIndexBuffer(&ib,&format,&indexOffset);ctx->VSGetConstantBuffers(1,1,&cb);
    if(!vb || !ids || !ib || !cb || idStride!=8 || vertexStride!=40)return;
    const UINT indexBytes=format==DXGI_FORMAT_R16_UINT?2:format==DXGI_FORMAT_R32_UINT?4:0;
    const uint64_t indexFirst=uint64_t(indexOffset)+uint64_t(start)*indexBytes;
    const uint64_t indexEnd=indexFirst+uint64_t(count)*indexBytes;
    if(!indexBytes || indexEnd>UINT64_C(0xffffffff))return;
    D3D11_BUFFER_DESC idd{},cbd{};ids->GetDesc(&idd);cb->GetDesc(&cbd);uint64_t at=uint64_t(idOffset)+uint64_t(startInstance)*8;
    if(at%4 || at+n*8>idd.ByteWidth || cbd.ByteWidth<276*16)return;
    Ptr<ID3D11ShaderResourceView> pool;ctx->VSGetShaderResources(33,1,&pool);if(!pool)return;
    D3D11_SHADER_RESOURCE_VIEW_DESC pd{};pool->GetDesc(&pd);Ptr<ID3D11Resource> pr;pool->GetResource(&pr);Ptr<ID3D11Buffer> pb;
    if(pd.ViewDimension!=D3D11_SRV_DIMENSION_BUFFER || pd.Buffer.FirstElement || FAILED(pr.As(&pb)))return;
    D3D11_BUFFER_DESC pbd{};pb->GetDesc(&pbd);if(pbd.StructureByteStride!=336 || pd.Buffer.NumElements!=pbd.ByteWidth/336)return;
    auto& diagnostic=diagnostics.eye[eye];++diagnostic.candidateDraws;diagnostic.candidateInstances+=n;
    if(probeCap){
        capProbe.eligible=true;
        ++diagnostic.eligibleCapRejectedDraws;diagnostic.eligibleCapRejectedInstances+=n;
        if(!capped){capped=true;Log::get().note("mesh motion: 512 instances per eye reached; excess geometry retains existing motion.");}
        return;
    }
    Ptr<ID3D11Device> dev;ctx->GetDevice(&dev);if(!prepare(ctx,dev.Get()) || (e.scene!=scene && !createEye(dev.Get(),scene.Get(),e))){fail();return;}
    auto& now=e.history[e.write];auto& prev=e.history[1-e.write];
    // Coverage only needs the copied instance IDs. Defer transform capture
    // until an input changes or the eye is consumed, rather than breaking
    // the graphics pipeline with one compute dispatch for every mesh.
    const bool wholeIds=idd.ByteWidth<=maxInstanceBytes;
    const bool packedOversized=!wholeIds && comparison.mode==OversizedMode::Packed;
    const bool acceptedCpuSample=comparison.measurement && ((comparison.metrics.acceptedDraws+1)&63u)==0;
    CpuSample acceptedCpu(acceptedCpuSample,comparison.metrics.acceptedCpuMs,comparison.metrics.acceptedCpuSamples);
    const bool gpuWritable=((pbd.BindFlags|idd.BindFlags)&D3D11_BIND_UNORDERED_ACCESS)!=0;
    unsigned inputChanges=0;
    if(pending.count){
        if(pending.context.Get()!=ctx)inputChanges|=1u<<unsigned(InputChange::Context);
        if(pending.scene!=cb)inputChanges|=1u<<unsigned(InputChange::Scene);
        if(pending.ids!=ids)inputChanges|=1u<<unsigned(InputChange::Ids);
        if(pending.pool!=pool)inputChanges|=1u<<unsigned(InputChange::Pool);
        if(pending.output!=now.uav)inputChanges|=1u<<unsigned(InputChange::Output);
        if(!wholeIds && !packedOversized)inputChanges|=1u<<unsigned(InputChange::Oversized);
    }
    if(inputChanges){for(unsigned i=0;i<unsigned(InputChange::Count);++i)if(inputChanges&(1u<<i))++diagnostics.inputChanges[i];flushCapture(FlushReason::InputChange);}
    if(failed)return;
    if(!pending.count){
        pending.context=ctx;pending.scene=cb;pending.ids=ids;pending.pool=pool;pending.poolResource=pr;pending.output=now.uav;
        // Reuse one GPU snapshot for all draws while this stream is unchanged.
        // The established oversized path copies this draw only and flushes it
        // before the next oversized draw. Phase B alone packs those ranges.
        if(wholeIds)copyInstanceIds(ctx,0,ids.Get(),0,idd.ByteWidth,true);
        else if(!packedOversized)copyInstanceIds(ctx,0,ids.Get(),UINT(at),UINT(at+n*8),false);
    }
    const UINT capturedIds=wholeIds?UINT(at):(packedOversized?pending.count*8:0);
    // A stream too large for the owned snapshot contributes only this draw's
    // IDs. The batch is capped at 512 records, so these ranged copies occupy
    // at most 4096 bytes while retaining one source identity for write flushes.
    if(packedOversized)copyInstanceIds(ctx,capturedIds,ids.Get(),UINT(at),UINT(at+n*8),false);
    Ptr<ID3D11VertexShader> vs;Ptr<ID3D11InputLayout> layout;ctx->VSGetShader(&vs,nullptr,nullptr);ctx->IAGetInputLayout(&layout);
    Settings data{};data.info[0]=now.count;data.info[1]=prev.count;data.info[2]=n;data.info[3]=capturedIds;data.dimensions[0]=float(e.width);data.dimensions[1]=float(e.height);data.dimensions[2]=float(now.count);data.dimensions[3]=float(n);
    IUnknown* objects[4]={vs.Get(),vb.Get(),ib.Get(),layout.Get()};
    for(unsigned i=0;i<4;++i){uint64_t key=reinterpret_cast<uint64_t>(objects[i]);data.key[2*i]=UINT(key);data.key[2*i+1]=UINT(key>>32);now.sources[now.count][i]=objects[i];}
    data.key[8]=UINT(base);data.key[9]=start;data.key[10]=count;data.key[11]=vertexStride;data.key[12]=offset;data.key[13]=indexOffset;data.key[14]=UINT(format);
    auto& vertexStamp=watched[vb.Get()];vertexStamp.seen=frames;
    // Inserting the index entry can rehash the map; retain the value, not
    // an iterator. Epochs remain globally ordered for both geometry inputs.
    const unsigned vertexEpoch=vertexStamp.epoch;
    auto& indexStamp=watched[ib.Get()];indexStamp.seen=frames;
    auto& slice=indexStamp.indices.try_emplace((indexFirst<<32)|indexEnd,IndexStamp{indexStamp.epoch,frames}).first->second;
    slice.seen=frames;data.key[15]=std::max(vertexEpoch,slice.epoch);
    for(unsigned i=0;i<n;++i){auto& input=pending.inputs[pending.count++];input=data;input.info[0]=now.count+i;input.info[3]+=i*8;}
    const bool coverageSample=(++draws&63u)==0;bool timed=false,timedComparison=false;
    if(coverageSample){if(comparison.measurement){timed=comparisonCoverageGpu.begin(ctx);timedComparison=timed;if(timed)++comparison.metrics.coverageGpuSubmitted;}else timed=drawGpu.begin(ctx);}
    if(!uploadSettings(ctx,data)){if(timed){if(timedComparison)comparisonCoverageGpu.end(ctx);else drawGpu.end(ctx);}return;}
    if(!e.cleared){float zero[4]{};ctx->ClearRenderTargetView(e.rtv.Get(),zero);e.cleared=true;}
    ID3D11RenderTargetView* rt[8]{};Ptr<ID3D11DepthStencilView> originalDepth;ctx->OMGetRenderTargets(8,rt,&originalDepth);
    Ptr<ID3D11PixelShader> ps;ID3D11ClassInstance* classes[256]{};UINT nc=256;ctx->PSGetShader(&ps,classes,&nc);
    Ptr<ID3D11Buffer> psCb;Ptr<ID3D11ShaderResourceView> psSrv;ctx->PSGetConstantBuffers(13,1,&psCb);ctx->PSGetShaderResources(15,1,&psSrv);
    vScreenSetRenderTargetsRaw(ctx,1,e.rtv.GetAddressOf(),originalDepth.Get());ctx->OMSetDepthStencilState(depthState.Get(),0);ctx->OMSetBlendState(blendState.Get(),nullptr,mask);
    ctx->PSSetShader(coverageShaders[kind].Get(),nullptr,0);ctx->PSSetConstantBuffers(13,1,settings.GetAddressOf());ctx->PSSetShaderResources(15,1,instanceView.GetAddressOf());
    issue(ctx,count,n,start,base,startInstance);
    ID3D11ShaderResourceView* none=nullptr;ctx->PSSetShaderResources(15,1,&none);
    ctx->PSSetShader(ps.Get(),classes,nc);ctx->PSSetConstantBuffers(13,1,psCb.GetAddressOf());ctx->PSSetShaderResources(15,1,psSrv.GetAddressOf());
    vScreenSetRenderTargetsRaw(ctx,8,rt,originalDepth.Get());ctx->OMSetDepthStencilState(ds.Get(),ref);ctx->OMSetBlendState(blend.Get(),factors,mask);
    for(auto* p:rt)if(p)p->Release();for(UINT i=0;i<nc;++i)classes[i]->Release();
    now.count+=n;++diagnostic.acceptedDraws;diagnostic.acceptedInstances+=n;
    if(comparison.measurement){++comparison.metrics.acceptedDraws;comparison.metrics.acceptedInstances+=n;if(!wholeIds){++comparison.metrics.oversizedDraws;comparison.metrics.oversizedInstances+=n;}}
    if(timed){if(timedComparison)comparisonCoverageGpu.end(ctx);else drawGpu.end(ctx);}
    // A UAV can change through GPU commands that have no CPU write hook.
    // Keep those sources immediate; ordinary immutable/read-only inputs batch.
    if(gpuWritable)flushCapture(FlushReason::GpuWritable);
    if(!noted){noted=true;Log::get().note("mesh motion: exact rigid draw transforms at %ux%u, independent of ship-metres split; original VS coverage, 512 instances per eye, batched GPU history matching. Animated/ambiguous geometry retains existing motion.",e.width,e.height);}
}
void meshMotionViews(ID3D11DeviceContext* ctx,ID3D11Texture2D* scene,ID3D11ShaderResourceView** views){
    using namespace mesh_motion_detail;views[0]=views[1]=nullptr;if(!enabled || failed)return;
    for(auto& e:eyes)if(e.scene.Get()==scene && e.cleared){
        flushCapture(FlushReason::EyeConsumption);
        if(failed)return;
        auto& now=e.history[e.write];auto& prev=e.history[1-e.write];
        if(!e.matched){
            ComputeState saved(ctx);bool timed=matchGpu.begin(ctx);Settings data{};data.info[1]=prev.count;
            if(!uploadSettings(ctx,data)){if(timed)matchGpu.end(ctx);return;}
            ctx->CSSetConstantBuffers(3,1,settings.GetAddressOf());ctx->CSSetShaderResources(2,1,prev.srv.GetAddressOf());ctx->CSSetUnorderedAccessViews(0,1,now.uav.GetAddressOf(),nullptr);ctx->CSSetShader(match.Get(),nullptr,0);ctx->Dispatch(now.count,1,1);
            if(timed)matchGpu.end(ctx);e.matched=true;
        }
        views[0]=e.srv.Get();views[1]=now.srv.Get();return;
    }
}
void meshMotionFrameBoundary(ID3D11DeviceContext* ctx){
    using namespace mesh_motion_detail;if(!enabled)return;
    if(comparison.measurement)diagnosticWindowHadComparison=true;
    flushCapture(FlushReason::FrameBoundary);
    if(ctx){drawGpu.poll(ctx);matchGpu.poll(ctx);captureGpu.poll(ctx);if(comparisonActive()){comparisonCopyGpu.poll(ctx);comparisonCoverageGpu.poll(ctx);comparisonFlushGpu.poll(ctx);}}
    processComparisonBoundary(ctx);
    if(++frames%1800==0){const auto& d=drawGpu.totals;const auto& m=matchGpu.totals;
        Log::get().note("mesh motion GPU: %u coverage reissues/%u frames; sampled coverage %.3f us (%u samples), batched match %.3f us/eye (%u samples); no waits/readbacks.",draws,frames,d.samples?d.ms*1000/d.samples:0,d.samples,m.samples?m.ms*1000/m.samples:0,m.samples);
        const auto& c=captureGpu.totals;Log::get().note("mesh motion capture: %u instances in %u batches; %.3f us/batch (%u samples, %u skipped).",capturedInstances,captureBatches,c.samples?c.ms*1000/c.samples:0,c.samples,c.skipped);
        for(unsigned i=0;i<2;++i){const auto& x=diagnostics.eye[i];Log::get().note("mesh motion diagnostic eye %u: fully checked candidates %llu draws/%llu instances; accepted %llu/%llu; cap decisions %llu/%llu total, bounded sample %llu/%llu (%llu/%llu eligible, %llu/%llu ineligible; no extrapolation).",i,(unsigned long long)x.candidateDraws,(unsigned long long)x.candidateInstances,(unsigned long long)x.acceptedDraws,(unsigned long long)x.acceptedInstances,(unsigned long long)x.capRejectedDraws,(unsigned long long)x.capRejectedInstances,(unsigned long long)x.sampledCapDraws,(unsigned long long)x.sampledCapInstances,(unsigned long long)x.eligibleCapRejectedDraws,(unsigned long long)x.eligibleCapRejectedInstances,(unsigned long long)x.ineligibleCapSampleDraws,(unsigned long long)x.ineligibleCapSampleInstances);}
        uint64_t flushCount=0;for(auto count:diagnostics.flushes)flushCount+=count;
        Log::get().note("mesh motion diagnostic CPU: draw %.3f us (%llu samples), flush %.3f us (%llu samples); batches %llu, %.1f instances average, %u max.",diagnostics.drawCpuSamples?diagnostics.drawCpuMs*1000/diagnostics.drawCpuSamples:0,(unsigned long long)diagnostics.drawCpuSamples,diagnostics.flushCpuSamples?diagnostics.flushCpuMs*1000/diagnostics.flushCpuSamples:0,(unsigned long long)diagnostics.flushCpuSamples,(unsigned long long)flushCount,flushCount?double(diagnostics.flushInstances)/double(flushCount):0,diagnostics.largestBatch);
        Log::get().note("mesh motion diagnostic depth metadata: %llu cache hits, %llu fills; scene/eye selection remains live.",(unsigned long long)diagnostics.depthMetadataHits,(unsigned long long)diagnostics.depthMetadataFills);
        Log::get().note("mesh motion diagnostic flushes: %s %llu, %s %llu, %s %llu, %s %llu, %s %llu.",flushReasonName(FlushReason::InputChange),(unsigned long long)diagnostics.flushes[unsigned(FlushReason::InputChange)],flushReasonName(FlushReason::WriteOrMap),(unsigned long long)diagnostics.flushes[unsigned(FlushReason::WriteOrMap)],flushReasonName(FlushReason::EyeConsumption),(unsigned long long)diagnostics.flushes[unsigned(FlushReason::EyeConsumption)],flushReasonName(FlushReason::FrameBoundary),(unsigned long long)diagnostics.flushes[unsigned(FlushReason::FrameBoundary)],flushReasonName(FlushReason::GpuWritable),(unsigned long long)diagnostics.flushes[unsigned(FlushReason::GpuWritable)]);
        Log::get().note("mesh motion diagnostic input-change causes (overlapping): context %llu, scene-cb %llu, instance-ids %llu, pool %llu, output %llu, oversized-stream %llu.",(unsigned long long)diagnostics.inputChanges[unsigned(InputChange::Context)],(unsigned long long)diagnostics.inputChanges[unsigned(InputChange::Scene)],(unsigned long long)diagnostics.inputChanges[unsigned(InputChange::Ids)],(unsigned long long)diagnostics.inputChanges[unsigned(InputChange::Pool)],(unsigned long long)diagnostics.inputChanges[unsigned(InputChange::Output)],(unsigned long long)diagnostics.inputChanges[unsigned(InputChange::Oversized)]);
        Log::get().note("mesh motion diagnostic oversized streams: %llu bounded ID copies, %llu instances.",(unsigned long long)diagnostics.rangedIdCopies,(unsigned long long)diagnostics.rangedIdInstances);
        if(diagnosticWindowHadComparison)Log::get().note("mesh motion diagnostic: this rolling 1800-frame window overlaps the controlled comparison; its global query samples and count denominators are mixed. Use the phase-tagged motion comparison lines instead.");
        diagnostics.clearWindow();diagnosticWindowHadComparison=false;}
    depthMetadata.clear();
    for(auto& e:eyes){
        e.write=1-e.write;auto& h=e.history[e.write];for(unsigned i=0;i<h.count;++i)for(auto& p:h.sources[i])p.Reset();h.count=0;e.cleared=e.matched=false;
        auto& prev=e.history[1-e.write];for(unsigned i=0;i<prev.count;++i)for(unsigned j=1;j<=2;++j)if(prev.sources[i][j])watched[static_cast<ID3D11Resource*>(prev.sources[i][j].Get())].seen=frames;
    }
    for(auto it=watched.begin();it!=watched.end();){
        if(it->second.seen!=frames){it=watched.erase(it);continue;}
        auto& indices=it->second.indices;
        for(auto i=indices.begin();i!=indices.end();)if(frames-i->second.seen>1)i=indices.erase(i);else ++i;
        ++it;
    }
}
void meshMotionBeforeMap(ID3D11Resource* resource){
    using namespace mesh_motion_detail;
    if(pending.count && (!resource || resource==pending.scene.Get() || resource==pending.ids.Get() || resource==pending.poolResource.Get()))flushCapture(FlushReason::WriteOrMap);
}
void meshMotionResourceWritten(ID3D11Resource* resource,uint64_t first,uint64_t end){
    using namespace mesh_motion_detail;
    // Copy/Update hooks call before the write; Map must flush before the
    // actual Map call, since a mapped resource cannot be used by the GPU.
    meshMotionBeforeMap(resource);
    if(!resource)depthMetadata.clear();
    if(!enabled || (resource && watched.find(resource)==watched.end()))return;
    if(resource){
        if(first==end)return; // Empty D3D11 box performs no write.
        if(first>end){first=0;end=~uint64_t(0);} // Unknown extent fails closed.
        ++geometryWrites;
        if(++geometryEpoch){
            auto& stamp=watched[resource];stamp.epoch=geometryEpoch;
            if(first || end!=~uint64_t(0))++rangeWrites;
            for(auto& entry:stamp.indices){
                const uint64_t begin=entry.first>>32,finish=UINT(entry.first);
                if(first<finish && begin<end)entry.second.epoch=geometryEpoch;
                else ++disjointIndices;
            }
            return;
        }
        // Epoch wrap is an unknown generation, so discard conservatively.
    }else ++unknownWrites;
    // A known write changes future correspondence only for its dependents.
    // The old geometry was already rasterised into this eye; its coverage
    // remains correct. Unknown command-list writes still invalidate all.
    for(auto& e:eyes){for(auto& h:e.history){for(unsigned i=0;i<h.count;++i)for(auto& p:h.sources[i])p.Reset();h.count=0;}e.cleared=e.matched=false;}
    watched.clear();
}
void meshMotionShutdown(){
    using namespace mesh_motion_detail;pending.clear();depthMetadata.clear();for(auto& e:eyes)e=Eye{};capture.Reset();match.Reset();for(auto& p:coverageShaders)p.Reset();settings.Reset();instances.Reset();instanceView.Reset();captureInputs.Reset();captureInputView.Reset();depthState.Reset();blendState.Reset();
    failed=noted=capped=diagnosticWindowHadComparison=false;drawGpu={};matchGpu={};captureGpu={};comparisonCopyGpu={};comparisonCoverageGpu={};comparisonFlushGpu={};comparison={};frames=draws=captureBatches=capturedInstances=0;diagnostics={};watched.clear();geometryEpoch=geometryWrites=unknownWrites=rangeWrites=disjointIndices=0;dump.Reset();dumpPrevious.Reset();dumpCount=dumpPreviousCount=0;dumpSceneFrame=~0u;dumpMeshFrame=dumpEye=dumpWidth=dumpHeight=dumpWriteSlot=0;
}
void meshMotionStageDump(ID3D11DeviceContext* ctx,ID3D11Texture2D* scene,unsigned sceneFrame){
    using namespace mesh_motion_detail;dump.Reset();dumpPrevious.Reset();dumpCount=dumpPreviousCount=0;
    if(!enabled || failed)return;
    for(unsigned eye=0;eye<2;++eye){auto& e=eyes[eye];if(e.scene.Get()==scene && e.cleared && e.matched){
        auto& h=e.history[e.write];auto& previous=e.history[1-e.write];if(!h.count)return;
        D3D11_BUFFER_DESC b{};b.ByteWidth=maxRecords*stride;b.Usage=D3D11_USAGE_STAGING;b.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        Ptr<ID3D11Device> dev;ctx->GetDevice(&dev);
        if(SUCCEEDED(dev->CreateBuffer(&b,nullptr,&dump)) && SUCCEEDED(dev->CreateBuffer(&b,nullptr,&dumpPrevious))){
            // The pair is copied before either history slot can roll.
            ctx->CopyResource(dump.Get(),h.buffer.Get());ctx->CopyResource(dumpPrevious.Get(),previous.buffer.Get());
            dumpCount=h.count;dumpPreviousCount=previous.count;dumpSceneFrame=sceneFrame;dumpMeshFrame=frames;dumpEye=eye;dumpWidth=e.width;dumpHeight=e.height;dumpWriteSlot=e.write;
        }else {dump.Reset();dumpPrevious.Reset();}
        return;
    }}
}
void meshMotionWriteDump(ID3D11DeviceContext* ctx,const wchar_t* directory,const wchar_t* stamp){
    using namespace mesh_motion_detail;
    if(!dump || !dumpPrevious){Log::get().note("mesh motion: eye run %ls has no captured mesh records.",stamp);return;}
    wchar_t currentPath[MAX_PATH],previousPath[MAX_PATH],markerPath[MAX_PATH];
    wchar_t currentName[MAX_PATH],previousName[MAX_PATH];
    _snwprintf_s(currentName,MAX_PATH,_TRUNCATE,L"eye_%s_Mesh.bin",stamp);
    _snwprintf_s(previousName,MAX_PATH,_TRUNCATE,L"eye_%s_MeshPrev.bin",stamp);
    _snwprintf_s(currentPath,MAX_PATH,_TRUNCATE,L"%s\\%s",directory,currentName);
    _snwprintf_s(previousPath,MAX_PATH,_TRUNCATE,L"%s\\%s",directory,previousName);
    _snwprintf_s(markerPath,MAX_PATH,_TRUNCATE,L"%s\\eye_%s_MeshProbe.json",directory,stamp);
    errno=0;
    if(_wremove(markerPath)!=0 && errno!=ENOENT){
        Log::get().note("mesh motion: eye run %ls cannot retire its prior marker; paired dump unavailable.",stamp);
        dump.Reset();dumpPrevious.Reset();dumpCount=dumpPreviousCount=0;return;
    }
    unsigned valid=0,matched=0;HRESULT readback=S_OK;
    auto writeRecords=[&](ID3D11Buffer* source,unsigned count,const wchar_t* path,bool inspect){
        D3D11_MAPPED_SUBRESOURCE mapped{};readback=ctx->Map(source,0,D3D11_MAP_READ,D3D11_MAP_FLAG_DO_NOT_WAIT,&mapped);if(FAILED(readback))return false;
        if(inspect)for(unsigned i=0;i<count;++i){auto* p=reinterpret_cast<const float*>(static_cast<const char*>(mapped.pData)+i*stride);valid+=p[56]==1;matched+=p[59]==1;}
        FILE* f=nullptr;_wfopen_s(&f,path,L"wb");bool ok=false;
        if(f){const UINT header[2]={count,stride};ok=fwrite("EDVRMSH1",1,8,f)==8 && fwrite(header,sizeof(header),1,f)==1 && fwrite(mapped.pData,stride,count,f)==count;ok=fclose(f)==0 && ok;}
        ctx->Unmap(source,0);return ok;
    };
    bool ok=writeRecords(dump.Get(),dumpCount,currentPath,true) && writeRecords(dumpPrevious.Get(),dumpPreviousCount,previousPath,false);
    auto utf8=[](const wchar_t* value){int bytes=WideCharToMultiByte(CP_UTF8,0,value,-1,nullptr,0,nullptr,nullptr);std::string out(bytes>0?size_t(bytes):0,'\0');if(bytes>0){WideCharToMultiByte(CP_UTF8,0,value,-1,&out[0],bytes,nullptr,nullptr);out.pop_back();}return out;};
    auto quote=[](const std::string& value){std::string out;for(unsigned char c:value){if(c=='\\' || c=='\"'){out.push_back('\\');out.push_back(char(c));}else if(c>=0x20)out.push_back(char(c));}return out;};
    const auto currentUtf8=quote(utf8(currentName)),previousUtf8=quote(utf8(previousName));
    if(ok){FILE* marker=nullptr;_wfopen_s(&marker,markerPath,L"wb");if(marker){
        const std::string sceneFrame=dumpSceneFrame==~0u?"null":std::to_string(dumpSceneFrame);
        ok=std::fprintf(marker,"{\n  \"schema\": 1,\n  \"recordFormat\": \"EDVRMSH1\",\n  \"recordStride\": %u,\n  \"capture\": {\"frame\": %s, \"meshFrame\": %u, \"eye\": %u, \"width\": %u, \"height\": %u, \"writeSlot\": %u, \"matched\": true},\n  \"current\": {\"file\": \"%s\", \"count\": %u},\n  \"previous\": {\"file\": \"%s\", \"count\": %u}\n}\n",stride,sceneFrame.c_str(),dumpMeshFrame,dumpEye,dumpWidth,dumpHeight,dumpWriteSlot,currentUtf8.c_str(),dumpCount,previousUtf8.c_str(),dumpPreviousCount)>0;ok=fclose(marker)==0 && ok;}else ok=false;}
    if(FAILED(readback))Log::get().note("mesh motion: eye run %ls readback unavailable (0x%08X).",stamp,unsigned(readback));
    else {
        Log::get().note("mesh motion: eye run %ls matched %u/%u rigid records (%u total, %u previous); paired dump %s. Sampled coverage %.3f us, match %.3f us/eye.",stamp,matched,valid,dumpCount,dumpPreviousCount,ok?"written":"WRITE FAILED",drawGpu.totals.samples?drawGpu.totals.ms*1000/drawGpu.totals.samples:0,matchGpu.totals.samples?matchGpu.totals.ms*1000/matchGpu.totals.samples:0);
        Log::get().note("mesh motion: %u known geometry writes isolated by generation, %u unknown writes reset all history; %zu geometry resources retained.",geometryWrites,unknownWrites,watched.size());
        Log::get().note("mesh motion: %u bounded writes preserved %u disjoint index histories; vertex writes remain conservative.",rangeWrites,disjointIndices);
    }
    dump.Reset();dumpPrevious.Reset();dumpCount=dumpPreviousCount=0;
}
}
