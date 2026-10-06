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

template<class Bind>
inline std::vector<FlatHistoryPressureResult> flatBenchHistoryPressureTests(
    ID3D11Device* dev,ID3D11DeviceContext* ctx,Bind bind,bool printJson) {
    std::vector<FlatHistoryPressureResult> results;
    ctx->ClearState();
    bind(Pose{},false);

    // 64 small, distinct geometries are provisionally captured before any
    // caller names a source. The 65th is labelled genuine foreign by this
    // workload, but only a caller could reserve space for it.
    {
        std::vector<UINT> indices(65*3);
        for(unsigned i=0;i<65;++i){indices[i*3]=0;indices[i*3+1]=1;indices[i*3+2]=2;}
        auto ib=flatHistoryPressureIndexBuffer(dev,indices);
        ctx->IASetIndexBuffer(ib.Get(),DXGI_FORMAT_R32_UINT,0);
        AnimatedVertexHistory history;
        FlatHistoryPressureResult result{"provisional_before_foreign"};
        for(unsigned i=0;i<65;++i) {
            AnimatedVertexHistory::Capture capture;
            const bool admitted=history.capture(ctx,issue,3,1,i*3,0,0,100,capture);
            if(i<AnimatedVertexHistory::maxRecords) {
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
        check(result.records==64 && result.bytes==64*3*32,
              "record pressure reports actual retained GPU-history allocation");
        history.advance(101);
        AnimatedVertexHistory::Capture reused;
        check(history.capture(ctx,issue,3,1,0,0,0,101,reused) && reused.candidateCount==1,
              "previous-frame indexed geometry reuses real GPU history at the record cap");
        result.priorCandidates=reused.candidateCount;
        check(history.recordCount()==64 && history.bytes()==result.bytes,
              "history reuse does not allocate a 65th record");
        history.advance(104);
        check(history.recordCount()==0 && history.bytes()==0,
              "two absent frames retire the full provisional record budget");
        result.retired=64;
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
              "byte pressure measures real allocation before 64-record limit");
        results.push_back(result);
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
