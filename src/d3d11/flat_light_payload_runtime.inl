// Included inside flat_runtime.cpp's owner-thread implementation namespace.
// Manual Full diagnostic only. No admission, binding, game-memory or GPU writes.
lightaudit::Stamp lightStamp(const FlatProjectionShadowMetadata& m) {
    lightaudit::Stamp s;s.generation=m.generation;s.mutation=m.mutationSerial;
    s.writeGeneration=m.writeGeneration;s.bank=m.bankEpoch;s.writeFrame=m.writeFrame;s.writeQ=m.writeSequence;
    s.width=m.width;s.tracked=m.tracked;s.mapped=m.mapped;s.pending=m.pending;s.shadow=m.shadowPresent;return s;
}
bool lightLive(const State& s) {return s.lightAudit&&s.lightAudit->collector.active();}
void lightBoundCb(State& s,ID3D11DeviceContext* ctx,unsigned stage,unsigned slot,unsigned row,unsigned bytes,
                   lightaudit::Cb& out,uint32_t q=0,bool originalTrackedSnapshot=false) {
    using namespace lightaudit;out={};out.context=reinterpret_cast<uintptr_t>(ctx);out.stage=stage;out.slot=slot;out.row=row;
    if(stage==0&&slot==1)out.bindingGeneration=bindingGeneration(BindSlot::VsCb1);
    Ptr<ID3D11Buffer> buffer;UINT first=0,count=0;
    if(s.projectionContext) {
        if(stage==0)s.projectionContext->VSGetConstantBuffers1(slot,1,&buffer,&first,&count);
        else s.projectionContext->PSGetConstantBuffers1(slot,1,&buffer,&first,&count);
        out.rangeKnown=1;
    } else {
        if(stage==0)ctx->VSGetConstantBuffers(slot,1,&buffer);else ctx->PSGetConstantBuffers(slot,1,&buffer);
    }
    out.buffer=reinterpret_cast<uintptr_t>(buffer.Get());out.first=first;
    D3D11_BUFFER_DESC desc{};if(buffer){buffer->GetDesc(&desc);out.width=desc.ByteWidth;}
    if(!s.projectionContext){count=out.width/16;out.rangeKnown=buffer?1u:0u;}
    out.count=count;
    if(originalTrackedSnapshot) {
        // Production Geometry validates a fresh actual CB binding on both sides
        // of Draw. The tracked original shadow is owner-serialized, so metadata
        // and the 96-byte copy can share one lookup; private copies do not qualify.
        const bool eligible=buffer&&bytes<=kDeclaredBytes&&range(first,count,out.width,row,bytes/16);
        const auto snapshot=s.projection?s.projection->snapshotTrackedConstants(
            buffer.Get(),static_cast<uint32_t>((uint64_t(first)+row)*16),bytes,out.payload,eligible):
            FlatProjectionTrackedSnapshot{};
        out.before=lightStamp(snapshot.metadata);out.after=out.before;
        if(eligible&&out.before.tracked&&out.before.shadow&&!out.before.mapped&&!out.before.pending)
            out.byteOffset=static_cast<uint32_t>((uint64_t(first)+row)*16);
        finish(out,snapshot.copied,bytes,s.prefix.frame,q?q:s.prefix.sequence+1);
        return;
    }
    out.before=lightStamp(s.projection?s.projection->constantsMetadata(buffer.Get()):FlatProjectionShadowMetadata{});
    bool copied=false;
    if(buffer&&bytes<=kDeclaredBytes&&range(first,count,out.width,row,bytes/16)&&out.before.tracked&&
       out.before.shadow&&!out.before.mapped&&!out.before.pending) {
        out.byteOffset=static_cast<uint32_t>((uint64_t(first)+row)*16);
        copied=s.projection->copyConstants(buffer.Get(),out.byteOffset,bytes,out.payload);
    }
    out.after=lightStamp(s.projection?s.projection->constantsMetadata(buffer.Get()):FlatProjectionShadowMetadata{});
    finish(out,copied,bytes,s.prefix.frame,q?q:s.prefix.sequence+1);
}
void lightDepth(State& s,const FlatContractObservation& key,lightaudit::DepthSurface& out) {
    Ptr<ID3D11RenderTargetView> rtv;Ptr<ID3D11DepthStencilView> dsv;
    s.context->OMGetRenderTargets(1,&rtv,&dsv);Ptr<ID3D11Resource> color,depth;
    if(rtv)rtv->GetResource(&color);if(dsv)dsv->GetResource(&depth);
    out.color=reinterpret_cast<uintptr_t>(color.Get());out.rtv=reinterpret_cast<uintptr_t>(rtv.Get());
    out.resource=reinterpret_cast<uintptr_t>(depth.Get());out.view=reinterpret_cast<uintptr_t>(dsv.Get());
    out.bindingMatches=key.depth==depth.Get()&&key.dsv==dsv.Get()&&key.color==color.Get()&&key.rtv==rtv.Get();
    if(!dsv||!depth)return;D3D11_DEPTH_STENCIL_VIEW_DESC v{};dsv->GetDesc(&v);
    out.viewDimension=v.ViewDimension;out.viewFormat=v.Format;out.flags=v.Flags;
    switch(v.ViewDimension) {
    case D3D11_DSV_DIMENSION_TEXTURE2D:out.mip=v.Texture2D.MipSlice;out.slices=1;break;
    case D3D11_DSV_DIMENSION_TEXTURE2DARRAY:out.mip=v.Texture2DArray.MipSlice;out.firstSlice=v.Texture2DArray.FirstArraySlice;out.slices=v.Texture2DArray.ArraySize;break;
    case D3D11_DSV_DIMENSION_TEXTURE2DMS:out.slices=1;break;
    case D3D11_DSV_DIMENSION_TEXTURE2DMSARRAY:out.firstSlice=v.Texture2DMSArray.FirstArraySlice;out.slices=v.Texture2DMSArray.ArraySize;break;
    default:return;
    }
    Ptr<ID3D11Texture2D> t;depth.As(&t);if(!t)return;D3D11_TEXTURE2D_DESC d{};t->GetDesc(&d);
    out.width=d.Width;out.height=d.Height;out.format=d.Format;out.mips=d.MipLevels;out.array=d.ArraySize;
    out.samples=d.SampleDesc.Count;out.quality=d.SampleDesc.Quality;out.known=1;
}
void lightTextureFrame(State& s) {
    auto& a=*s.lightAudit;if(a.textureFrame==s.prefix.frame)return;
    a.textureFrame=s.prefix.frame;a.textureCount=a.textureOverflow=0;for(auto& p:a.textures)p={};
    a.collector.frame(s.prefix.frame);
}
void lightTextureDraw(State& s,const FlatRuntimeDraw& d) {
    if(!lightLive(s))return;auto& a=*s.lightAudit;const auto& k=d.key;FlatComputeInternalScope internal;
    const auto record=[&](const void* resource,const void* view,unsigned slot,bool depthWrite,
                          unsigned dimension,unsigned mip,unsigned slice,unsigned slices,bool known,unsigned format,unsigned width,unsigned height) {
        if(!resource)return;unsigned index=0;
        for(;index<a.textureCount;++index)if(a.textures[index].resource==reinterpret_cast<uintptr_t>(resource))break;
        if(index==a.textureCount){if(index==32){++a.textureOverflow;return;}++a.textureCount;}
        auto& p=a.textures[index];if(!p.resource){p.resource=reinterpret_cast<uintptr_t>(resource);p.firstQ=s.prefix.sequence+1;}
        p.frame=s.prefix.frame;p.lastQ=s.prefix.sequence+1;++p.draws;p.vs=k.vs;p.ps=k.ps;
        p.depth=reinterpret_cast<uintptr_t>(k.depth);p.dsv=reinterpret_cast<uintptr_t>(k.dsv);p.depthWrite=depthWrite;p.bad=0;
        p.format=format;p.width=width;p.height=height;p.view=reinterpret_cast<uintptr_t>(view);p.targetSlot=slot;
        p.viewDimension=dimension;p.mip=mip;p.firstSlice=slice;p.slices=slices;p.rangeKnown=known;
    };
    ID3D11RenderTargetView* raw[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
    s.context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT,raw,nullptr);
    for(unsigned slot=0;slot<D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT;++slot) {
        Ptr<ID3D11RenderTargetView> v;v.Attach(raw[slot]);if(!v)continue;Ptr<ID3D11Resource> resource;v->GetResource(&resource);
        D3D11_RENDER_TARGET_VIEW_DESC desc{};v->GetDesc(&desc);unsigned mip=0,slice=0,slices=1;bool known=true;
        if(desc.ViewDimension==D3D11_RTV_DIMENSION_TEXTURE2D)mip=desc.Texture2D.MipSlice;
        else if(desc.ViewDimension==D3D11_RTV_DIMENSION_TEXTURE2DARRAY){mip=desc.Texture2DArray.MipSlice;slice=desc.Texture2DArray.FirstArraySlice;slices=desc.Texture2DArray.ArraySize;}
        else if(desc.ViewDimension!=D3D11_RTV_DIMENSION_TEXTURE2DMS)known=false;
        Ptr<ID3D11Texture2D> texture;if(resource)resource.As(&texture);D3D11_TEXTURE2D_DESC td{};if(texture)texture->GetDesc(&td);
        record(resource.Get(),v.Get(),slot,false,desc.ViewDimension,mip,slice,slices,known&&texture,desc.Format,td.Width,td.Height);
    }
    if(k.depth){Ptr<ID3D11DepthStencilState> state;UINT ref=0;
        s.context->OMGetDepthStencilState(&state,&ref);D3D11_DEPTH_STENCIL_DESC desc{};
        desc.DepthEnable=TRUE;desc.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ALL;if(state)state->GetDesc(&desc);
        if(desc.DepthEnable&&desc.DepthWriteMask==D3D11_DEPTH_WRITE_MASK_ALL) {
            lightaudit::DepthSurface depth;lightDepth(s,k,depth);
            if(!(depth.flags&D3D11_DSV_READ_ONLY_DEPTH))record(k.depth,k.dsv,0,true,depth.viewDimension,depth.mip,depth.firstSlice,depth.slices,depth.known!=0,depth.viewFormat,depth.width,depth.height);
        }
    }
}
void lightInput(State& s,unsigned stage,unsigned slot,lightaudit::Input& out) {
    out.stage=stage;out.slot=slot;Ptr<ID3D11ShaderResourceView> view;
    if(stage==0)s.context->VSGetShaderResources(slot,1,&view);else s.context->PSGetShaderResources(slot,1,&view);
    if(!view)return;out.bound=1;out.view=reinterpret_cast<uintptr_t>(view.Get());
    Ptr<ID3D11Resource> resource;view->GetResource(&resource);out.resource=reinterpret_cast<uintptr_t>(resource.Get());
    D3D11_SHADER_RESOURCE_VIEW_DESC desc{};view->GetDesc(&desc);out.dimension=desc.ViewDimension;out.format=desc.Format;
    if(desc.ViewDimension==D3D11_SRV_DIMENSION_TEXTURE2D){out.first=desc.Texture2D.MostDetailedMip;out.count=desc.Texture2D.MipLevels;out.slices=1;out.rangeKnown=1;}
    else if(desc.ViewDimension==D3D11_SRV_DIMENSION_TEXTURE2DARRAY){out.first=desc.Texture2DArray.MostDetailedMip;out.count=desc.Texture2DArray.MipLevels;out.mips=desc.Texture2DArray.FirstArraySlice;out.slices=desc.Texture2DArray.ArraySize;out.rangeKnown=1;}
    else if(desc.ViewDimension==D3D11_SRV_DIMENSION_BUFFER){out.first=desc.Buffer.FirstElement;out.count=desc.Buffer.NumElements;out.rangeKnown=1;}
    if(!resource)return;
    Ptr<ID3D11Texture2D> texture;Ptr<ID3D11Buffer> buffer;resource.As(&texture);resource.As(&buffer);
    if(texture){D3D11_TEXTURE2D_DESC d{};texture->GetDesc(&d);out.width=d.Width;out.height=d.Height;out.samples=d.SampleDesc.Count;out.array=d.ArraySize;out.descriptorKnown=1;}
    else if(buffer){D3D11_BUFFER_DESC d{};buffer->GetDesc(&d);out.width=d.ByteWidth;out.stride=d.StructureByteStride;out.descriptorKnown=1;}
    auto& a=*s.lightAudit;out.resourceAlias=out.resource==a.collector.sample.depth;
    const auto& depth=a.collector.sample.depthSurface;
    // This is view-range identity only, not texture producer ownership.
    out.subresourceAlias=out.resourceAlias&&depth.known&&out.rangeKnown&&
        (desc.ViewDimension==D3D11_SRV_DIMENSION_TEXTURE2D||desc.ViewDimension==D3D11_SRV_DIMENSION_TEXTURE2DARRAY)&&
        out.first==depth.mip&&out.count==1&&out.mips==depth.firstSlice&&out.slices==depth.slices;
    for(unsigned i=0;i<a.textureCount;++i)if(a.textures[i].resource==out.resource){out.producer=a.textures[i];out.producerKnown=1;break;}
    // This ledger contains only observed target/depth/input identities, never CB churn.
    for(const auto& w:a.textureWrites)if(w.resource==resource.Get()){
        out.ledgerKnown=1;out.ledgerFrame=w.frame;out.ledgerQ=w.sequence;break;
    }
}
uint64_t captureLightPayloadBefore(State& s,const FlatRuntimeDraw& d,char kind,uint32_t count,uint32_t start,
                                 int32_t base,uint32_t instances,uint32_t startInstance) {
    using namespace lightaudit;if(!lightLive(s))return 0;lightTextureFrame(s);
    auto& a=*s.lightAudit;auto& c=a.collector;const auto& k=d.key;
    // All camera-bearing geometry candidates, including excluded/alternate families.
    if(k.camera&&k.depth&&k.color&&k.color!=s.prefix.output) {
        Donor donor;donor.frame=s.prefix.frame;donor.q=s.prefix.sequence+1;donor.color=reinterpret_cast<uintptr_t>(k.color);
        donor.depth=reinterpret_cast<uintptr_t>(k.depth);donor.dsv=reinterpret_cast<uintptr_t>(k.dsv);donor.rtv=reinterpret_cast<uintptr_t>(k.rtv);
        donor.b1=reinterpret_cast<uintptr_t>(k.b1);donor.vs=k.vs;donor.ps=k.ps;donor.cameraHash=k.cameraHash;
        donor.uploadFrame=k.writeEpoch;donor.uploadQ=k.writeSeq;donor.kind=k.kind;donor.supported=d.supported;
        donor.poolFamily=d.poolFamilyVs;donor.current=flatRuntimeCameraCurrent(d,s.prefix.sequence+1,s.prefix.frame);
        donor.width=k.width;donor.height=k.height;donor.format=k.format;donor.depthFormat=k.depthFormat;
        std::memcpy(donor.camera,d.camera,kCameraBytes);
        {
            FlatComputeInternalScope internal;
            lightBoundCb(s,s.context.Get(),0,1,270,kCameraBytes,donor.original);
            lightDepth(s,k,donor.depthSurface);
            donor.rangeMatches=donor.original.buffer==donor.b1&&donor.original.bytes==kCameraBytes&&
                !std::memcmp(donor.original.payload,donor.camera,kCameraBytes);
            c.donor(donor);
        }
    }
    uint64_t token=0;
    if(k.vs==kVs&&k.ps==kPs&&k.format==26&&c.begin(s.prefix.frame,s.prefix.sequence+1,
        reinterpret_cast<uintptr_t>(k.color),reinterpret_cast<uintptr_t>(k.depth))) {
        auto& p=c.sample;p.context=reinterpret_cast<uintptr_t>(s.context.Get());p.dsv=reinterpret_cast<uintptr_t>(k.dsv);p.rtv=reinterpret_cast<uintptr_t>(k.rtv);
        lightDepth(s,k,p.depthSurface);p.width=k.width;p.height=k.height;p.drawKind=kind;p.count=count;p.start=start;p.base=base;p.instances=instances;p.startInstance=startInstance;
        p.upstreamOwnerBefore=flatCameraInjectUpstreamOwns();p.phase[0]=s.phase.currentX;p.phase[1]=s.phase.currentY;p.uncertain=s.prefix.uncertain||foreignWork.load(std::memory_order_acquire);
        for(unsigned i=0;i<s.prefix.targetsUsed;++i)if(s.prefix.targets[i].resource==k.color)p.priorWrites=s.prefix.targets[i].writes.draws;
        p.producerOverflow=a.textureOverflow;p.writeEvictions=a.writeEvictions;
        FlatComputeInternalScope internal;Ptr<ID3D11VertexShader> vs;Ptr<ID3D11PixelShader> ps;
        s.context->VSGetShader(&vs,nullptr,nullptr);s.context->PSGetShader(&ps,nullptr,nullptr);
        p.actualShaders=lookupShaderHash(vs.Get())==kVs&&lookupShaderHash(ps.Get())==kPs;
        lightBoundCb(s,s.context.Get(),0,2,0,kDeclaredBytes,p.original[0]);
        lightBoundCb(s,s.context.Get(),1,2,0,kDeclaredBytes,p.original[1]);
        lightInput(s,0,0,p.inputs[0]);for(unsigned i=0;i<4;++i)lightInput(s,1,i,p.inputs[i+1]);
        token=c.generation;
    }
    if(c.open&&c.sample.frame==s.prefix.frame&&c.sample.h==reinterpret_cast<uintptr_t>(k.color)) {
        if(!k.camera&&k.depth==reinterpret_cast<const void*>(c.sample.depth))++c.sample.noCameraSameDepth;
        if(k.vs==kHdrCopyVs&&k.ps==kHdrCopyPs){c.sample.handoff=1;c.sample.handoffQ=s.prefix.sequence+1;}
        c.writer(s.prefix.frame,reinterpret_cast<uintptr_t>(k.color),{k.vs,k.ps,reinterpret_cast<uintptr_t>(k.depth),reinterpret_cast<uintptr_t>(k.dsv),s.prefix.sequence+1,k.format});
    }
    lightTextureDraw(s,d);return token;
}
void captureLightPreForward(State& s,ID3D11DeviceContext* ctx,uint64_t token,const FlatProjectionBindingPlan* plan,
                            bool indirect) {
    using namespace lightaudit;if(!s.lightAudit)return;auto& c=s.lightAudit->collector;
    if(!c.open||c.generation!=token||c.sample.frame!=s.prefix.frame)return;
    auto& p=c.sample;p.preForward=1;p.upstreamOwnerPreForward=flatCameraInjectUpstreamOwns();
    Ptr<ID3D11VertexShader> actualVs;Ptr<ID3D11PixelShader> actualPs;
    ctx->VSGetShader(&actualVs,nullptr,nullptr);ctx->PSGetShader(&actualPs,nullptr,nullptr);
    p.preForwardVs=lookupShaderHash(actualVs.Get());p.preForwardPs=lookupShaderHash(actualPs.Get());p.indirect=indirect;p.indirectKnown=1;
    FlatComputeInternalScope internal;
    for(unsigned stage=0;stage<2;++stage) {
        auto& e=p.effective[stage];lightBoundCb(s,ctx,stage,2,0,kDeclaredBytes,e.observed,p.q);
        FlatProjectionDiagnosticBinding binding;
        if(plan&&plan->diagnosticBinding(stage==0?FlatProjectionStage::Vertex:FlatProjectionStage::Pixel,2,binding)) {
            e.planOriginal=reinterpret_cast<uintptr_t>(binding.original);e.planReplacement=reinterpret_cast<uintptr_t>(binding.replacement);
            e.planRevision=binding.revision;e.preparedRevision=binding.preparedRevision;e.planReady=binding.ready;e.planFirst=binding.first;e.planCount=binding.count;
            if(binding.ready&&plan->copyUploadedConstants(stage==0?FlatProjectionStage::Vertex:FlatProjectionStage::Pixel,2,
                static_cast<uint32_t>(uint64_t(binding.first)*16),kDeclaredBytes,e.expected))e.expectedBytes=kDeclaredBytes;
            const bool associated=binding.ready&&e.planOriginal==p.original[stage].buffer&&e.planReplacement==e.observed.buffer&&
                binding.first==e.observed.first&&binding.count==e.observed.count&&same(p.original[stage].after,
                    lightStamp(s.projection->constantsMetadata(static_cast<ID3D11Buffer*>(binding.original))));
            if(associated&&e.expectedBytes==kDeclaredBytes)e.association=2; // expected uploaded bytes stay distinct from observed CPU shadow
        }
        if(e.observed.buffer==p.original[stage].buffer&&same(p.original[stage].after,e.observed.before)&&
           e.observed.bytes==kDeclaredBytes)e.association=1;
    }
}
void captureLightReturned(State& s,uint64_t token) {
    if(!s.lightAudit)return;auto& c=s.lightAudit->collector;
    if(!c.open||c.generation!=token||c.sample.frame!=s.prefix.frame)return;
    c.sample.drawReturned=1;
    for(const auto& original:c.sample.original)if(s.projection&&original.buffer&&
        !lightaudit::same(original.after,lightStamp(s.projection->constantsMetadata(reinterpret_cast<ID3D11Buffer*>(original.buffer)))))c.sample.mutation=1;
}
bool lightWriteVerified(const std::wstring& path,const unsigned char* bytes,size_t size) {
    FILE* f=nullptr;if(_wfopen_s(&f,path.c_str(),L"wb")||!f)return false;
    const bool wrote=std::fwrite(bytes,1,size,f)==size;const bool closed=std::fclose(f)==0;if(!wrote||!closed)return false;
    if(_wfopen_s(&f,path.c_str(),L"rb")||!f)return false;
    unsigned char block[4096];size_t at=0;bool same=true;
    while(at<size){const size_t n=std::min(sizeof(block),size-at);if(std::fread(block,1,n,f)!=n||std::memcmp(block,bytes+at,n)){same=false;break;}at+=n;}
    if(same&&std::fgetc(f)!=EOF)same=false;if(std::ferror(f))same=false;return std::fclose(f)==0&&same;
}
void lightSavePending(State& s) {
    if(!s.lightAudit)return;auto& a=*s.lightAudit;auto& c=a.collector;if(!c.pending)return;
    bool okay=false;try {
        const auto bytes=lightaudit::encode(c.sample);const std::string name="sample_"+std::to_string(c.attempts)+".lca";
        okay=lightWriteVerified(a.directory+L"\\"+std::wstring(name.begin(),name.end()),bytes.data(),bytes.size());
        if(okay)a.files.push_back(name);
    } catch(...) {okay=false;}
    Log::get().note("flat light camera audit: event=sample-written generation=%llu frame=%llu q=%u attempt=%u write-verified=%u declared-bytes=752 VS-status=%u VS-bytes=%u VS-freshness=%u PS-status=%u PS-bytes=%u PS-freshness=%u donors=%u donor-overflow=%u writers=%u writer-overflow=%u pre-forward=%u draw-returned=%u record-complete=%u; observation only, no camera authorization or GPU execution receipt",
        (unsigned long long)c.generation,(unsigned long long)c.sample.frame,c.sample.q,c.attempts,okay?1u:0u,
        c.sample.original[0].status,c.sample.original[0].bytes,c.sample.original[0].fresh,c.sample.original[1].status,c.sample.original[1].bytes,c.sample.original[1].fresh,
        c.sample.donorCount,c.sample.donorOverflow,c.sample.writerCount,c.sample.writerOverflow,c.sample.preForward,c.sample.drawReturned,lightaudit::recordComplete(c.sample)?1u:0u);
    c.saved(okay);
}
void finishLightAudit(State& s,const char* reason) {
    if(!s.lightAudit)return;auto& a=*s.lightAudit;auto& c=a.collector;if(!c.armed||a.finalized)return;
    if(c.open){c.sample.abandoned=1;c.finishFrame();}lightSavePending(s);
    c.done=true;c.armed=false;bool okay=false;
    try {
        std::ostringstream o;o<<"{\"schema\":\"edvr-light-camera-audit\",\"version\":1,\"wire\":\"EDVRLCA1\",\"sample_bytes\":"<<sizeof(lightaudit::Sample)
            <<",\"generation\":"<<c.generation<<",\"arm_frame\":"<<c.armFrame<<",\"status\":\""<<reason<<"\",\"attempts\":"<<c.attempts
            <<",\"record_complete\":"<<c.good<<",\"write_failures\":"<<c.writeFailures<<",\"pending\":0,\"authorization\":false,\"observation\":\"audited-frame-only CPU pre-forward; original means pre-private replacement, no GPU execution receipt\",\"input_contents\":\"uncaptured\",\"files\":[";
        for(size_t i=0;i<a.files.size();++i){if(i)o<<',';o<<'"'<<a.files[i]<<'"';}o<<"]}";const auto body=o.str();
        okay=body.size()<4096&&lightWriteVerified(a.directory+L"\\manifest.json",reinterpret_cast<const unsigned char*>(body.data()),body.size());
    }catch(...){okay=false;}
    a.finalized=true;
    Log::get().note("flat light camera audit: event=COMPLETE generation=%llu reason=%s attempts=%u files=%u write-failures=%u manifest-verified=%u pending=0 drained=1 result=%s path=%ls; file completion only, no admission/camera/GPU execution certification",
        (unsigned long long)c.generation,reason,c.attempts,c.written,c.writeFailures,okay?1u:0u,
        !okay||c.writeFailures?"write-failure":!c.attempts?"never-ran":c.good?"record-complete":"payload-only-or-partial",a.directory.c_str());
}
void lightAuditFrameEnd(State& s,uint64_t frame) {
    if(!s.lightAudit)return;auto& c=s.lightAudit->collector;
    if(c.open&&c.sample.frame<=frame){c.sample.producerOverflow=s.lightAudit->textureOverflow;c.sample.writeEvictions=s.lightAudit->writeEvictions;c.sample.uncertain|=s.prefix.uncertain||foreignWork.load(std::memory_order_acquire);c.finishFrame();lightSavePending(s);}
    if(c.done)finishLightAudit(s,"budget-complete");
    else if(c.expired(frame))finishLightAudit(s,"audit-expired");
}
void armLightAudit(State& s,uint64_t frame) {
    finishLightAudit(s,"cancelled-rearm");
    if(!s.lightAudit)s.lightAudit.reset(new(std::nothrow) LightAuditState);
    if(!s.lightAudit){Log::get().note("flat light camera audit: event=arm-refused reason=allocation-failed pending=0 drained=1");return;}
    auto& a=*s.lightAudit;a.finalized=false;a.files.clear();a.textureFrame=0;a.textureCount=a.textureOverflow=0;
    for(auto& w:a.textureWrites)w={};a.writeNext=a.writeEvictions=0;
    const auto root=Config::get().logDir()+L"\\light_camera_payload";
    const bool rootOkay=CreateDirectoryW(root.c_str(),nullptr)||GetLastError()==ERROR_ALREADY_EXISTS;
    const uint64_t generation=++s.lightAuditGeneration;
    a.directory=root+L"\\audit_"+std::to_wstring(frame)+L"_"+std::to_wstring(GetTickCount64())+L"_"+std::to_wstring(generation);
    if(!rootOkay||!CreateDirectoryW(a.directory.c_str(),nullptr)){
        a.finalized=true;Log::get().note("flat light camera audit: event=arm-refused reason=directory-failed pending=0 drained=1");return;}
    a.collector.arm(frame,generation);
    Log::get().note("flat light camera audit: event=armed generation=%llu arm-frame=%llu anchor=8289669D93A18C1D/C6E6E419DA9F6FAD attempts=4 record-complete-save-stop=2 spacing=91 declared-CB2-bytes=752 donor-limit=64 writer-limit=128 producer-limit=32 no-GPU-readback=1 path=%ls; manual Full only, independent of diag.flat_origin_capture",
        (unsigned long long)generation,(unsigned long long)frame,a.directory.c_str());
}
void lightAuditMutation(State& s,ID3D11Resource* resource) {
    if(!lightLive(s)||!resource)return;auto& a=*s.lightAudit;auto& c=a.collector;
    c.writtenResource(reinterpret_cast<uintptr_t>(resource));bool known=false;
    for(unsigned i=0;i<a.textureCount;++i)known|=a.textures[i].resource==reinterpret_cast<uintptr_t>(resource)||a.textures[i].depth==reinterpret_cast<uintptr_t>(resource);
    if(c.open)for(const auto& input:c.sample.inputs)known|=input.resource==reinterpret_cast<uintptr_t>(resource);
    if(!known)return;
    for(auto& producer:a.textures)if(producer.resource==reinterpret_cast<uintptr_t>(resource))producer.bad=1;
    LightAuditState::Write* write=nullptr;
    for(auto& w:a.textureWrites)if(w.resource==resource){write=&w;break;}
    if(!write){write=&a.textureWrites[a.writeNext++%32];if(write->resource)++a.writeEvictions;}
    *write={resource,s.prefix.frame,s.prefix.sequence};
}
