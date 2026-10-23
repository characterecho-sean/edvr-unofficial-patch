// Fixed CPU-only receipt. Never changes qualification or requests a readback.
void forwardPhaseRelease(State& s) {
    for(auto& h:s.forwardLight.phasePending)h={};for(auto& h:s.forwardLight.phaseCommitted)h={};
}
void forwardPhaseLifecycle(State& s) {s.forwardLight.phaseReceipt.lifecycle();forwardPhaseRelease(s);}
void forwardPhaseObserve(State& s,const FlatRuntimeDraw& draw,const forward_light::Domain& domain,
        const forward_light::Publication& publication,const ForwardConstants constants[2],
        const FlatProjectionJitter& phase,forward_light::Reason originalReason) {
    auto& capture=s.forwardLight.phaseReceipt;if(capture.frozen)return;
    if(capture.discardSuccessfulNonzero(originalReason,s.phase.currentX,s.phase.currentY))return;
    phase_failure::Receipt r;const auto& k=draw.key;r.frame=s.prefix.frame;r.q=s.prefix.sequence+1;
    r.context=reinterpret_cast<uintptr_t>(s.context.Get());r.device=reinterpret_cast<uintptr_t>(s.device.Get());
    r.hdr=reinterpret_cast<uintptr_t>(k.color);r.dsv=reinterpret_cast<uintptr_t>(k.dsv);r.vs=k.vs;r.ps=k.ps;r.width=k.width;r.height=k.height;
    r.raw=publication.raw;r.linear=publication.linear;r.publication=publication;r.source=domain.source;r.domainVersions=domain.versions;
    r.phaseWidth=s.phaseWidth;r.phaseHeight=s.phaseHeight;r.pixel[0]=s.phase.currentX;r.pixel[1]=s.phase.currentY;
    r.previousPixel[0]=s.phase.previousX;r.previousPixel[1]=s.phase.previousY;r.priorAccepted=s.phase.previousAcceptedValid;
    r.ndc=phase;r.applied=s.phase.applied;r.owner=flatCameraInjectUpstreamOwns()?1u:0u;
    r.selectedHdr=reinterpret_cast<uintptr_t>(s.phaseHdr.Get());r.selectedRaw=reinterpret_cast<uintptr_t>(s.phaseDepth.Get());
    r.selectedFrame=s.havePrevious?s.previous.frame:0;
    r.epoch=capture.epoch;r.selectedSource=capture.lastSelection.source;
    r.lastSelectedDomain=capture.selectionKnown&&capture.lastSelection.frame==r.selectedFrame&&
        r.selectedFrame&&r.selectedFrame<r.frame&&r.selectedHdr==r.hdr&&r.selectedRaw==r.raw.resource&&
        r.phaseWidth==r.width&&r.phaseHeight==r.height&&phase_failure::selectedCohort(capture.lastSelection,r);
    std::memcpy(r.viewport,k.viewport,sizeof(r.viewport));std::memcpy(r.camera,domain.camera,96);
    for(unsigned stage=0;stage<2;++stage){r.original[stage]=constants[stage].id;std::memcpy(r.cb[stage],constants[stage].bytes,752);}
    // Stamp from the geometry observation that supplied these exact96B,
    // not potentially newer unused bytes in that CPU buffer at light time.
    r.donor=s.forwardLight.phaseObservedDonor[static_cast<unsigned>(&domain-s.forwardLight.ledger.domains)];
    r.donorFlags=3; // tracked, shadow-present, not mapped/pending at observation
    r.donorStampMatches=r.donor.generation==domain.source.generation&&r.donor.bank==domain.source.bank&&r.donor.writeGeneration==domain.source.writeGeneration;
    r.originalReason=originalReason;r.diagnostic=phase_failure::analyze(r.camera,r.cb[0],phase);
    const int slot=capture.offer(r);if(slot<0)return;
    auto& hold=s.forwardLight.phasePending[slot];hold={};hold.hdr=static_cast<ID3D11Resource*>(const_cast<void*>(k.color));
    hold.raw=reinterpret_cast<ID3D11Resource*>(r.raw.resource);hold.linear=reinterpret_cast<ID3D11Resource*>(r.linear.resource);
    hold.source=reinterpret_cast<ID3D11Buffer*>(r.source.buffer);
    for(unsigned stage=0;stage<2;++stage)hold.original[stage]=reinterpret_cast<ID3D11Buffer*>(r.original[stage].buffer);
}
void forwardPhaseSelected(State& s,const FlatMonoFrame& selected) {
    auto& c=s.forwardLight.phaseReceipt;if(c.frozen)return;
    // This callback runs only after actual successful temporal resolve. Pin
    // the source cohort independently of whether its payload snapshot exists.
    c.selectionKnown=false;
    for(unsigned i=0;i<s.forwardLight.ledger.used;++i){const auto& domain=s.forwardLight.ledger.domains[i];
        if(domain.raw.resource!=reinterpret_cast<uintptr_t>(selected.depth)||domain.failure!=forward_light::Reason::None||
           domain.source.buffer!=reinterpret_cast<uintptr_t>(selected.sceneConstants)||!domain.origins||std::memcmp(domain.camera,selected.camera,96))continue;
        bool typed=false;for(const auto& a:s.forwardLight.authority)if(a.live&&a.frame==selected.frame&&a.hdr==reinterpret_cast<uintptr_t>(selected.hdr)&&a.raw==domain.raw.resource)typed=true;
        if(!typed)continue;
        auto& r=c.lastSelection;r={};r.epoch=c.epoch;r.frame=selected.frame;r.context=reinterpret_cast<uintptr_t>(s.context.Get());r.device=reinterpret_cast<uintptr_t>(s.device.Get());
        r.hdr=reinterpret_cast<uintptr_t>(selected.hdr);r.raw=domain.raw;r.source=domain.source;r.width=selected.renderWidth;r.height=selected.renderHeight;c.selectionKnown=true;break;
    }
    c.selected(selected.frame,reinterpret_cast<uintptr_t>(selected.hdr),reinterpret_cast<uintptr_t>(selected.depth),
        reinterpret_cast<uintptr_t>(selected.sceneConstants),reinterpret_cast<const unsigned char*>(selected.camera));
    for(unsigned i=0;i<phase_failure::kSlots;++i)if(c.valid[i]&&c.committed[i].frame==selected.frame)
        s.forwardLight.phaseCommitted[i]=s.forwardLight.phasePending[i];
}
void forwardPhaseIdentity(const char* role,const char* stage,const forward_light::ConstantIdentity& c) {
    Log::get().note("flat phase receipt identity: role=%s stage=%s buffer=%llX generation=%llu mutation=%llu writeGeneration=%llu bank=%llu writeFrame=%llu writeQ=%u first=%u count=%u width=%u bytes=%u hash=%08X",
        role,stage,(unsigned long long)c.buffer,(unsigned long long)c.generation,(unsigned long long)c.mutation,(unsigned long long)c.writeGeneration,
        (unsigned long long)c.bank,(unsigned long long)c.writeFrame,c.writeQ,c.first,c.count,c.width,c.bytes,c.hash);
}
unsigned forwardPhasePayload(const char* role,const char* name,const unsigned char* bytes,unsigned count) {
    const uint32_t hash=phase_failure::hash(bytes,count);unsigned chunks=0;
    for(unsigned offset=0;offset<count;offset+=128){const unsigned n=std::min(128u,count-offset);char hex[257];
        for(unsigned i=0;i<n;++i)std::snprintf(hex+i*2,3,"%02X",bytes[offset+i]);
        Log::get().note("flat phase receipt payload: role=%s name=%s offset=%u bytes=%u total=%u hash=%08X hex=%s",role,name,offset,n,count,hash,hex);++chunks;}
    return chunks;
}
unsigned forwardPhaseSample(const char* role,const phase_failure::Receipt& r) {
    const auto& a=r.diagnostic;
    Log::get().note("flat phase receipt sample: role=%s epoch=%llu frame=%llu q=%u context=%llX device=%llX H=%llX DSV=%llX raw=%llX linear=%llX width=%u height=%u VS=%016llX PS=%016llX originalReason=%s diagnosticReason=%s issued=%u returned=%u selected=%u lastSelectedDomain=%u selectedFrame=%llu selectedH=%llX selectedRaw=%llX recentCount=%u",
        role,(unsigned long long)r.epoch,(unsigned long long)r.frame,r.q,(unsigned long long)r.context,(unsigned long long)r.device,(unsigned long long)r.hdr,(unsigned long long)r.dsv,
        (unsigned long long)r.raw.resource,(unsigned long long)r.linear.resource,r.width,r.height,(unsigned long long)r.vs,(unsigned long long)r.ps,
        forward_light::name(r.originalReason),forward_light::name(a.reason),r.issued?1u:0u,r.returned?1u:0u,r.selected?1u:0u,r.lastSelectedDomain?1u:0u,
        (unsigned long long)r.selectedFrame,(unsigned long long)r.selectedHdr,(unsigned long long)r.selectedRaw,r.recentCount);
    Log::get().note("flat phase receipt phase: role=%s pixelX=%.9g pixelY=%.9g ndcX=%.9g ndcY=%.9g phaseWidth=%u phaseHeight=%u upstream=%u applied=%u previousX=%.9g previousY=%.9g priorAccepted=%u viewport=(%.9g,%.9g,%.9g,%.9g,%.9g,%.9g)",
        role,r.pixel[0],r.pixel[1],r.ndc.ndcX,r.ndc.ndcY,r.phaseWidth,r.phaseHeight,r.owner,r.applied,r.previousPixel[0],r.previousPixel[1],r.priorAccepted?1u:0u,
        r.viewport[0],r.viewport[1],r.viewport[2],r.viewport[3],r.viewport[4],r.viewport[5]);
    Log::get().note("flat phase receipt fit: role=%s stage=%s axis=%d column=%d fittedX=%.17g fittedY=%.17g scaleX=%.17g scaleY=%.17g dephasedScaleX=%.17g dephasedScaleY=%.17g resolvedX=%u resolvedY=%u lhs=%.17g rhs=%.17g residual=%.17g bound=%.17g; zero-fit can overlap unphased and previous-zero epoch, no unique heuristic classification",
        role,phase_failure::stageName(a.stage),a.axis,a.column,a.fitted[0],a.fitted[1],a.scale[0],a.scale[1],a.dephasedScale[0],a.dephasedScale[1],a.resolved[0]?1u:0u,a.resolved[1]?1u:0u,a.lhs,a.rhs,a.residual,a.bound);
    Log::get().note("flat phase receipt lineage: role=%s publication=%llu publicationFrame=%llu publicationQ=%u publicationIssued=%u publicationReturned=%u rawWidth=%u rawHeight=%u rawKnown=%u linearWidth=%u linearHeight=%u linearKnown=%u rawMip=%u rawSlice=%u rawSlices=%u rawSamples=%u linearMip=%u linearSlice=%u linearSlices=%u linearSamples=%u source=%llX sourceGeneration=%llu sourceBank=%llu sourceWriteGeneration=%llu sourceWitnessFrame=%llu sourceWitnessQ=%u sourceFirst=%u sourceCount=%u domainVersions=%u donorFlags=%u donorStampMatches=%u selectedSource=%llX selectedGeneration=%llu selectedBank=%llu selectedFirst=%u selectedCount=%u",
        role,(unsigned long long)r.publication.token,(unsigned long long)r.publication.frame,r.publication.q,r.publication.issued?1u:0u,r.publication.returned?1u:0u,r.raw.width,r.raw.height,r.raw.known?1u:0u,r.linear.width,r.linear.height,r.linear.known?1u:0u,
        r.raw.mip,r.raw.slice,r.raw.slices,r.raw.samples,r.linear.mip,r.linear.slice,r.linear.slices,r.linear.samples,
        (unsigned long long)r.source.buffer,(unsigned long long)r.source.generation,(unsigned long long)r.source.bank,(unsigned long long)r.source.writeGeneration,(unsigned long long)r.source.frame,
        r.source.writeQ,r.source.first,r.source.count,r.domainVersions,r.donorFlags,r.donorStampMatches?1u:0u,(unsigned long long)r.selectedSource.buffer,(unsigned long long)r.selectedSource.generation,
        (unsigned long long)r.selectedSource.bank,r.selectedSource.first,r.selectedSource.count);
    forwardPhaseIdentity(role,"donor",r.donor);forwardPhaseIdentity(role,"vs2",r.original[0]);forwardPhaseIdentity(role,"ps2",r.original[1]);
    for(unsigned i=0;i<r.recentCount;++i){const auto& h=r.recent[i];Log::get().note("flat phase receipt requested: role=%s index=%u epoch=%llu frame=%llu pixelX=%.9g pixelY=%.9g width=%u height=%u",role,i,(unsigned long long)h.epoch,(unsigned long long)h.frame,h.pixel[0],h.pixel[1],h.width,h.height);}
    unsigned chunks=forwardPhasePayload(role,"donor96",r.camera,96);chunks+=forwardPhasePayload(role,"vs2",r.cb[0],752);chunks+=forwardPhasePayload(role,"ps2",r.cb[1],752);
    Log::get().note("flat phase receipt END: role=%s bytes=1600 chunks=%u hash=%08X",role,chunks,phase_failure::payloadHash(r));return chunks;
}
void forwardPhaseReport(State& s) {
    auto& c=s.forwardLight.phaseReceipt;
    if(c.frozen&&!c.emitted){
        Log::get().note("flat phase receipt manifest: version=1 generation=1 matched=%u overflow=%u resets=%llu cancelled=%llu; pre-issue rejected candidate tied to last-selected domain, bootstrap CPU-returned and temporal-selected, no GPU execution receipt, original means app-bound before private replacement",
            c.matched?1u:0u,c.overflow,(unsigned long long)c.resets,(unsigned long long)c.cancelled);
        unsigned chunks=0;uint32_t hash=2166136261u;if(c.matched){chunks+=forwardPhaseSample("bootstrap",c.paired);hash=phase_failure::payloadHash(c.paired,hash);}
        chunks+=forwardPhaseSample("failure",c.failure);hash=phase_failure::payloadHash(c.failure,hash);
        Log::get().note("flat phase receipt COMPLETE: version=1 generation=1 records=%u bytes=%u chunks=%u hash=%08X result=%s; all receipt lines queued in order, checksum covers raw payload only; decoder also requires every metadata record/index; all lines queued, no readback or asynchronous capture jobs",
            c.matched?2u:1u,c.matched?3200u:1600u,chunks,hash,c.matched?"paired":"missing-bootstrap");
        c.emitted=true;forwardPhaseRelease(s);
    }
    Log::get().note("flat phase receipt status: state=%s primaryEntries=%llu eligibleSnapshots=%llu notArmed=%llu phaseFailures=%llu outsideLastSelected=%llu zeroSamples=%llu evictions=%u resets=%llu cancelled=%llu processCap=1; automatic, probe and Full independent",
        c.state(),(unsigned long long)c.entries,(unsigned long long)c.calls,(unsigned long long)c.notArmed,(unsigned long long)c.primaryFailures,
        (unsigned long long)c.noSelectedDomain,(unsigned long long)c.zeroSamples,c.overflow,(unsigned long long)c.resets,(unsigned long long)c.cancelled);
}
