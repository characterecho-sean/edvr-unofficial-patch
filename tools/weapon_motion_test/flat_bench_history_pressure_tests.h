#pragma once

#include <cstdio>
#include <cstring>
#include <vector>

// Reconstructed indexed draws exercise the real WARP original-VS, identity
// dispatch and stream-output history. These labels describe synthetic input;
// AnimatedVertexHistory itself has no world/foreign classification policy.
struct FlatHistoryPressureResult {
    const char* name;
    unsigned captures=0, records=0, bytes=0, priorCandidates=0, retired=0;
    const char* firstRefusal="";
};

inline ComPtr<ID3D11Buffer> flatHistoryPressureIndexBuffer(
    ID3D11Device* dev,const std::vector<UINT>& indices) {
    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth=UINT(indices.size()*sizeof(UINT));
    desc.BindFlags=D3D11_BIND_INDEX_BUFFER;
    D3D11_SUBRESOURCE_DATA data{};data.pSysMem=indices.data();
    ComPtr<ID3D11Buffer> result;
    hr(dev->CreateBuffer(&desc,&data,&result));
    return result;
}

inline std::vector<unsigned char> flatHistoryPressurePositionBytes(
    ID3D11Device* dev,ID3D11DeviceContext* ctx,ID3D11ShaderResourceView* view) {
    ComPtr<ID3D11Resource> resource;
    view->GetResource(&resource);
    ComPtr<ID3D11Buffer> source;
    hr(resource.As(&source));
    D3D11_BUFFER_DESC desc{};
    source->GetDesc(&desc);
    desc.Usage=D3D11_USAGE_STAGING;
    desc.BindFlags=0;
    desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    desc.MiscFlags=0;
    desc.StructureByteStride=0;
    ComPtr<ID3D11Buffer> staging;
    hr(dev->CreateBuffer(&desc,nullptr,&staging));
    ctx->CopyResource(staging.Get(),source.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    hr(ctx->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped));
    std::vector<unsigned char> bytes(desc.ByteWidth);
    std::memcpy(bytes.data(),mapped.pData,bytes.size());
    ctx->Unmap(staging.Get(),0);
    return bytes;
}

inline FlatForegroundMotion::Inputs flatHistoryPressureInputs(unsigned token) {
    FlatForegroundMotion::Inputs inputs{};
    inputs.camera[3][2]=.0675f;
    inputs.writerToken=token;
    inputs.gpuIdentity=true;
    return inputs;
}

inline ComPtr<ID3D11GeometryShader> flatHistoryPressureStreamOutput(
    ID3D11DeviceContext* ctx,AnimatedVertexHistory& history,
    const AnimatedVertexHistory::Capture& capture) {
    D3D11_PRIMITIVE_TOPOLOGY topology{};
    ctx->IAGetPrimitiveTopology(&topology);
    ComPtr<ID3D11GeometryShader> previous,result;
    ctx->GSGetShader(&previous,nullptr,nullptr);
    history.bindPositions(ctx,capture);
    ctx->GSGetShader(&result,nullptr,nullptr);
    ctx->GSSetShader(previous.Get(),nullptr,0);
    ctx->IASetPrimitiveTopology(topology);
    return result;
}

template<class Bind>
inline std::vector<FlatHistoryPressureResult> flatBenchHistoryPressureTests(
    ID3D11Device* dev,ID3D11DeviceContext* ctx,Bind bind,bool printJson) {
    std::vector<FlatHistoryPressureResult> results;
    ctx->ClearState();
    bind(Pose{},false);
    // Every record-limit case is written against the production cap, so raising it moves the cases with it.
    constexpr unsigned cap=AnimatedVertexHistory::maxRecords;

    // The cap's worth of small, distinct geometries are provisionally
    // captured before any caller names a source. The next one is labelled
    // genuine foreign by this workload, but only a caller could reserve
    // space for it.
    {
        std::vector<UINT> indices((cap+1)*3);
        for(unsigned i=0;i<cap+1;++i){indices[i*3]=0;indices[i*3+1]=1;indices[i*3+2]=2;}
        auto ib=flatHistoryPressureIndexBuffer(dev,indices);
        ctx->IASetIndexBuffer(ib.Get(),DXGI_FORMAT_R32_UINT,0);
        AnimatedVertexHistory history;
        FlatHistoryPressureResult result{"provisional_before_foreign"};
        for(unsigned i=0;i<cap+1;++i) {
            AnimatedVertexHistory::Capture capture;
            const bool admitted=history.capture(ctx,issue,3,1,i*3,0,0,100,capture);
            if(i<cap) {
                check(admitted && capture.currentPositions && capture.currentIdentity,
                      "reconstructed provisional GPU captures fill record budget");
                ++result.captures;
            } else {
                check(!admitted && capture.refusal && !std::strcmp(capture.refusal,"history-budget"),
                      "reconstructed foreign draw observes first record-budget refusal");
                result.firstRefusal=capture.refusal;
            }
        }
        result.records=unsigned(history.recordCount());result.bytes=history.bytes();
        check(result.records==cap && result.bytes==cap*3*32,
              "record pressure reports actual retained GPU-history allocation");
        history.advance(101);
        AnimatedVertexHistory::Capture reused;
        check(history.capture(ctx,issue,3,1,0,0,0,101,reused) && reused.candidateCount==1,
              "previous-frame indexed geometry reuses real GPU history at the record cap");
        result.priorCandidates=reused.candidateCount;
        check(history.recordCount()==cap && history.bytes()==result.bytes,
              "history reuse does not allocate a record past the cap");
        history.advance(104);
        check(history.recordCount()==0 && history.bytes()==0,
              "two absent frames retire the full provisional record budget");
        result.retired=cap;
        results.push_back(result);
    }

    // All indices address the rig's four real vertices. Distinct near-limit
    // counts make distinct geometry keys while keeping upload data at 512 KiB.
    {
        constexpr unsigned largeCount=131070;
        std::vector<UINT> indices(largeCount);
        for(unsigned i=0;i<largeCount;++i)indices[i]=i%3;
        auto ib=flatHistoryPressureIndexBuffer(dev,indices);
        ctx->IASetIndexBuffer(ib.Get(),DXGI_FORMAT_R32_UINT,0);
        AnimatedVertexHistory history;
        FlatHistoryPressureResult result{"byte_budget"};
        for(unsigned i=0;i<9;++i) {
            AnimatedVertexHistory::Capture capture;
            const unsigned count=largeCount-i*3;
            const bool admitted=history.capture(ctx,issue,count,1,0,0,0,200,capture);
            if(i<8) {
                check(admitted && capture.currentPositions && capture.currentIdentity,
                      "reconstructed large indexed draw reaches real GPU history");
                ++result.captures;
            } else {
                check(!admitted && capture.refusal && !std::strcmp(capture.refusal,"history-budget"),
                      "ninth large draw observes first byte-budget refusal");
                result.firstRefusal=capture.refusal;
            }
        }
        result.records=unsigned(history.recordCount());result.bytes=history.bytes();
        const unsigned expectedBytes=8*largeCount*32-3*32*28;
        check(result.records==8 && result.bytes==expectedBytes &&
              result.bytes<=AnimatedVertexHistory::maxBytes &&
              result.bytes+(largeCount-8*3)*32>AnimatedVertexHistory::maxBytes,
              "byte pressure measures real allocation before the record limit");
        results.push_back(result);

        // Reuse those eight real SO captures to isolate stale byte occupancy:
        // the fresh 102-index draw exceeds 32 MiB by 64 bytes, with only eight
        // records present. The known write invalidates every old key first.
        history.advance(201);
        indices[0]=2;indices[2]=0;
        ctx->UpdateSubresource(ib.Get(),0,nullptr,indices.data(),0,0);
        check(history.resourceWritten(ib.Get())==4 &&
              history.recordCount()==8 && history.bytes()==expectedBytes,
              "known IB write leaves invalidated position bytes accounted in frame");
        std::vector<UINT> freshIndices(102);
        for(unsigned i=0;i<freshIndices.size();++i)freshIndices[i]=i%3;
        auto freshIb=flatHistoryPressureIndexBuffer(dev,freshIndices);
        ctx->IASetIndexBuffer(freshIb.Get(),DXGI_FORMAT_R32_UINT,0);
        AnimatedVertexHistory::Capture fresh;
        check(history.recordCount()<AnimatedVertexHistory::maxRecords &&
              uint64_t(history.bytes())+102*32>AnimatedVertexHistory::maxBytes,
              "invalidated prior bytes alone would exceed the unchanged limit");
        check(history.capture(ctx,issue,102,1,0,0,0,201,fresh) && fresh.candidateCount==0,
              "pressure reclaims invalidated bytes and admits fresh same-frame GPU draw");
        const auto pressured=history.accounting(201);
        check(pressured.recordCount==8 && pressured.invalid==7 && pressured.current==1 &&
              pressured.reclaimedRecords==1 && pressured.reclaimedBytes==largeCount*32,
              "byte-pressure compaction stops after one invalid record makes room");
        history.advance(202);
        check(history.recordCount()==1 && history.bytes()==102*32 &&
              history.accounting(202).reclaimedRecords==0,
              "advance retires remaining invalidated bytes and resets pressure receipt");
        check(history.capture(ctx,issue,102,1,0,0,0,202,fresh) && fresh.candidateCount==1,
              "fresh GPU draw remains admitted after byte-pressure retirement");
        FlatHistoryPressureResult invalidated{"invalidated_byte_occupancy"};
        invalidated.captures=10;invalidated.records=pressured.recordCount;invalidated.bytes=pressured.bytes;
        invalidated.retired=8;
        results.push_back(invalidated);
    }

    {
        std::vector<UINT> indices(12*3);
        for(unsigned i=0;i<12;++i){indices[i*3]=0;indices[i*3+1]=1;indices[i*3+2]=2;}
        auto ib=flatHistoryPressureIndexBuffer(dev,indices);
        ctx->IASetIndexBuffer(ib.Get(),DXGI_FORMAT_R32_UINT,0);
        AnimatedVertexHistory history;
        FlatHistoryPressureResult result{"transient_churn"};
        for(unsigned frame=300;frame<302;++frame) {
            history.advance(frame);
            for(unsigned i=0;i<6;++i) {
                AnimatedVertexHistory::Capture capture;
                check(history.capture(ctx,issue,3,1,(frame-300)*18+i*3,0,0,frame,capture),
                      "reconstructed transient indexed geometry captures on GPU");
                ++result.captures;
            }
        }
        history.advance(302);
        check(history.recordCount()==12 && history.bytes()==12*3*32,
              "two-frame-old transient histories remain within eviction window");
        const auto usage=history.accounting(302);
        check(usage.recordCount==usage.invalid+usage.pending+usage.current+usage.prior+usage.older &&
              usage.prior==6 && usage.older==6 && usage.reclaimedRecords==0,
              "usage partitions valid prior and older records without double counting");
        history.advance(303);
        check(history.recordCount()==6 && history.bytes()==6*3*32,
              "third frame retires first transient cohort");
        result.retired=6;
        result.records=unsigned(history.recordCount());result.bytes=history.bytes();
        history.advance(304);
        check(history.recordCount()==0 && history.bytes()==0,
              "next absent frame retires remaining transient cohort");
        results.push_back(result);
    }

    {
        std::vector<UINT> indices{0,1,2};
        auto ib=flatHistoryPressureIndexBuffer(dev,indices);
        ctx->IASetIndexBuffer(ib.Get(),DXGI_FORMAT_R32_UINT,0);
        AnimatedVertexHistory history;
        FlatHistoryPressureResult result{"mutation_reset"};
        AnimatedVertexHistory::Capture capture;
        check(history.capture(ctx,issue,3,1,0,0,0,400,capture),
              "reconstructed mutation case first captures actual GPU history");
        ++result.captures;
        const UINT rewrittenIndices[3]{0,2,1};
        ctx->UpdateSubresource(ib.Get(),0,nullptr,rewrittenIndices,0,0);
        check(history.resourceWritten(ib.Get())==4,
              "index mutation invalidates correspondence in actual history");
        history.advance(401);
        check(history.recordCount()==0 && history.bytes()==0,
              "index mutation releases allocation before new frame capture");
        check(history.capture(ctx,issue,3,1,0,0,0,401,capture) && capture.candidateCount==0,
              "new capture after index mutation has no stale prior candidate");
        ++result.captures;
        ComPtr<ID3D11Buffer> boundVertices;UINT stride=0,offset=0;
        ctx->IAGetVertexBuffers(1,1,&boundVertices,&stride,&offset);
        const float rewrittenVertices[4][4]{
            {-.59f,-.6f,1.f,0.f},{.61f,-.6f,1.f,0.f},
            {.61f,.6f,1.25f,1.f},{-.59f,.6f,1.25f,1.f}};
        ctx->UpdateSubresource(boundVertices.Get(),0,nullptr,rewrittenVertices,0,0);
        check(history.resourceWritten(boundVertices.Get())==2,
              "vertex mutation invalidates original-VS position history");
        history.advance(402);
        check(history.recordCount()==0 && history.bytes()==0,
              "vertex mutation also releases bounded history allocation");
        check(history.capture(ctx,issue,3,1,0,0,0,402,capture) && capture.candidateCount==0,
              "new capture after vertex mutation has no stale prior candidate");
        ++result.captures;
        check(history.resourceWritten(nullptr)==1,
              "explicit global reset invalidates actual GPU history");
        history.advance(403);
        check(history.recordCount()==0 && history.bytes()==0,
              "global reset releases all GPU history allocations");
        result.records=unsigned(history.recordCount());result.bytes=history.bytes();
        result.retired=3;
        results.push_back(result);
    }

    // The flat adapter has a separate current-draw preflight at the same
    // cap. Split the distinct records across adjacent frames (the seeded
    // frame before, 24 of the current frame) so the late draw reaches the
    // actual allocator instead of failing that adapter guard.
    {
        constexpr unsigned seeded=cap-24;
        std::vector<UINT> indices((cap+1)*3);
        for(unsigned i=0;i<cap+1;++i){indices[i*3]=0;indices[i*3+1]=1;indices[i*3+2]=2;}
        auto ib=flatHistoryPressureIndexBuffer(dev,indices);
        ctx->IASetIndexBuffer(ib.Get(),DXGI_FORMAT_R32_UINT,0);
        FlatForegroundMotion motion;
        FlatHistoryPressureResult result{"adapter_cross_frame_record_budget"};
        for(unsigned i=0;i<seeded;++i) {
            auto inputs=flatHistoryPressureInputs(i+1);
            inputs.beforeWorld=false;
            check(motion.capture(ctx,issue,3,1,i*3,0,0,600,inputs),
                  "adapter seeds actual previous-frame GPU records");
            ++result.captures;
        }
        motion.beginFrame(601);
        for(unsigned i=seeded;i<cap;++i) {
            auto inputs=flatHistoryPressureInputs(i+1);
            inputs.beforeWorld=true;
            check(motion.capture(ctx,issue,3,1,i*3,0,0,601,inputs),
                  "adapter admits current-frame reconstructed pre-world captures");
            ++result.captures;
        }
        auto late=flatHistoryPressureInputs(cap+1);
        late.beforeWorld=false;
        // The refusal is the draw's, not the frame's: the runtime covers the draw per pixel when it has marked it (coverDraw), and makes
        // it the frame's (fail) when it has not.
        check(!motion.capture(ctx,issue,3,1,cap*3,0,0,601,late) && !motion.refusal() &&
              motion.drawRefusal() && !std::strcmp(motion.drawRefusal(),"history-budget"),
              "late reconstructed foreign draw reaches allocator record budget as the draw's refusal, not the frame's");
        const auto stats=motion.stats();
        check(stats.attempts==cap+1 && stats.gpuAttempts==cap+1 && stats.submitted==cap &&
              stats.preflightRefused==0 && stats.warmedAfterRefusal==0,
              "cross-frame allocator refusal is distinct from adapter draw bound");
        const auto receipt=motion.budgetReceipt();
        check(receipt.valid==1 && receipt.records==cap && receipt.bytes==cap*3*32 &&
              receipt.requestedBytes==3*32 && receipt.recordLimitHit==1 && receipt.byteLimitHit==0,
              "first budget receipt identifies record limit and actual retained allocation");
        check(receipt.currentDraws==24 && receipt.previousDraws==seeded &&
              receipt.beforeWorldCurrent==24 && receipt.beforeWorldPrevious==0 &&
              receipt.current==24 && receipt.prior==seeded && receipt.older==0 &&
              receipt.invalid==0 && receipt.pending==0 &&
              receipt.reclaimedRecords==0 && receipt.reclaimedBytes==0 &&
              receipt.knownMutations==0 && receipt.unknownMutations==0,
              "budget receipt preserves current/prior and provisional draw attribution");
        motion.coverDraw(motion.drawRefusal());
        check(!motion.refusal() && motion.coveredDraws()==1 && motion.stats().coveredBudget==1 &&
              motion.stats().coveredOccurrence==0 && motion.stats().coveredOther==0,
              "a covered budget refusal counts once, under its cause, and leaves the frame unrefused");
        result.records=receipt.records;result.bytes=receipt.bytes;result.firstRefusal=motion.drawRefusal();
        results.push_back(result);
        motion.beginFrame(602);
        check(!motion.budgetReceipt().valid && !motion.refusal(),
              "new frame clears first budget receipt and sticky refusal");
        auto bad=flatHistoryPressureInputs(0);
        check(!motion.capture(ctx,issue,3,1,cap*3,0,0,602,bad) &&
              motion.refusal() && !std::strcmp(motion.refusal(),"foreground-writer-token-unavailable") &&
              !motion.budgetReceipt().valid,
              "non-budget preflight refusal cannot fabricate a history receipt");
    }

    // A known write invalidates the prior frame's correspondence, but its
    // records still occupy the helper's budget until the next advance().
    {
        std::vector<UINT> indices(cap*3);
        for(unsigned i=0;i<cap;++i){indices[i*3]=0;indices[i*3+1]=1;indices[i*3+2]=2;}
        auto oldIb=flatHistoryPressureIndexBuffer(dev,indices);
        auto freshIb=flatHistoryPressureIndexBuffer(dev,{0,1,2});
        ctx->IASetIndexBuffer(oldIb.Get(),DXGI_FORMAT_R32_UINT,0);
        FlatForegroundMotion motion;
        FlatHistoryPressureResult result{"adapter_invalidated_prior_occupancy"};
        for(unsigned i=0;i<cap;++i) {
            const auto inputs=flatHistoryPressureInputs(i+1);
            check(motion.capture(ctx,issue,3,1,i*3,0,0,700,inputs),
                  "adapter seeds prior-frame geometry before known write");
            ++result.captures;
        }
        motion.beginFrame(701);
        indices[0]=2;indices[2]=0;
        ctx->UpdateSubresource(oldIb.Get(),0,nullptr,indices.data(),0,0);
        motion.resourceWritten(oldIb.Get());
        check(!motion.refusal(),
              "known write to prior-only geometry does not poison current adapter frame");
        ctx->IASetIndexBuffer(freshIb.Get(),DXGI_FORMAT_R32_UINT,0);
        const auto fresh=flatHistoryPressureInputs(cap+1);
        check(motion.capture(ctx,issue,3,1,0,0,0,701,fresh) && !motion.refusal(),
              "adapter reclaims invalidated prior record for same-frame GPU capture");
        const auto admitted=motion.stats();
        check(admitted.attempts==cap+1 && admitted.gpuAttempts==cap+1 && admitted.submitted==cap+1 &&
              admitted.preflightRefused==0,
              "same-frame reclamation passes adapter and submits actual GPU work");
        ++result.captures;
        motion.beginFrame(702);
        check(!motion.refusal() && motion.capture(ctx,issue,3,1,0,0,0,702,fresh),
              "next frame retains prior fresh geometry after old records retire");
        check(motion.stats().submitted==cap+2,
              "fresh post-retirement draw submits original-VS GPU history");
        ++result.captures;
        result.records=cap;result.bytes=cap*3*32;result.retired=cap;
        results.push_back(result);
    }

    // The caller owns Capture's COM views independently of the helper's
    // records. Keep one live across invalidation and retirement as the VR
    // caller could, and compare its actual GPU position bytes afterward.
    {
        std::vector<UINT> indices(cap*3);
        for(unsigned i=0;i<cap;++i){indices[i*3]=0;indices[i*3+1]=1;indices[i*3+2]=2;}
        auto oldIb=flatHistoryPressureIndexBuffer(dev,indices);
        auto freshIb=flatHistoryPressureIndexBuffer(dev,{0,1,2});
        ctx->IASetIndexBuffer(oldIb.Get(),DXGI_FORMAT_R32_UINT,0);
        AnimatedVertexHistory history;
        AnimatedVertexHistory::Capture retained;
        FlatHistoryPressureResult result{"invalidated_capture_snapshot_lifetime"};
        for(unsigned i=0;i<cap;++i) {
            AnimatedVertexHistory::Capture next;
            check(history.capture(ctx,issue,3,1,i*3,0,0,800,next,true),
                  "direct helper seeds bounded actual GPU records");
            if(i==0)retained=std::move(next);
            ++result.captures;
        }
        const auto before=flatHistoryPressurePositionBytes(dev,ctx,retained.currentPositions.Get());
        check(before.size()==3*16,
              "retained original-VS snapshot contains three actual GPU positions");
        history.advance(801);
        indices[0]=2;indices[2]=0;
        ctx->UpdateSubresource(oldIb.Get(),0,nullptr,indices.data(),0,0);
        check(history.resourceWritten(oldIb.Get())==4 &&
              history.recordCount()==cap && history.bytes()==cap*3*32,
              "known IB write invalidates but retains same-frame occupancy");
        ctx->IASetIndexBuffer(freshIb.Get(),DXGI_FORMAT_R32_UINT,0);
        AnimatedVertexHistory::Capture refused;
        check(history.capture(ctx,issue,3,1,0,0,0,801,refused) && refused.candidateCount==0,
              "fresh helper key reclaims invalidated slot before frame advance");
        ++result.captures;
        const auto pressured=history.accounting(801);
        check(pressured.recordCount==cap && pressured.invalid==cap-1 && pressured.current==1 &&
              pressured.reclaimedRecords==1 && pressured.reclaimedBytes==3*32,
              "record-pressure compaction releases one invalidated slot and stops");
        history.advance(802);
        check(history.recordCount()==1 && history.bytes()==3*32 &&
              history.accounting(802).reclaimedRecords==0,
              "frame advance retires remaining invalidated records");
        check(flatHistoryPressurePositionBytes(dev,ctx,retained.currentPositions.Get())==before &&
              retained.currentIdentity && retained.instanceIndex,
              "caller-owned GPU Capture remains alive and unchanged after record retirement");
        check(history.capture(ctx,issue,3,1,0,0,0,802,refused) &&
              refused.candidateCount==1,
              "fresh geometry retains only its own valid prior candidate");
        ++result.captures;
        result.records=pressured.recordCount;result.bytes=pressured.bytes;result.retired=cap;
        result.priorCandidates=refused.candidateCount;
        results.push_back(result);
    }

    // Pressure compaction shifts an outstanding prepared B and a fresh
    // unpublished D. Both must publish by their owned views, while the erased
    // A's SO program must be reused by the next capture of the same VS.
    {
        bind(Pose{},false);
        ComPtr<ID3D11VertexShader> originalVs;
        ctx->VSGetShader(&originalVs,nullptr,nullptr);
        UINT codeSize=0;
        hr(originalVs->GetPrivateData(AnimatedVertexHistory::bytecodeKey,&codeSize,nullptr));
        std::vector<unsigned char> code(codeSize);
        hr(originalVs->GetPrivateData(AnimatedVertexHistory::bytecodeKey,&codeSize,code.data()));
        ComPtr<ID3D11VertexShader> otherVs;
        hr(dev->CreateVertexShader(code.data(),code.size(),nullptr,&otherVs));
        AnimatedVertexHistory::rememberShader(otherVs.Get(),code.data(),code.size());
        auto aIb=flatHistoryPressureIndexBuffer(dev,{0,1,2});
        auto bIb=flatHistoryPressureIndexBuffer(dev,{0,1,2});
        constexpr unsigned fillers=cap-3;   // with A, B and the pending D, the cap exactly
        std::vector<UINT> fillerIndices(fillers*3);
        for(unsigned i=0;i<fillerIndices.size();++i)fillerIndices[i]=i%3;
        auto fillerIb=flatHistoryPressureIndexBuffer(dev,fillerIndices);
        auto dIb=flatHistoryPressureIndexBuffer(dev,{0,1,2});
        auto cIb=flatHistoryPressureIndexBuffer(dev,{0,1,2});
        AnimatedVertexHistory history;
        AnimatedVertexHistory::Capture captureA,capture;
        FlatHistoryPressureResult result{"outstanding_capture_record_index"};
        ctx->IASetIndexBuffer(aIb.Get(),DXGI_FORMAT_R32_UINT,0);
        check(history.capture(ctx,issue,3,1,0,0,0,900,captureA),
              "earlier A record captures original VS positions on GPU");
        const auto oldSo=flatHistoryPressureStreamOutput(ctx,history,captureA);
        check(bool(oldSo),"A owns an actual original-VS stream-output shader");
        ctx->VSSetShader(otherVs.Get(),nullptr,0);
        ctx->IASetIndexBuffer(bIb.Get(),DXGI_FORMAT_R32_UINT,0);
        check(history.capture(ctx,issue,3,1,0,0,0,900,capture),
              "later B record captures independently from A");
        result.captures=2;
        ctx->IASetIndexBuffer(fillerIb.Get(),DXGI_FORMAT_R32_UINT,0);
        for(unsigned i=0;i<fillers;++i) {
            check(history.capture(ctx,issue,3,1,i*3,0,0,900,capture),
                  "alternate original VS fills bounded prior records");
            ++result.captures;
        }
        history.advance(901);
        AnimatedVertexHistory::Capture pendingB;
        ctx->IASetIndexBuffer(bIb.Get(),DXGI_FORMAT_R32_UINT,0);
        check(history.prepareCapture(ctx,3,1,0,0,0,901,pendingB) &&
              pendingB.candidateCount==1 && history.recordCount()==cap-1,
              "B prepares with one prior candidate while A precedes it in record vector");
        AnimatedVertexHistory::Capture pendingD;
        ctx->IASetIndexBuffer(dIb.Get(),DXGI_FORMAT_R32_UINT,0);
        check(history.prepareCapture(ctx,3,1,0,0,0,901,pendingD) &&
              history.accounting(901).pending==1 && history.recordCount()==cap,
              "fresh D reservation is pending rather than invalidated at record cap");
        const UINT rewrittenA[3]{2,1,0};
        ctx->UpdateSubresource(aIb.Get(),0,nullptr,rewrittenA,0,0);
        check(history.resourceWritten(aIb.Get())==4,
              "known write invalidates only A while B capture is outstanding");
        ctx->VSSetShader(originalVs.Get(),nullptr,0);
        ctx->IASetIndexBuffer(cIb.Get(),DXGI_FORMAT_R32_UINT,0);
        AnimatedVertexHistory::Capture captureC;
        check(history.capture(ctx,issue,3,1,0,0,0,901,captureC),
              "C pressure capture reclaims invalid A without erasing pending D");
        ++result.captures;
        check(flatHistoryPressureStreamOutput(ctx,history,captureC)==oldSo,
              "reclaimed last same-original record reuses its SO program");
        const auto pressured=history.accounting(901);
        check(pressured.recordCount==cap && pressured.invalid==0 && pressured.pending==1 &&
              pressured.current==1 && pressured.prior==cap-2 && pressured.reclaimedRecords==1 &&
              pressured.reclaimedBytes==3*32,
              "compaction reclaims only explicitly invalid A, preserving pending D");
        ctx->VSSetShader(otherVs.Get(),nullptr,0);
        ctx->IASetIndexBuffer(bIb.Get(),DXGI_FORMAT_R32_UINT,0);
        history.submitIdentity(ctx,pendingB);
        history.submitPositions(ctx,issue,0,pendingB);
        ++result.captures;
        ctx->IASetIndexBuffer(dIb.Get(),DXGI_FORMAT_R32_UINT,0);
        history.submitIdentity(ctx,pendingD);
        history.submitPositions(ctx,issue,0,pendingD);
        ++result.captures;
        history.advance(902);
        check(history.recordCount()==cap && history.bytes()==cap*3*32 &&
              history.accounting(902).reclaimedRecords==0,
              "B and D remain while pressure reclamation receipt resets");
        AnimatedVertexHistory::Capture nextB;
        ctx->IASetIndexBuffer(bIb.Get(),DXGI_FORMAT_R32_UINT,0);
        check(history.capture(ctx,issue,3,1,0,0,0,902,nextB) &&
              nextB.candidateCount==1 && nextB.previousPositions[0]==pendingB.currentPositions,
              "shifted B publishes exactly its owned view, not a neighbor's");
        ++result.captures;
        AnimatedVertexHistory::Capture nextD;
        ctx->IASetIndexBuffer(dIb.Get(),DXGI_FORMAT_R32_UINT,0);
        check(history.capture(ctx,issue,3,1,0,0,0,902,nextD) &&
              nextD.candidateCount==1 && nextD.previousPositions[0]==pendingD.currentPositions,
              "unpublished D survives compaction and later publishes its own view");
        ++result.captures;
        result.records=unsigned(history.recordCount());result.bytes=history.bytes();
        result.priorCandidates=nextB.candidateCount+nextD.candidateCount;result.retired=1;
        results.push_back(result);
        ctx->VSSetShader(originalVs.Get(),nullptr,0);
    }

    // Reusing an invalidated exact key must not let an older prepared Capture
    // publish the new reservation's parity after its source geometry changed.
    {
        auto ib=flatHistoryPressureIndexBuffer(dev,{0,1,2});
        ctx->IASetIndexBuffer(ib.Get(),DXGI_FORMAT_R32_UINT,0);
        AnimatedVertexHistory history;
        AnimatedVertexHistory::Capture seeded,stale,fresh,next;
        FlatHistoryPressureResult result{"stale_capture_epoch_guard"};
        check(history.capture(ctx,issue,3,1,0,0,0,950,seeded),
              "old exact key captures before mutation");
        ++result.captures;
        history.advance(951);
        check(history.prepareCapture(ctx,3,1,0,0,0,951,stale) && stale.candidateCount==1,
              "old capture prepares a prior candidate before known write");
        const UINT rewritten[3]{2,1,0};
        ctx->UpdateSubresource(ib.Get(),0,nullptr,rewritten,0,0);
        check(history.resourceWritten(ib.Get())==4 && history.accounting(951).invalid==1,
              "known write invalidates outstanding exact-key reservation");
        check(history.prepareCapture(ctx,3,1,0,0,0,951,fresh) &&
              fresh.candidateCount==0 && history.accounting(951).pending==1,
              "new exact-key reservation reuses allocation without prior history");
        history.submitIdentity(ctx,stale);
        history.submitPositions(ctx,issue,0,stale);
        history.advance(952);
        check(history.recordCount()==0 && history.bytes()==0,
              "stale prepared capture cannot republish invalidated allocation");
        check(history.capture(ctx,issue,3,1,0,0,0,952,next) && next.candidateCount==0,
              "new GPU capture sees no fabricated prior from stale reservation");
        ++result.captures;
        result.records=unsigned(history.recordCount());result.bytes=history.bytes();
        result.retired=1;
        results.push_back(result);
    }

    {
        std::vector<UINT> indices{0,1,2};
        auto ib=flatHistoryPressureIndexBuffer(dev,indices);
        ctx->IASetIndexBuffer(ib.Get(),DXGI_FORMAT_R32_UINT,0);
        AnimatedVertexHistory history;
        FlatHistoryPressureResult result{"duplicate_occurrence_cap"};
        for(unsigned i=0;i<5;++i) {
            AnimatedVertexHistory::Capture capture;
            const bool admitted=history.capture(ctx,issue,3,1,0,0,0,500,capture);
            if(i<4){check(admitted,"four real same-frame GPU occurrences captured");++result.captures;}
            else {
                check(!admitted && capture.refusal && !std::strcmp(capture.refusal,"occurrence-cap"),
                      "fifth duplicate refuses by occurrence cap before history budget");
                result.firstRefusal=capture.refusal;
            }
        }
        result.records=unsigned(history.recordCount());result.bytes=history.bytes();
        check(result.records==4 && result.bytes==4*3*32,
              "duplicate occurrence cap is independent of global record and byte caps");
        history.advance(501);
        AnimatedVertexHistory::Capture prior;
        check(history.capture(ctx,issue,3,1,0,0,0,501,prior) && prior.candidateCount==4,
              "next frame exposes four real prior occurrence candidates");
        result.priorCandidates=prior.candidateCount;
        results.push_back(result);
    }

    // THE GRENADE HOLD (design section 104). Holding a grenade drew one mesh dozens of times in a frame (the log's refusal inventory: 28
    // draws of one key refused for the occurrence cap, 10 of another), and the history refused every one beyond the fourth. The extended
    // policy, the flat adapter's, allows 64 draws of a key and hands each the window of four priors nearest its own ordinal; the GPU map
    // still matches by identity inside the window. The default policy (VR) is what duplicate_occurrence_cap pins and is unchanged.
    {
        std::vector<UINT> indices{0,1,2};
        auto ib=flatHistoryPressureIndexBuffer(dev,indices);
        ctx->IASetIndexBuffer(ib.Get(),DXGI_FORMAT_R32_UINT,0);
        FlatHistoryPressureResult result{"extended_occurrence_window"};
        constexpr unsigned pieces=9;
        {
            AnimatedVertexHistory plain;
            for(unsigned i=0;i<5;++i) {
                AnimatedVertexHistory::Capture capture;
                const bool admitted=plain.capture(ctx,issue,3,1,0,0,0,1000,capture);
                check(admitted==(i<4),"the default policy still admits four draws of one key and refuses the fifth");
            }
        }
        AnimatedVertexHistory history;
        std::vector<AnimatedVertexHistory::Capture> first(pieces);
        for(unsigned i=0;i<pieces;++i) {
            check(history.capture(ctx,issue,3,1,0,0,0,1000,first[i],false,true) && first[i].occurrences==i,
                  "the extended policy admits more than four draws of one key in a frame, each its own record");
            ++result.captures;
        }
        check(history.recordCount()==pieces && history.bytes()==pieces*3*32,
              "nine draws of one key hold nine records of their own");
        history.advance(1001);
        for(unsigned k=0;k<pieces;++k) {
            AnimatedVertexHistory::Capture next;
            check(history.capture(ctx,issue,3,1,0,0,0,1001,next,false,true) && next.occurrences==k && next.priorRecords==pieces &&
                  next.candidateCount==4,"the frame after has nine prior records and hands each draw a window of four");
            const unsigned firstOfWindow=(std::min)(k>0?k-1:0u,pieces-4);
            check(next.previousPositions[k-firstOfWindow]==first[k].currentPositions &&
                  next.previousIdentity[k-firstOfWindow]==first[k].currentIdentity,
                  "the window holds the draw's own previous record at its own ordinal");
            result.priorCandidates=next.candidateCount;
            ++result.captures;
        }
        {
            AnimatedVertexHistory limited;
            for(unsigned i=0;i<AnimatedVertexHistory::maxExtendedOccurrences+1;++i) {
                AnimatedVertexHistory::Capture capture;
                const bool admitted=limited.capture(ctx,issue,3,1,0,0,0,1100,capture,false,true);
                if(i<AnimatedVertexHistory::maxExtendedOccurrences)check(admitted,"sixty-four draws of one key are admitted by the extended policy");
                else {
                    check(!admitted && capture.refusal && !std::strcmp(capture.refusal,"occurrence-cap"),
                          "the sixty-fifth draw of one key is refused by the occurrence cap");
                    result.firstRefusal=capture.refusal;
                }
            }
            check(limited.recordCount()==AnimatedVertexHistory::maxExtendedOccurrences,"the refused draw allocates nothing");
        }
        result.records=unsigned(history.recordCount());result.bytes=history.bytes();
        results.push_back(result);
    }

    // A record that no later frame can use as a prior is reclaimable under budget pressure. The log's budget receipt (frame 39036): 128
    // records, 122 of them last used two frames before and none this frame, 15 percent of the bytes: the first draw of the frame was refused
    // for the record limit. Under the extended policy those records give way to the fresh key; under the default policy they do not.
    {
        std::vector<UINT> indices((cap+1)*3);
        for(unsigned i=0;i<cap+1;++i){indices[i*3]=0;indices[i*3+1]=1;indices[i*3+2]=2;}
        auto ib=flatHistoryPressureIndexBuffer(dev,indices);
        ctx->IASetIndexBuffer(ib.Get(),DXGI_FORMAT_R32_UINT,0);
        FlatHistoryPressureResult result{"extended_spent_record_reclaim"};
        auto seeded=[&](AnimatedVertexHistory& history,unsigned frame) {
            for(unsigned i=0;i<cap;++i) {
                AnimatedVertexHistory::Capture capture;
                check(history.capture(ctx,issue,3,1,i*3,0,0,frame,capture,false,true),"the record budget fills with distinct keys");
                ++result.captures;
            }
            check(history.recordCount()==cap,"the history holds the full record budget");
        };
        AnimatedVertexHistory plain,extended;
        seeded(plain,1200);seeded(extended,1200);
        plain.advance(1201);extended.advance(1201);plain.advance(1202);extended.advance(1202);
        check(extended.recordCount()==cap && extended.accounting(1202).older==cap,
              "two frames without a draw leave every record spent but not yet retired (age two)");
        AnimatedVertexHistory::Capture refused,admitted;
        check(!plain.capture(ctx,issue,3,1,cap*3,0,0,1202,refused,false,false) && refused.refusal &&
              !std::strcmp(refused.refusal,"history-budget"),
              "the default policy refuses a fresh key at the record limit although every record is spent");
        result.firstRefusal=refused.refusal;
        check(extended.capture(ctx,issue,3,1,cap*3,0,0,1202,admitted,false,true) && admitted.candidateCount==0,
              "the extended policy reclaims a spent record for the fresh key");
        const auto usage=extended.accounting(1202);
        check(usage.recordCount==cap && usage.current==1 && usage.older==cap-1 && usage.reclaimedRecords==1 &&
              usage.reclaimedBytes==3*32,"one spent record was reclaimed, and no more than the fresh key needed");
        // A record the frame before used is a prior some draw of this frame may still need: never reclaimed.
        AnimatedVertexHistory live;
        seeded(live,1300);live.advance(1301);
        AnimatedVertexHistory::Capture lateRefused;
        check(!live.capture(ctx,issue,3,1,cap*3,0,0,1301,lateRefused,false,true) && lateRefused.refusal &&
              !std::strcmp(lateRefused.refusal,"history-budget") && live.accounting(1301).reclaimedRecords==0,
              "records the frame before used are priors and are not reclaimed: the fresh key is refused");
        result.records=usage.recordCount;result.bytes=usage.bytes;result.retired=1;
        results.push_back(result);
    }

    // The adapter: a draw refused its capture is the draw's refusal, not the frame's. The caller covers it per pixel when it marked the
    // draw (coverDraw) and makes it the frame's (fail) when it did not. Real draws of one key past the cap are the refused ones here.
    {
        std::vector<UINT> indices{0,1,2};
        auto ib=flatHistoryPressureIndexBuffer(dev,indices);
        ctx->IASetIndexBuffer(ib.Get(),DXGI_FORMAT_R32_UINT,0);
        FlatForegroundMotion motion;
        FlatHistoryPressureResult result{"adapter_covered_refusals"};
        unsigned token=0;
        for(unsigned i=0;i<AnimatedVertexHistory::maxExtendedOccurrences;++i) {
            check(motion.capture(ctx,issue,3,1,0,0,0,1400,flatHistoryPressureInputs(++token)) && !motion.drawRefusal(),
                  "the adapter admits sixty-four draws of one key, as the flat policy allows");
            ++result.captures;
        }
        check(!motion.capture(ctx,issue,3,1,0,0,0,1400,flatHistoryPressureInputs(++token)) && !motion.refusal() &&
              motion.drawRefusal() && !std::strcmp(motion.drawRefusal(),"occurrence-cap"),
              "the draw past the cap is refused as the draw's refusal, and the frame stands unrefused");
        result.firstRefusal=motion.drawRefusal();
        motion.coverDraw(motion.drawRefusal());
        check(motion.coveredDraws()==1 && motion.stats().coveredOccurrence==1 && motion.stats().coveredBudget==0 &&
              motion.stats().coveredOther==0 && !motion.refusal(),"a covered occurrence-cap refusal is counted under its cause and refuses nothing");
        // A refusal does not poison the draws after it: a different key still captures.
        ctx->IASetIndexBuffer(flatHistoryPressureIndexBuffer(dev,{0,1,2,0,1,2}).Get(),DXGI_FORMAT_R32_UINT,0);
        check(motion.capture(ctx,issue,3,1,3,0,0,1400,flatHistoryPressureInputs(++token)) && !motion.drawRefusal() && !motion.refusal(),
              "a draw after a covered refusal captures as usual");
        // The same refusal with no owner mark to cover it is the frame's, as every capture failure once was.
        motion.beginFrame(1401);
        ctx->IASetIndexBuffer(ib.Get(),DXGI_FORMAT_R32_UINT,0);
        for(unsigned i=0;i<AnimatedVertexHistory::maxExtendedOccurrences;++i)
            check(motion.capture(ctx,issue,3,1,0,0,0,1401,flatHistoryPressureInputs(100+i)),"the next frame admits the same sixty-four");
        check(!motion.capture(ctx,issue,3,1,0,0,0,1401,flatHistoryPressureInputs(200)) && motion.drawRefusal() && !motion.refusal(),
              "the sixty-fifth is the draw's refusal again");
        motion.fail(motion.drawRefusal());
        check(motion.refusal() && !std::strcmp(motion.refusal(),"occurrence-cap") && motion.coveredDraws()==0,
              "uncovered, the refusal is the frame's: prepareH refuses while it stands");
        // The frame's own refusals do not defer: a draw that no owner mark could ever cover is the frame's at once.
        motion.beginFrame(1402);
        auto bad=flatHistoryPressureInputs(0);
        check(!motion.capture(ctx,issue,3,1,0,0,0,1402,bad) && motion.refusal() &&
              !std::strcmp(motion.refusal(),"foreground-writer-token-unavailable") && !motion.drawRefusal(),
              "a draw with no writer token the owner plane could carry is the frame's refusal, never deferred");
        results.push_back(result);
    }

    if(printJson) {
        std::printf("EDVR_BENCH_HISTORY_RESULT {\"schema\":\"edvr-flat-sdk-history-bench\",\"version\":1,\"verdict\":\"PASS\",\"provenance\":\"reconstructed GPU workload using production AnimatedVertexHistory\",\"cases\":[");
        for(size_t i=0;i<results.size();++i) {
            const auto& r=results[i];
            std::printf("%s{\"name\":\"%s\",\"observed\":{\"records\":%u,\"bytes\":%u,\"admissions\":%u,\"firstRefusal\":\"%s\",\"priorCandidates\":%u,\"retired\":%u},\"passed\":true}",
                        i?",":"",r.name,r.records,r.bytes,r.captures,r.firstRefusal,r.priorCandidates,r.retired);
        }
        std::printf("]}\n");
    }
    return results;
}
