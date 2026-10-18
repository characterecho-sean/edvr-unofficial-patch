// Production flat-only exact-light adapter; independent of every diagnostic arm.
// Existing owner-context observers only. No GPU reads, game calls or bindings.
// One Geometry proof for this owner. Cleared at every forward reset. The epoch
// header is included by flat_runtime.cpp, outside this anonymous namespace.
forward_light::GeometryReuseProof& geometryCert() {
    static forward_light::GeometryReuseProof proof;
    return proof;
}
void geometryCertClear() {
    geometryCert()={};
    flatIssueEpochRevoke();
}
void forwardRefusal(State&,forward_light::Reason,const void* =nullptr);
void forwardPhaseLifecycle(State&);
void forwardRevoke(State& s,uint64_t resource,forward_light::Reason reason) {
    for(auto& a:s.forwardLight.authority)if(a.affected(s.prefix.frame,resource)) {
        a.live=false;
        // A later relevant write must cancel even if no light draw follows.
        for(unsigned i=0;i<s.prefix.targetsUsed;++i)if(reinterpret_cast<uintptr_t>(s.prefix.targets[i].resource)==a.hdr) {
            auto& target=s.prefix.targets[i];target.hdrBad=true;target.hdrCamera=false;
        }
        forwardRefusal(s,reason,reinterpret_cast<void*>(a.hdr));
    }
}
void forwardMutation(State& s,uint64_t resource,bool forgetSources=true) {
    s.forwardLight.ledger.mutation(resource,forgetSources);
    forwardRevoke(s,resource,forward_light::Reason::MutatedProducer);
}
void forwardReset(State& s,uint64_t frame=0,bool captureLifecycle=true) {
    if(captureLifecycle)forwardPhaseLifecycle(s);
    geometryCertClear();
    auto& f=s.forwardLight;forwardRevoke(s,0,forward_light::Reason::Ownership);f.ledger.reset(frame);
    f.dsvDescription.clear();f.depthDescription.clear();
    for(auto& observed:f.phaseObservedDonor)observed={};
    for(auto& a:f.authority)a={};for(auto& h:f.authorityHdr)h.Reset();
    for(auto& h:f.domainHolds)h={};for(auto& h:f.publicationHolds)h={};
}
void forwardFrame(State& s) {
    auto& f=s.forwardLight;const bool upstream=flatCameraInjectUpstreamOwns();
    if(f.ledger.frame!=s.prefix.frame||!f.ownerKnown||f.upstream!=upstream)forwardReset(s,s.prefix.frame,f.ownerKnown&&f.upstream!=upstream);
    f.phaseReceipt.phase(s.prefix.frame,s.phase.currentX,s.phase.currentY,s.phaseWidth,s.phaseHeight);
    f.ownerKnown=true;f.upstream=upstream;
}
forward_light::Surface forwardTexture(ID3D11Resource* resource,unsigned mip,unsigned slice,unsigned slices,uint32_t* format=nullptr) {
    const auto observed=forward_light::describeTexture(reinterpret_cast<uintptr_t>(resource),mip,slice,slices,
        [&](forward_light::TextureDescription& out) {
            Ptr<ID3D11Texture2D> texture;resource->QueryInterface(IID_PPV_ARGS(&texture));
            if(!texture)return false;
            D3D11_TEXTURE2D_DESC d{};texture->GetDesc(&d);
            out={d.Width,d.Height,d.MipLevels,d.ArraySize,d.SampleDesc.Count,static_cast<uint32_t>(d.Format)};
            return true;
        });
    if(format)*format=observed.format;
    return observed.surface;
}
bool forwardSrv(State& s,unsigned slot,bool raw,forward_light::Surface& out,
                Ptr<ID3D11ShaderResourceView>& view,Ptr<ID3D11Resource>& resource) {
    s.context->PSGetShaderResources(slot,1,&view);if(!view)return false;view->GetResource(&resource);if(!resource)return false;
    D3D11_SHADER_RESOURCE_VIEW_DESC v{};view->GetDesc(&v);uint32_t format=0;
    out=forwardTexture(resource.Get(),v.Texture2D.MostDetailedMip,0,1,&format);
    return out.known&&v.ViewDimension==D3D11_SRV_DIMENSION_TEXTURE2D&&v.Texture2D.MipLevels==1&&
        (raw?(format==DXGI_FORMAT_R32G8X24_TYPELESS&&v.Format==DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS):
             (format==DXGI_FORMAT_R32_TYPELESS&&v.Format==DXGI_FORMAT_R32_FLOAT));
}
bool forwardDsvActual(State& s,const FlatContractObservation& k,forward_light::Surface& out,
                      Ptr<ID3D11DepthStencilView>& dsv,Ptr<ID3D11Resource>& resource,bool& writes) {
    // Actual mutable bindings are queried on every draw. The one-entry caches
    // retain COM identities only for descriptions that cannot change while a
    // view/state is alive; a different binding immediately takes the miss path.
    flatcpu::CopyCounter counts;
    counts.add(flatcpu::kCopyRawDsvGet);
    return forward_light::observeCurrentDsv(dsv,[&](auto& current) {
        s.context->OMGetRenderTargets(0,nullptr,&current);
    },[&](const auto&) {
    auto& cache=s.forwardLight.dsvDescription;
    ForwardDsvDescription fresh{};
    bool dsvHit=false;
    const ForwardDsvDescription* described=cache.describe(dsv,fresh,dsvHit,[&](ForwardDsvDescription& value) {
        dsv->GetResource(&value.resource);
        dsv->GetDesc(&value.view);
        value.surface=forwardTexture(value.resource.Get(),value.view.Texture2D.MipSlice,0,1,&value.format);
        return value.resource&&value.surface.known;
    });
    counts.add(dsvHit?flatcpu::kCopyRawDsvHit:flatcpu::kCopyRawDsvMiss);
    const ForwardDsvDescription& actual=*described;
    resource=actual.resource;out=actual.surface;
    const auto& v=actual.view;
    const uint32_t format=actual.format;
    Ptr<ID3D11DepthStencilState> state;UINT ref=0;
    counts.add(flatcpu::kCopyRawDepthGet);
    s.context->OMGetDepthStencilState(&state,&ref);
    D3D11_DEPTH_STENCIL_DESC z{};z.DepthEnable=TRUE;z.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ALL;
    if(state) {
        auto& depthCache=s.forwardLight.depthDescription;
        D3D11_DEPTH_STENCIL_DESC freshDepth{};
        bool depthHit=false;
        const auto* describedDepth=depthCache.describe(state,freshDepth,depthHit,[&](D3D11_DEPTH_STENCIL_DESC& value) {
            state->GetDesc(&value);return true;
        });
        counts.add(depthHit?flatcpu::kCopyRawDepthHit:flatcpu::kCopyRawDepthMiss);
        z=*describedDepth;
    } else {counts.add(flatcpu::kCopyRawDepthDefault);s.forwardLight.depthDescription.clear();}
    const forward_light::DsvObservationProof actualProof{out,reinterpret_cast<uintptr_t>(resource.Get()),
        static_cast<uint32_t>(v.ViewDimension),format,static_cast<uint32_t>(v.Format),
        z.DepthEnable!=FALSE,z.DepthWriteMask==D3D11_DEPTH_WRITE_MASK_ALL,(v.Flags&D3D11_DSV_READ_ONLY_DEPTH)!=0};
    const forward_light::DsvRequirement expected{reinterpret_cast<uintptr_t>(k.depth),k.depthWidth,k.depthHeight,
        D3D11_DSV_DIMENSION_TEXTURE2D,DXGI_FORMAT_R32G8X24_TYPELESS,DXGI_FORMAT_D32_FLOAT_S8X24_UINT};
    return forward_light::dsvObservationMatches(actualProof,expected,writes);
    });
}
bool forwardDsv(State& s,const FlatContractObservation& k,forward_light::Surface& out,
                Ptr<ID3D11DepthStencilView>& dsv,Ptr<ID3D11Resource>& resource,bool& writes) {
    // Ordinary draw observation still compares the current DSV to the draw's
    // binding identity. Issue validation has no earlier issued-view identity:
    // its former key.dsv was a redundant immediate query of the same context.
    return forwardDsvActual(s,k,out,dsv,resource,writes)&&dsv.Get()==k.dsv;
}
bool forwardViewport(State& s,unsigned width,unsigned height) {
    UINT n=D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    D3D11_VIEWPORT views[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};s.context->RSGetViewports(&n,views);
    const float v[]={views[0].TopLeftX,views[0].TopLeftY,views[0].Width,views[0].Height,views[0].MinDepth,views[0].MaxDepth};
    return n==1&&flat_mono_detail::fullViewport(n,v,width,height);
}
struct ForwardConstants {forward_light::ConstantIdentity id{};unsigned char bytes[784]{};};
forward_light::Reason forwardConstants(State& s,unsigned stage,unsigned required,ForwardConstants& out) {
    using namespace forward_light;out={};Ptr<ID3D11Buffer> cb;UINT first=0,count=0;
    if(!s.projection||!s.projectionContext)return Reason::OriginalMissing;
    if(!stage)s.projectionContext->VSGetConstantBuffers1(2,1,&cb,&first,&count);
    else s.projectionContext->PSGetConstantBuffers1(2,1,&cb,&first,&count);
    if(!cb)return Reason::Binding;D3D11_BUFFER_DESC desc{};cb->GetDesc(&desc);
    if(required>sizeof(out.bytes)||!lightaudit::range(first,count,desc.ByteWidth,0,required/16))return Reason::Range;
    const auto before=s.projection->constantsMetadata(cb.Get());
    const ConstantIdentity candidate{reinterpret_cast<uintptr_t>(cb.Get()),before.generation,before.mutationSerial,before.writeGeneration,
        before.bankEpoch,before.writeFrame,first,count,desc.ByteWidth,before.writeSequence,required,0};
    const auto ready=constantReady(candidate,before.tracked,before.shadowPresent,before.mapped,before.pending,s.prefix.frame,s.prefix.sequence+1,required);
    if(ready!=Reason::None)return ready;
    if(before.width!=desc.ByteWidth)return Reason::OriginalChanged;
    if(!s.projection->copyConstants(cb.Get(),first*16,required,out.bytes))return Reason::OriginalMissing;
    const auto after=s.projection->constantsMetadata(cb.Get());
    if(!lightaudit::same(lightStamp(before),lightStamp(after)))return Reason::OriginalChanged;
    uint32_t hash=2166136261u;for(unsigned i=0;i<required;++i)hash=(hash^out.bytes[i])*16777619u;
    out.id={reinterpret_cast<uintptr_t>(cb.Get()),before.generation,before.mutationSerial,before.writeGeneration,
        before.bankEpoch,before.writeFrame,first,count,desc.ByteWidth,before.writeSequence,required,hash};return Reason::None;
}
#include "flat_phase_failure_runtime.inl"
void forwardRefusal(State& s,forward_light::Reason why,const void* h) {
    auto& f=s.forwardLight;f.last=why;++f.refused;++f.reasons[static_cast<unsigned>(why)];
    const bool owned=flatRuntimeOwnsForwardCamera(s.prefix,h);
    if(owned){flatRuntimeMarkUncertain(s.prefix,FlatRuntimeUncertainCause::ForwardRefusal,h,nullptr,0,0,static_cast<uint32_t>(why));
        flatTraceMark(s.traceRing,kFlatTraceEventMarkUncertain,nullptr);failPhase(s,forward_light::name(why));}
}
bool forwardActualPair(State& s,uint64_t vs,uint64_t ps) {
    Ptr<ID3D11VertexShader> v;Ptr<ID3D11PixelShader> p;s.context->VSGetShader(&v,nullptr,nullptr);s.context->PSGetShader(&p,nullptr,nullptr);
    Ptr<ID3D11GeometryShader> gs;Ptr<ID3D11HullShader> hs;Ptr<ID3D11DomainShader> ds;
    s.context->GSGetShader(&gs,nullptr,nullptr);s.context->HSGetShader(&hs,nullptr,nullptr);s.context->DSGetShader(&ds,nullptr,nullptr);
    Ptr<ID3D11Predicate> predicate;BOOL value=FALSE;s.context->GetPredication(&predicate,&value);
    return !gs&&!hs&&!ds&&!predicate&&lookupShaderHash(v.Get())==vs&&lookupShaderHash(p.Get())==ps;
}
bool forwardObserveFromCert(State& s,const FlatRuntimeDraw& d,forward_light::Issue& issue) {
    using namespace forward_light;auto& f=s.forwardLight;const auto& k=d.key;
    const Role role=d.supported?Role::Origin:typedRole(k.vs,k.ps);
    if(role==Role::Unknown)return false;
    const auto& cert=geometryCert();
    if(!geometryObserveReuse(cert,flatIssueEpochLoad(),s.prefix.frame,k.vs,k.ps,reinterpret_cast<uintptr_t>(k.depth),
        reinterpret_cast<uintptr_t>(k.dsv),reinterpret_cast<uintptr_t>(k.b1),s.phase.currentX,s.phase.currentY,d.camera,flatIssueEpochFrozen()))
        return false;
    const Surface raw=cert.raw;const bool writes=cert.depthWrite;
    if(writes)forwardMutation(s,raw.resource,false);
    const bool current=flatRuntimeCameraCurrent(d,s.prefix.sequence+1,s.prefix.frame)&&cert.constants.writeQ<k.writeSeq;
    Source source{cert.constants.buffer,cert.constants.generation,cert.constants.bank,cert.constants.writeGeneration,
        k.writeEpoch,k.writeSeq,cert.constants.first,cert.constants.count};
    f.ledger.observe(raw,role,writes,current,source,current?d.camera:nullptr,s.prefix.sequence+1);
    if(auto* domain=f.ledger.domain(raw)){const auto index=static_cast<unsigned>(domain-f.ledger.domains);
        if(current){issue.kind=Issue::Geometry;issue.frame=s.prefix.frame;issue.q=s.prefix.sequence+1;
            issue.raw=raw;issue.vs=k.vs;issue.ps=k.ps;issue.depthWrite=writes;
            issue.phaseX=s.phase.currentX;issue.phaseY=s.phase.currentY;std::memcpy(issue.camera,d.camera,96);
            issue.constants[0]=cert.constants;
            if(!f.phaseReceipt.frozen)f.phaseObservedDonor[index]=issue.constants[0];}
    }
    if(auto* domain=f.ledger.domain(raw))if(domain->failure!=Reason::None)forwardRevoke(s,raw.resource,domain->failure);
    if(f.ledger.overflow)forwardRevoke(s,0,Reason::Overflow);
    return true;
}
forward_light::Issue forwardObserveDomain(State& s,const FlatRuntimeDraw& d) {
    using namespace forward_light;Issue issue;auto& f=s.forwardLight;const auto& k=d.key;
    if(!k.depth||flatContractKind(false,k.depth,k.depth,k.depthWidth,k.depthHeight,26,s.prefix.width,s.prefix.height,false)!=kFlatContractScreen)return issue;
    flatcpu::Scope domainCost(flatcpu::kCopyDomain);
    // Pool and supported producers skip the shader Gets. Reuse their depth and
    // camera observation while the proof's epoch still names this binding.
    if((d.supported||d.poolFamilyVs)&&forwardObserveFromCert(s,d,issue)){flatIssueEpochCountObserveReuse();return issue;}
    flatIssueEpochCountObserveFresh();
    FlatComputeInternalScope internal;Surface raw;Ptr<ID3D11DepthStencilView> dsv;Ptr<ID3D11Resource> resource;bool writes=false;
    if(!forwardDsv(s,k,raw,dsv,resource,writes)){f.ledger.overflow=true;forwardRevoke(s,0,Reason::Lineage);return issue;}
    if(writes)forwardMutation(s,raw.resource,false); // depth draw invalidates old conversion, not coherent earlier geometry
    Role role=d.supported?Role::Origin:typedRole(k.vs,k.ps);
    if(role==Role::Unknown){f.ledger.observe(raw,role,writes,false,{},nullptr,s.prefix.sequence+1);if(writes)forwardRevoke(s,raw.resource,Reason::UnknownWriter);return issue;}
    if(!d.supported&&!d.poolFamilyVs&&!forwardActualPair(s,k.vs,k.ps)){f.ledger.observe(raw,Role::Unknown,true,false,{},nullptr,s.prefix.sequence+1);forwardRevoke(s,raw.resource,Reason::UnknownWriter);return issue;}
    lightaudit::Cb cb;lightBoundCb(s,s.context.Get(),0,1,270,96,cb);
    const bool current=flatRuntimeCameraCurrent(d,s.prefix.sequence+1,s.prefix.frame)&&cb.buffer==reinterpret_cast<uintptr_t>(k.b1)&&
        cb.status==lightaudit::Full&&cb.fresh==lightaudit::Current&&cb.before.writeQ<k.writeSeq&&
        !std::memcmp(cb.payload,d.camera,96);
    Source source{cb.buffer,cb.before.generation,cb.before.bank,cb.before.writeGeneration,k.writeEpoch,k.writeSeq,cb.first,cb.count};
    f.ledger.observe(raw,role,writes,current,source,current?d.camera:nullptr,s.prefix.sequence+1);
    if(auto* domain=f.ledger.domain(raw)){const auto index=static_cast<unsigned>(domain-f.ledger.domains);
        f.domainHolds[index].raw=resource;f.domainHolds[index].view=dsv;
        if(k.b1)f.domainHolds[index].buffer=static_cast<ID3D11Buffer*>(const_cast<void*>(k.b1));
        if(current){issue.kind=Issue::Geometry;issue.frame=s.prefix.frame;issue.q=s.prefix.sequence+1;
            issue.raw=raw;issue.vs=k.vs;issue.ps=k.ps;issue.depthWrite=writes;
            issue.phaseX=s.phase.currentX;issue.phaseY=s.phase.currentY;std::memcpy(issue.camera,d.camera,96);
            issue.constants[0]={cb.buffer,cb.before.generation,cb.before.mutation,cb.before.writeGeneration,cb.before.bank,
                cb.before.writeFrame,cb.first,cb.count,cb.width,cb.before.writeQ,96,static_cast<uint32_t>(flatCameraHash(cb.payload))};
            if(!f.phaseReceipt.frozen)f.phaseObservedDonor[index]=issue.constants[0];}
    }
    if(auto* domain=f.ledger.domain(raw))if(domain->failure!=Reason::None)forwardRevoke(s,raw.resource,domain->failure);
    if(f.ledger.overflow)forwardRevoke(s,0,Reason::Overflow);
    return issue;
}
// Every actual bound target can mutate a retained raw/linear identity, not
// just RT0. The existing dispatch and explicit-write seams cover other writes.
void forwardDrawWrites(State& s,ID3D11DeviceContext* actualDrawContext) {
    flatcpu::CopyCounter counts;counts.add(flatcpu::kCopyWatch);
    auto& ledger=s.forwardLight.ledger;bool anyLivePublication=false;
    for(const auto& p:ledger.publications)anyLivePublication|=p.live;
    const bool watching=ledger.used!=0||anyLivePublication;
    if(!watching){counts.add(flatcpu::kCopyWatchSkip);return;}
    flatcpu::Scope outputCost(flatcpu::kCopyOutputs);
    // Re-evaluate actual demand on every watched draw, even with CPU timing
    // off. Unknown state always takes the old full scan. Current Issue kinds
    // cannot resurrect failed domains/dead publications: represented live
    // dependencies already force scanning (see outputWatch and its tests).
    const bool known=owner()&&actualDrawContext&&actualDrawContext==s.context.Get()&&
        s.forwardLight.ownerKnown&&s.forwardLight.upstream&&s.device&&s.context&&
        ledger.frame==s.prefix.frame&&!s.prefix.uncertain&&!foreignWork.load(std::memory_order_acquire);
    const auto decision=forward_light::outputWatch(ledger,s.forwardLight.authority,s.prefix.frame,anyLivePublication,known,false);
    counts.add(flatcpu::kCopyGuardChecks);counts.add(flatcpu::kCopyShadowChecks);
    if(decision.reason==forward_light::OutputWatchReason::UnknownState)counts.add(flatcpu::kCopyShadowUnknown);
    if(decision.reason==forward_light::OutputWatchReason::NoUsableState) {
        counts.add(flatcpu::kCopyInactiveCandidate);counts.add(flatcpu::kCopyGuardSkipped);
        return; // No forward dependency can observe these mutations; no D3D query.
    }
    counts.add(flatcpu::kCopyGuardFallback);
    FlatComputeInternalScope internal;
    ID3D11RenderTargetView* targets[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
    ID3D11UnorderedAccessView* uavs[D3D11_1_UAV_SLOT_COUNT]{};
    const UINT n=s.device&&s.device->GetFeatureLevel()>=D3D_FEATURE_LEVEL_11_1?D3D11_1_UAV_SLOT_COUNT:D3D11_PS_CS_UAV_REGISTER_COUNT;
    counts.add(flatcpu::kCopyOmGet);counts.add(flatcpu::kCopyRtvSlots,D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT);counts.add(flatcpu::kCopyUavSlots,n);
    s.context->OMGetRenderTargetsAndUnorderedAccessViews(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT,targets,nullptr,0,n,uavs);
    counts.add(flatcpu::kCopyBlendGet);
    Ptr<ID3D11BlendState> blend;FLOAT factors[4]{};UINT mask=0;s.context->OMGetBlendState(&blend,factors,&mask);
    D3D11_BLEND_DESC desc{};if(blend){counts.add(flatcpu::kCopyBlendDesc);blend->GetDesc(&desc);}
    for(unsigned i=0;i<D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT;++i)if(targets[i]) {
        counts.add(flatcpu::kCopyRtvBound);
        Ptr<ID3D11RenderTargetView> view;view.Attach(targets[i]);
        const auto slot=desc.IndependentBlendEnable?i:0;
        if(!blend||desc.RenderTarget[slot].RenderTargetWriteMask){counts.add(flatcpu::kCopyGetResource);Ptr<ID3D11Resource> resource;view->GetResource(&resource);
            forwardMutation(s,reinterpret_cast<uintptr_t>(resource.Get()),false);}
    }
    for(unsigned i=0;i<n;++i)if(uavs[i]){counts.add(flatcpu::kCopyUavBound);Ptr<ID3D11UnorderedAccessView> view;view.Attach(uavs[i]);
        counts.add(flatcpu::kCopyGetResource);Ptr<ID3D11Resource> resource;view->GetResource(&resource);forwardMutation(s,reinterpret_cast<uintptr_t>(resource.Get()));}
}
bool forwardConverterState(State& s,unsigned width,unsigned height) {
    forward_light::ConverterRaster raster;raster.width=width;raster.height=height;
    D3D11_PRIMITIVE_TOPOLOGY topology=D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;s.context->IAGetPrimitiveTopology(&topology);raster.topology=topology;
    Ptr<ID3D11RasterizerState> state;s.context->RSGetState(&state);D3D11_RASTERIZER_DESC rs{};
    rs.FillMode=D3D11_FILL_SOLID;rs.CullMode=D3D11_CULL_BACK;rs.DepthClipEnable=TRUE;if(state)state->GetDesc(&rs);
    raster.fill=rs.FillMode;raster.cull=rs.CullMode;raster.bias=rs.DepthBias;raster.clamp=rs.DepthBiasClamp;raster.slope=rs.SlopeScaledDepthBias;
    raster.clip=rs.DepthClipEnable!=FALSE;raster.scissor=rs.ScissorEnable!=FALSE;raster.multisample=rs.MultisampleEnable!=FALSE;raster.antialiasedLine=rs.AntialiasedLineEnable!=FALSE;
    if(raster.scissor){UINT n=D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;D3D11_RECT rects[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
        s.context->RSGetScissorRects(&n,rects);raster.scissors=n;raster.left=rects[0].left;raster.top=rects[0].top;raster.right=rects[0].right;raster.bottom=rects[0].bottom;}
    Ptr<ID3D11DepthStencilState> depth;UINT ref=0;s.context->OMGetDepthStencilState(&depth,&ref);D3D11_DEPTH_STENCIL_DESC ds{};
    ds.DepthEnable=TRUE;if(depth)depth->GetDesc(&ds);raster.depth=ds.DepthEnable!=FALSE;raster.stencil=ds.StencilEnable!=FALSE;
    if(!forward_light::converterRaster(raster))return false;
    Ptr<ID3D11BlendState> blend;FLOAT factors[4]{};UINT mask=0;s.context->OMGetBlendState(&blend,factors,&mask);
    if(mask!=~0u)return false;
    if(!blend)return true;
    D3D11_BLEND_DESC desc{};blend->GetDesc(&desc);const auto& r=desc.RenderTarget[0];
    return (r.RenderTargetWriteMask&D3D11_COLOR_WRITE_ENABLE_RED)&&(!r.BlendEnable||
        (r.SrcBlend==D3D11_BLEND_ONE&&r.DestBlend==D3D11_BLEND_ZERO&&r.BlendOp==D3D11_BLEND_OP_ADD));
}
// Publish bound raw-input/linear-output association only after CPU Draw return.
// This does not certify pixel coverage or metric equivalence; resolve uses raw DSV.
forward_light::Issue forwardBefore(State& s,ID3D11DeviceContext* actualDrawContext,FlatRuntimeDraw& d,char kind,unsigned count,unsigned start,unsigned instances,unsigned startInstance) {
    using namespace forward_light;forwardFrame(s);Issue issue;auto& f=s.forwardLight;const auto& k=d.key;
    if(!f.upstream)return issue;
    forwardDrawWrites(s,actualDrawContext);
    if(k.color)forwardMutation(s,reinterpret_cast<uintptr_t>(k.color),false);
    issue=forwardObserveDomain(s,d);
    if(k.vs!=kDepthVs||k.ps!=kDepthPs)return issue;
    flatcpu::Scope converterCost(flatcpu::kCopyConverter);
    issue={};++f.converters;FlatComputeInternalScope internal;
    if(kind!='N'||count!=3||start||instances!=1||startInstance||k.depth||!forwardConverterState(s,k.width,k.height)||!forwardActualPair(s,kDepthVs,kDepthPs)||
       !forwardViewport(s,k.width,k.height)){forwardRefusal(s,Reason::Binding);return issue;}
    Ptr<ID3D11ShaderResourceView> input;Ptr<ID3D11Resource> rawResource;Surface raw;
    Ptr<ID3D11RenderTargetView> target;Ptr<ID3D11DepthStencilView> depth;Ptr<ID3D11Resource> linearResource;
    s.context->OMGetRenderTargets(1,&target,&depth);if(target)target->GetResource(&linearResource);
    D3D11_RENDER_TARGET_VIEW_DESC view{};if(target)target->GetDesc(&view);
    Ptr<ID3D11Texture2D> texture;if(linearResource)linearResource.As(&texture);D3D11_TEXTURE2D_DESC desc{};if(texture)texture->GetDesc(&desc);
    const auto linear=forwardTexture(linearResource.Get(),view.Texture2D.MipSlice,0,1);
    if(!forwardSrv(s,0,true,raw,input,rawResource)||!linear.known||depth||linearResource.Get()!=k.color||target.Get()!=k.rtv||
       view.ViewDimension!=D3D11_RTV_DIMENSION_TEXTURE2D||view.Format!=DXGI_FORMAT_R32_FLOAT||desc.Format!=DXGI_FORMAT_R32_TYPELESS||
       raw.width!=linear.width||raw.height!=linear.height){forwardRefusal(s,Reason::Lineage);return issue;}
    const auto token=f.ledger.publish(raw,linear,s.prefix.sequence+1);if(!token){forwardRefusal(s,Reason::Overflow);return issue;}
    for(unsigned i=0;i<kPublications;++i)if(f.ledger.publications[i].token==token){auto& h=f.publicationHolds[i];h.raw=rawResource;h.linear=linearResource;h.input=input;h.target=target;}
    issue.kind=Issue::Converter;issue.token=token;issue.frame=s.prefix.frame;issue.q=s.prefix.sequence+1;
    issue.raw=raw;issue.linear=linear;issue.vs=kDepthVs;issue.ps=kDepthPs;return issue;
}
unsigned forwardConsumer(uint64_t vs,uint64_t ps) {
    if(vs==forward_light::kVs&&ps==forward_light::kPs)return 1;
    if(vs==0x963B52C73B4143ACull&&ps==0x50364C9D994141D5ull)return 2;
    if(vs==0x24DE25E496342EB8ull&&ps==0x1A53D2791C12CE92ull)return 3;
    return 0;
}
forward_light::Issue forwardQualify(State& s,FlatRuntimeDraw& d) {
    using namespace forward_light;Issue issue;auto& f=s.forwardLight;auto& k=d.key;const auto consumer=forwardConsumer(k.vs,k.ps);
    if(!consumer||k.format!=26)return issue;++f.checks;
    if(consumer==1)f.phaseReceipt.note(flatCameraInjectUpstreamOwns());
    bool first=true,typed=false;for(unsigned i=0;i<s.prefix.targetsUsed;++i)if(s.prefix.targets[i].resource==k.color){first=!s.prefix.targets[i].writes.draws;typed=s.prefix.targets[i].tone.forwardLightCamera;}
    if((!first&&!typed)||(consumer!=1&&!typed))return issue; // old inverse/ordinary H untouched; later recipes cannot establish camera
    flatcpu::Scope forwardCost(flatcpu::kCopyForward);
    const auto refuse=[&](Reason r){forwardRefusal(s,r,k.color);return Issue{};};
    if(!phaseReceipt(flatCameraInjectUpstreamOwns(),s.phase.currentX,s.phase.currentY,s.phase.applied))return refuse(Reason::Ownership);
    if(s.prefix.uncertain||foreignWork.load(std::memory_order_acquire))return refuse(Reason::Uncertain);
    if(!flatDeferredVerifyInPhase(nonzeroPhase(s),flatDeferredPhaseDomain(k.color,k.depth,k.width,k.height,
        s.phaseHdr.Get(),s.phaseDepth.Get(),s.namedDepth,s.phaseWidth,s.phaseHeight)))return refuse(Reason::Lineage);
    FlatComputeInternalScope internal;
    if(!forwardActualPair(s,k.vs,k.ps)||!forwardViewport(s,k.width,k.height))return refuse(Reason::Binding);
    Surface raw,linear;Ptr<ID3D11DepthStencilView> dsv;Ptr<ID3D11Resource> rawResource;bool writes=false;
    if(!forwardDsv(s,k,raw,dsv,rawResource,writes)||writes)return refuse(Reason::Lineage);
    Ptr<ID3D11RenderTargetView> rtv;Ptr<ID3D11Resource> color;s.context->OMGetRenderTargets(1,&rtv,nullptr);if(rtv)rtv->GetResource(&color);
    Ptr<ID3D11Texture2D> texture;if(color)color.As(&texture);D3D11_TEXTURE2D_DESC desc{};if(texture)texture->GetDesc(&desc);
    D3D11_RENDER_TARGET_VIEW_DESC cv{};if(rtv)rtv->GetDesc(&cv);
    if(color.Get()!=k.color||rtv.Get()!=k.rtv||!texture||desc.Format!=DXGI_FORMAT_R11G11B10_FLOAT||desc.Width!=raw.width||desc.Height!=raw.height||
       desc.ArraySize!=1||desc.MipLevels!=1||desc.SampleDesc.Count!=1||cv.ViewDimension!=D3D11_RTV_DIMENSION_TEXTURE2D||cv.Texture2D.MipSlice!=0)return refuse(Reason::Binding);
    Ptr<ID3D11ShaderResourceView> input;Ptr<ID3D11Resource> linearResource;
    const unsigned inputSlot=consumer==3?1:0;if(!forwardSrv(s,inputSlot,false,linear,input,linearResource))return refuse(Reason::Lineage);
    Reason reason=Reason::None;const auto* publication=f.ledger.find(raw,linear,s.prefix.sequence+1,reason);if(!publication)return refuse(reason);
    auto* domain=f.ledger.domain(raw);if(!domain||f.ledger.overflow)return refuse(Reason::Overflow);
    if(domain->failure!=Reason::None)return refuse(domain->failure);
    if(!domain->origins)return refuse(Reason::MissingDonor);
    const FlatContractRecord* donor=nullptr;
    // Same-depth sources remain separately typed in the reducer, which repeats this test.
    FlatContractObservation key=k;key.vs=kVs;key.ps=kPs;
    donor=flatRuntimeForwardDonor(s.prefix,key,s.prefix.sequence+1);
    if(!donor||reinterpret_cast<uintptr_t>(donor->key.b1)!=domain->source.buffer||std::memcmp(donor->camera,domain->camera,96))return refuse(Reason::Ambiguous);
    ForwardConstants constants[2];
    for(unsigned stage=0;stage<2;++stage){reason=forwardConstants(s,stage,stage&&consumer==3?784:752,constants[stage]);if(reason!=Reason::None)return refuse(reason);}
    // Stage resources may differ. Only the consumed camera fields must agree;
    // each stage keeps its own full declared-span and write identity.
    if(std::memcmp(constants[0].bytes+2*16,constants[1].bytes+2*16,3*16)||
       std::memcmp(constants[0].bytes+10*16,constants[1].bytes+10*16,4*16))return refuse(Reason::OriginalChanged);
    FlatProjectionJitter phase;if(!flatProjectionJitter(s.phase.currentX,s.phase.currentY,k.width,k.height,phase))return refuse(Reason::Phase);
    reason=match(domain->camera,constants[0].bytes,phase);
    if(consumer==1&&first&&!f.phaseReceipt.frozen&&(reason==Reason::None||reason==Reason::Phase))
        forwardPhaseObserve(s,d,*domain,*publication,constants,phase,reason);
    if(reason!=Reason::None)return refuse(reason);
    issue.kind=Issue::Light;issue.token=publication->token;issue.frame=s.prefix.frame;issue.color=reinterpret_cast<uintptr_t>(k.color);
    issue.q=s.prefix.sequence+1;issue.raw=raw;issue.linear=linear;issue.linearSlot=inputSlot;issue.vs=k.vs;issue.ps=k.ps;
    issue.phaseX=s.phase.currentX;issue.phaseY=s.phase.currentY;
    for(unsigned i=0;i<2;++i)issue.constants[i]=constants[i].id;
    if(consumer==1&&first){d.forwardHdrVerified=d.forwardUpstreamPhase=true;std::memcpy(d.camera,donor->camera,96);k.cameraHash=donor->key.cameraHash;
        auto& projection=k.projection[1];projection.resource=reinterpret_cast<void*>(constants[0].id.buffer);projection.width=constants[0].id.width;
        projection.copied=752;projection.hash=constants[0].id.hash;projection.writeEpoch=constants[0].id.writeFrame;projection.writeSeq=constants[0].id.writeQ;
        projection.status=FlatProjectionStatus::Available;issue.donated=true;++f.qualified;}
    return issue;
}
void forwardAttest(State& s,const forward_light::Issue& issue) {
    bool admitted=false;for(unsigned i=0;i<s.prefix.targetsUsed;++i)if(reinterpret_cast<uintptr_t>(s.prefix.targets[i].resource)==issue.color)
        admitted=s.prefix.targets[i].tone.forwardLightCamera&&s.prefix.targets[i].hdrCamera&&!s.prefix.targets[i].hdrBad;
    if(!admitted)return;
    for(unsigned i=0;i<forward_light::kPublications;++i){auto& a=s.forwardLight.authority[i];
        if(!a.live||a.hdr==issue.color){a={issue.frame,issue.color,issue.raw.resource,issue.linear.resource,issue.token,true};
            s.forwardLight.authorityHdr[i]=reinterpret_cast<ID3D11Resource*>(issue.color);return;}}
    s.forwardLight.ledger.overflow=true;forwardRefusal(s,forward_light::Reason::Overflow,reinterpret_cast<void*>(issue.color));
}
// Actual pre-Draw state, after any private mechanisms. Upstream-only means
// no new private binding; any changed owner/phase/buffer/lineage cancels authority.
bool forwardIssueValid(State& s,const forward_light::Issue& issue) {
    using namespace forward_light;if(issue.frame!=s.prefix.frame||!owner()||s.prefix.uncertain||foreignWork.load(std::memory_order_acquire)||
       !phaseReceipt(flatCameraInjectUpstreamOwns(),s.phase.currentX,s.phase.currentY,s.phase.applied))return false;
    if(issue.kind==Issue::Geometry) {
        if(geometryIssueReuse(geometryCert(),issue,flatIssueEpochLoad(),s.prefix.frame,s.phase.currentX,s.phase.currentY,flatIssueEpochFrozen())) {
            flatIssueEpochCountReuse();
            return true;
        }
        const uint64_t epoch=flatIssueEpochLoad();
        {
            flatcpu::Scope shaderCost(flatcpu::kIssueShader);
            if(!engineVelocityFlatSourceShaders(s.context.Get(),issue.vs,issue.ps)){flatIssueEpochCountFresh();return false;}
        }
        Ptr<ID3D11DepthStencilView> dsv;Ptr<ID3D11Resource> depth;Surface observed;bool writes=false;
        FlatContractObservation key;key.depth=reinterpret_cast<void*>(issue.raw.resource);
        key.depthWidth=issue.raw.width;key.depthHeight=issue.raw.height;
        {
            flatcpu::Scope depthCost(flatcpu::kIssueDsv);
            flatcpu::CopyCounter counts;counts.add(flatcpu::kCopyIssueGeometryDsvGet);
            if(!forwardDsvActual(s,key,observed,dsv,depth,writes)||writes!=issue.depthWrite||!plane(observed,issue.raw)||
               issue.phaseX!=s.phase.currentX||issue.phaseY!=s.phase.currentY){flatIssueEpochCountFresh();return false;}
        }
        bool proven=false;uintptr_t provenDsv=0;
        {
            flatcpu::Scope constantsCost(flatcpu::kIssueConstants);
            lightaudit::Cb cb;lightBoundCb(s,s.context.Get(),0,1,270,96,cb,0,true);
            const ConstantIdentity id{cb.buffer,cb.before.generation,cb.before.mutation,cb.before.writeGeneration,cb.before.bank,
                cb.before.writeFrame,cb.first,cb.count,cb.width,cb.before.writeQ,96,static_cast<uint32_t>(flatCameraHash(cb.payload))};
            proven=cb.status==lightaudit::Full&&cb.fresh==lightaudit::Current&&same(id,issue.constants[0])&&!std::memcmp(cb.payload,issue.camera,96);
            if(proven)provenDsv=reinterpret_cast<uintptr_t>(dsv.Get());
        }
        flatIssueEpochCountFresh();
        // A setter during the reads leaves the epoch moved. Do not publish that torn result.
        if(proven&&!flatIssueEpochFrozen()&&flatIssueEpochLoad()==epoch) {
            geometryCertStamp(geometryCert(),issue,epoch,provenDsv);
            flatIssueEpochPublishBuffer(issue.constants[0].buffer);
        }
        return proven;
    }else if(!forwardActualPair(s,issue.vs,issue.ps))return false;
    Ptr<ID3D11ShaderResourceView> input;Ptr<ID3D11Resource> resource;Surface observed;
    if(!forwardSrv(s,issue.kind==Issue::Converter?0:issue.linearSlot,issue.kind==Issue::Converter,observed,input,resource)||
       !plane(observed,issue.kind==Issue::Converter?issue.raw:issue.linear))return false;
    if(issue.kind==Issue::Converter){Ptr<ID3D11RenderTargetView> target;Ptr<ID3D11Resource> output;Ptr<ID3D11DepthStencilView> dsv;
        s.context->OMGetRenderTargets(1,&target,&dsv);if(target)target->GetResource(&output);
        D3D11_RENDER_TARGET_VIEW_DESC v{};if(target)target->GetDesc(&v);
        return !dsv&&target&&forwardConverterState(s,issue.linear.width,issue.linear.height)&&v.ViewDimension==D3D11_RTV_DIMENSION_TEXTURE2D&&v.Format==DXGI_FORMAT_R32_FLOAT&&
            plane(forwardTexture(output.Get(),v.Texture2D.MipSlice,0,1),issue.linear)&&forwardViewport(s,issue.linear.width,issue.linear.height);}
    if(issue.phaseX!=s.phase.currentX||issue.phaseY!=s.phase.currentY)return false;
    Reason why;const auto* publication=s.forwardLight.ledger.find(issue.raw,issue.linear,issue.q,why);
    if(!publication||publication->token!=issue.token)return false;
    Ptr<ID3D11DepthStencilView> dsv;Ptr<ID3D11Resource> depth;FlatContractObservation key;
    key.depth=reinterpret_cast<void*>(issue.raw.resource);key.depthWidth=issue.raw.width;key.depthHeight=issue.raw.height;
    // Re-read actual DSV and its subresource once for this post- or pre-Draw
    // validation. No binding is reused across the game's actual draw.
    bool writes=false;flatcpu::CopyCounter counts;counts.add(flatcpu::kCopyIssueLightDsvGet);
    if(!forwardDsvActual(s,key,observed,dsv,depth,writes)||writes||!plane(observed,issue.raw))return false;
    Ptr<ID3D11RenderTargetView> rtv;Ptr<ID3D11Resource> color;s.context->OMGetRenderTargets(1,&rtv,nullptr);if(rtv)rtv->GetResource(&color);
    if(reinterpret_cast<uintptr_t>(color.Get())!=issue.color||!forwardViewport(s,issue.raw.width,issue.raw.height))return false;
    for(unsigned stage=0;stage<2;++stage){ForwardConstants cb;if(forwardConstants(s,stage,issue.constants[stage].bytes,cb)!=Reason::None||!same(cb.id,issue.constants[stage]))return false;}
    return true;
}
void forwardBeginIssue(State& s,forward_light::Issue& issue,bool indirect) {
    using namespace forward_light;if(issue.kind==Issue::None)return;FlatComputeInternalScope internal;
    if(indirect||!forwardIssueValid(s,issue)){if(issue.kind==Issue::Converter)s.forwardLight.ledger.abandon(issue.token);else if(issue.kind==Issue::Geometry){forwardMutation(s,issue.raw.resource);forwardRefusal(s,Reason::Issue);}
        else forwardRefusal(s,Reason::Issue,reinterpret_cast<void*>(issue.color));return;}
    issue.issued=true;if(issue.donated)s.forwardLight.phaseReceipt.issue(issue.frame,issue.q);if(issue.kind==Issue::Converter)s.forwardLight.ledger.issued(issue.token);
}
void forwardReturn(State& s,forward_light::Issue& issue) {
    using namespace forward_light;if(issue.kind==Issue::None)return;
    FlatComputeInternalScope internal;
    if(issue.issued&&forwardIssueValid(s,issue)) {
        issue.returned=true;if(issue.kind==Issue::Converter)s.forwardLight.ledger.returned(issue.token);
        else if(issue.donated){s.forwardLight.phaseReceipt.returned(issue.frame,issue.q);++s.forwardLight.accepted;s.forwardLight.last=Reason::None;}
    }else if(issue.kind==Issue::Converter)s.forwardLight.ledger.abandon(issue.token);
    else if(issue.kind==Issue::Geometry){forwardMutation(s,issue.raw.resource);forwardRefusal(s,Reason::Issue);}
    else forwardRefusal(s,Reason::Issue,reinterpret_cast<void*>(issue.color));
}
void forwardAbandon(State& s,const forward_light::Issue& issue) {
    using namespace forward_light;if(issue.kind==Issue::None||issue.returned)return;
    if(issue.donated)s.forwardLight.phaseReceipt.abandon(issue.frame,issue.q);
    if(issue.kind==Issue::Converter)s.forwardLight.ledger.abandon(issue.token);
    else if(issue.kind==Issue::Geometry){forwardMutation(s,issue.raw.resource);forwardRefusal(s,Reason::Abandoned);}
    else forwardRefusal(s,Reason::Abandoned,reinterpret_cast<void*>(issue.color));
}
void forwardReport(State& s) {
    auto& f=s.forwardLight;const uint64_t now=GetTickCount64();if(now-f.lastLog<5000&&!f.phaseReceipt.frozen)return;
    if(now-f.lastLog<5000&&f.phaseReceipt.emitted)return;f.lastLog=now;
    forwardPhaseReport(s);
    Log::get().note("flat forward light camera: frame=%llu converters=%llu checks=%llu qualified=%llu CPU-issued-donations=%llu refused=%llu last=%s upstream=%u phase=(%.9g,%.9g) applied=%llu; production exact-pair guard, raw DSV reconstruction, probe flag independent",
        (unsigned long long)s.prefix.frame,(unsigned long long)f.converters,(unsigned long long)f.checks,(unsigned long long)f.qualified,
        (unsigned long long)f.accepted,(unsigned long long)f.refused,forward_light::name(f.last),flatCameraInjectUpstreamOwns()?1u:0u,
        s.phase.currentX,s.phase.currentY,(unsigned long long)s.phase.applied);
    const auto count=[&](forward_light::Reason r){return static_cast<unsigned long long>(f.reasons[static_cast<unsigned>(r)]);};
    using R=forward_light::Reason;
    Log::get().note("flat forward light refusals: ownership=%llu overflow=%llu unknown-depth=%llu stale-donor=%llu ambiguous=%llu producer-missing=%llu producer-pending=%llu producer-mutated=%llu lineage=%llu binding=%llu range=%llu original-missing=%llu original-stale=%llu original-changed=%llu forward=%llu origin=%llu view=%llu phase=%llu issue=%llu abandoned=%llu; cumulative guard reasons, no admission bypass",
        count(R::Ownership),count(R::Overflow),count(R::UnknownWriter),count(R::StaleDonor),count(R::Ambiguous),count(R::MissingProducer),count(R::PendingProducer),
        count(R::MutatedProducer),count(R::Lineage),count(R::Binding),count(R::Range),count(R::OriginalMissing),count(R::OriginalStale),count(R::OriginalChanged),
        count(R::Forward),count(R::Origin),count(R::View),count(R::Phase),count(R::Issue),count(R::Abandoned));
    static uint64_t reuseFrame=0;
    const bool haveBaseline=reuseFrame!=0;
    const uint64_t frames=haveBaseline&&s.prefix.frame>reuseFrame?s.prefix.frame-reuseFrame:0;
    reuseFrame=s.prefix.frame;
    const FlatIssueEpochWindow reuse=flatIssueEpochTakeWindow();
    if(!haveBaseline)return; // the first report would divide by the absolute frame number
    const auto per=[&](uint64_t n){return frames?static_cast<double>(n)/static_cast<double>(frames):0.0;};
    Log::get().note("flat geometry issue reuse: reused=%.0f/frame fresh=%.0f/frame observe-reused=%.0f/frame observe-fresh=%.0f/frame depth-sets=%.0f/frame stage-sets=%.0f/frame rt-sets=%.0f/frame cb1-sets=%.0f/frame cb-writes=%.0f/frame revokes=%.0f/frame frames=%llu frozen=%u; reused skipped the shader/DSV/CB queries, fresh ran them, zero reused with frozen=0 means the certificate never matched, revokes count epoch clears such as the frame reset",
        per(reuse.reuse),per(reuse.fresh),per(reuse.observeReuse),per(reuse.observeFresh),per(reuse.depthSets),per(reuse.stageSets),
        per(reuse.rtSets),per(reuse.cb1Sets),per(reuse.cbWrites),per(reuse.revokes),(unsigned long long)frames,reuse.frozen);
}
