#pragma once
#include <vector>
#include <thread>
#include <cstring>

// Real COM query allocation and issuance, with deterministic asynchronous
// readback/failure injection. Separate default-driver tests use actual GetData.
struct Ops {
    struct Query { ID3D11Query* ptr; D3D11_QUERY kind; uint64_t tick=0; bool open=false; };
    std::vector<Query> queries;
    unsigned creates=0,releases=0,commands=0,active=0;
    int failCreate=-1;
    bool pending=false,failed=false,disjoint=false,zeroFrequency=false,reverseTicks=false;
    bool failBegin=false,failEnd=false,failStamp=false,bad=false;
    uint64_t tick=0;
    Query* find(ID3D11Asynchronous* q) { for(auto& x:queries)if(x.ptr==q)return &x;bad=true;return nullptr; }
    static HRESULT create(void* p,ID3D11Device* d,const D3D11_QUERY_DESC* desc,ID3D11Query** out) {
        auto& s=*static_cast<Ops*>(p);const auto n=s.creates++;*out=nullptr;
        if(int(n)==s.failCreate)return E_OUTOFMEMORY;
        const auto r=d->CreateQuery(desc,out);
        if(r==S_OK&&*out)s.queries.push_back({*out,desc->Query});
        return r;
    }
    static HRESULT begin(void* p,ID3D11DeviceContext* c,ID3D11Asynchronous* q) {
        auto& s=*static_cast<Ops*>(p);++s.commands;auto* x=s.find(q);
        if(!x||s.active||x->kind!=D3D11_QUERY_TIMESTAMP_DISJOINT){s.bad=true;return E_FAIL;}
        if(s.failBegin)return E_FAIL;
        ++s.active;x->open=true;c->Begin(q);return S_OK;
    }
    static HRESULT end(void* p,ID3D11DeviceContext* c,ID3D11Asynchronous* q) {
        auto& s=*static_cast<Ops*>(p);++s.commands;auto* x=s.find(q);
        if(!x||s.active!=1){s.bad=true;return E_FAIL;}
        const bool dj=x->kind==D3D11_QUERY_TIMESTAMP_DISJOINT;
        if(dj){if(!x->open)s.bad=true;x->open=false;--s.active;}
        x->tick=++s.tick;c->End(q);
        return (dj?s.failEnd:s.failStamp)?E_FAIL:S_OK;
    }
    static HRESULT data(void* p,ID3D11DeviceContext*,ID3D11Asynchronous* q,void* out,UINT size,UINT flags) {
        auto& s=*static_cast<Ops*>(p);++s.commands;auto* x=s.find(q);
        if(!x||x->open||flags!=D3D11_ASYNC_GETDATA_DONOTFLUSH){s.bad=true;return E_FAIL;}
        if(s.pending)return S_FALSE;
        if(s.failed)return E_FAIL;
        if(x->kind==D3D11_QUERY_TIMESTAMP_DISJOINT){
            if(size!=sizeof(D3D11_QUERY_DATA_TIMESTAMP_DISJOINT)){s.bad=true;return E_FAIL;}
            *static_cast<D3D11_QUERY_DATA_TIMESTAMP_DISJOINT*>(out)={s.zeroFrequency?0u:1000u,s.disjoint?TRUE:FALSE};
        }else{
            if(size!=sizeof(UINT64)){s.bad=true;return E_FAIL;}
            *static_cast<UINT64*>(out)=s.reverseTicks?100000-x->tick:x->tick;
        }
        return S_OK;
    }
    static void release(void* p,ID3D11Query* q) noexcept {
        auto& s=*static_cast<Ops*>(p);auto* x=s.find(q);
        if(x){if(x->open)--s.active;x->ptr=nullptr;}
        ++s.releases;q->Release();
    }
    edvr::GpuSpanD3D11Ops callbacks(){return {this,create,begin,end,data,release};}
};
using DeviceTiming=edvr::openxr::DeviceGpuTiming;
inline void phases(DeviceTiming& t,ID3D11DeviceContext* c,bool reverse=false) {
    const unsigned forward[]={0,1,2,3},backward[]={1,0,3,2};
    for(unsigned i=0;i<4;++i){auto p=reverse?backward[i]:forward[i];t.beginGpuWork(p,c);t.endGpuWork(p,c);}
}
inline void collectorCases(ID3D11Device* d,ID3D11DeviceContext* c) {
    EdvrNativeDeviceGpuSample out[8]{};
    {
        Ops o;DeviceTiming t;check(t.initialize(d,c,o.callbacks()),"typed collector initializes");
        const auto initial=o.commands;check(t.beginFrame(1,true)&&o.commands==initial,"frame begin has no context commands");
        phases(t,c,true);const auto issued=o.commands;
        check(t.poll(out,8)==0&&o.commands==issued,"unaccepted sample not polled");
        check(t.acceptFrame(1)&&o.commands==issued,"accept is CPU-only");
        o.pending=true;check(t.poll(out,8)==0,"S_FALSE stays pending");
        auto before=o.commands;check(t.beginFrame(2,true)&&o.commands==before,"next frame preserves pending without D3D work");
        phases(t,c);check(t.acceptFrame(2),"second accepted");
        o.pending=false;before=o.commands;check(t.poll(out,0)==0&&o.commands==before,"zero capacity does not poll");
        check(t.poll(out,1)==1&&out[0].sequence==1&&out[0].status==EdvrNativeGpuValid,"reverse eye sample survives next frame");
        check(out[0].transferMs[0]==1.0&&out[0].transferMs[1]==1.0&&out[0].composeMs[0]==1.0&&out[0].composeMs[1]==1.0,"four distinct elapsed intervals");
        check(t.poll(out,8)==1&&out[0].sequence==2,"capacity preserves undelivered sample");
        const auto allocations=o.creates;check(t.beginFrame(3,true),"reuse frame");phases(t,c);t.acceptFrame(3);
        check(t.poll(out,8)==1&&o.creates==allocations,"ready query objects reused");
        check(!t.beginFrame(3,true)&&!t.beginFrame(2,true),"duplicate and old sequence rejected");
        check(!o.bad&&o.active==0,"single disjoint scope and DONOTFLUSH validated");
        before=o.commands;t.abandon();check(o.commands==before&&o.releases==o.queries.size(),"completed abandon releases only");
    }
    {
        Ops o;DeviceTiming t;t.initialize(d,c,o.callbacks());t.beginFrame(1,true);t.beginGpuWork(0,c);
        auto before=o.commands;t.invalidate();check(o.commands==before,"invalidate does not close query on CPU");
        t.beginFrame(2,false);check(o.commands==before,"disable does not close query on CPU");
        t.beginGpuWork(0,c);check(o.active==0,"next real boundary closes cancelled scope even disabled");
        before=o.commands;t.acceptFrame(2);check(o.commands==before,"disabled accept CPU-only");
        check(t.poll(out,8)==1&&out[0].status==EdvrNativeGpuDisabled&&o.commands==before,"disabled status needs no readback");
        t.beginFrame(3,true);before=o.commands;
        std::thread foreign([&]{t.beginGpuWork(0,c);t.endGpuWork(0,c);check(t.poll(out,8)==0,"foreign-thread poll refused");});foreign.join();
        t.beginGpuWork(0,nullptr);t.endGpuWork(0,nullptr);
        check(o.commands==before,"wrong owner/context issue no commands");
        phases(t,c);t.acceptFrame(3);check(t.poll(out,8)==1&&out[0].status==EdvrNativeGpuValid,"re-enable measures fresh work");
        t.beginFrame(4,true);t.beginGpuWork(0,c);t.endGpuWork(0,c);t.beginGpuWork(0,c);t.endGpuWork(0,c);t.acceptFrame(4);
        check(t.poll(out,8)==1&&out[0].status==EdvrNativeGpuIncomplete&&!o.bad,"duplicate phase invalidates only that frame");
        t.beginFrame(5,true);phases(t,c);t.acceptFrame(5);check(t.poll(out,8)==1&&out[0].status==EdvrNativeGpuValid,"malformed frame does not disable future timing");
        t.beginFrame(6,true);t.beginGpuWork(0,c);before=o.commands;t.acceptFrame(6);
        check(o.commands==before,"partial accept CPU-only");
        check(t.poll(out,8)==1&&out[0].status==EdvrNativeGpuIncomplete&&!o.active,"partial pair cannot become valid");
    }
    for(unsigned failure=0;failure<7;++failure){
        Ops o;DeviceTiming t;t.initialize(d,c,o.callbacks());t.beginFrame(1,true);
        o.failed=failure==0;o.disjoint=failure==1;o.zeroFrequency=failure==2;o.reverseTicks=failure==3;
        o.failBegin=failure==4;o.failEnd=failure==5;o.failStamp=failure==6;
        phases(t,c);t.acceptFrame(1);
        check(t.poll(out,8)==1&&out[0].status==EdvrNativeGpuQueryFailure,"bad query result never becomes valid");
        o.failed=o.disjoint=o.zeroFrequency=o.reverseTicks=o.failBegin=o.failEnd=o.failStamp=false;
        auto before=o.commands;t.beginFrame(2,true);phases(t,c);t.acceptFrame(2);
        check(t.poll(out,8)==1&&out[0].status==(failure==5?EdvrNativeGpuQueryFailure:EdvrNativeGpuValid),"only uncertain disjoint End permanently stops timing");
        if(failure==5)check(o.commands==before,"failed End never retried and no new query commands");
        check(!o.bad&&!o.active,"failure lifecycle remains balanced");
    }
    for(int fail=0;fail<9;++fail){
        Ops o;o.failCreate=fail;DeviceTiming t;t.initialize(d,c,o.callbacks());t.beginFrame(1,true);phases(t,c);t.acceptFrame(1);
        check(t.poll(out,8)==1&&out[0].status==EdvrNativeGpuQueryFailure,"partial allocation reports unavailable");
        check(o.releases==o.queries.size(),"partial allocation releases every query");
        o.failCreate=-1;t.beginFrame(2,true);phases(t,c);t.acceptFrame(2);
        check(t.poll(out,8)==1&&out[0].status==EdvrNativeGpuValid,"allocation failure recovers next frame");
    }
    {
        Ops o;o.pending=true;DeviceTiming t;t.initialize(d,c,o.callbacks());
        for(uint64_t seq=1;seq<=8;++seq){t.beginFrame(seq,true);phases(t,c);t.acceptFrame(seq);}
        const auto allocations=o.creates;t.beginFrame(9,true);phases(t,c);t.acceptFrame(9);
        check(t.poll(out,8)==1&&out[0].sequence==9&&out[0].status==EdvrNativeGpuIncomplete&&o.creates==allocations,"ring pressure bounded and reported");
        o.pending=false;t.beginFrame(10,true);phases(t,c);t.acceptFrame(10);
        check(t.poll(out,8)==1&&out[0].status==EdvrNativeGpuValid,"ring exhaustion recovers");
        t.beginFrame(11,true);phases(t,c);t.acceptFrame(11);
        // Cross the collector's actual clock deadline. A nominal sleep alone
        // can leave GetTickCount64 at the inclusive 2000 ms freshness limit.
        const auto staleAfter=GetTickCount64()+2001;
        while(GetTickCount64()<=staleAfter)Sleep(1);
        check(t.poll(out,8)==1&&out[0].status==EdvrNativeGpuStale,"old completion retains age and becomes stale");
    }
}
