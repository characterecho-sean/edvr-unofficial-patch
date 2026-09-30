#pragma once
// Actual production cache/binding functions, injected COM failure boundaries.
// Real WARP context commands execute before the one-shot setter/query faults.
namespace holoFailure {
using namespace edvr::ui_holo_remap;
enum class Site {Good,GetDevice,CreateFail,CreatePartialFail,CreateFault,CreateNull,
    BufferFail,BufferPartialFail,BufferFault,BufferNull,GetShader,GetBuffer,Update,SetBuffer,SetShader,Classes,
    BeforeShader,BeforeBuffer,PersistentBeforeShader,PersistentBeforeBuffer,PersistentBeforeBoth};
struct Child {void** table;ULONG refs=0;};
inline ULONG STDMETHODCALLTYPE childRelease(Child* p){return --p->refs;}
struct Device {void** table;ULONG refs=1;Site site=Site::Good;Child ps{},cb{};unsigned creates=0,buffers=0;};
inline ULONG STDMETHODCALLTYPE add(Device* d){return ++d->refs;}
inline ULONG STDMETHODCALLTYPE drop(Device* d){return --d->refs;}
inline void fault(){RaiseException(0xe0421313,0,0,nullptr);}
inline HRESULT STDMETHODCALLTYPE createPs(Device* d,const void* bytes,SIZE_T n,ID3D11ClassLinkage* linkage,ID3D11PixelShader** out){
    ++d->creates;check(bytes&&n>64&&!linkage,"production creation consumes verified DXBC without linkage");*out=nullptr;
    if(d->site==Site::CreateFail)return E_FAIL;if(d->site==Site::CreateNull)return S_OK;
    d->ps.refs=1;*out=reinterpret_cast<ID3D11PixelShader*>(&d->ps);
    if(d->site==Site::CreateFault)fault();return d->site==Site::CreatePartialFail?E_FAIL:S_OK;
}
inline HRESULT STDMETHODCALLTYPE createCb(Device* d,const D3D11_BUFFER_DESC* desc,const D3D11_SUBRESOURCE_DATA*,ID3D11Buffer** out){
    ++d->buffers;check(desc->ByteWidth==16&&desc->BindFlags==D3D11_BIND_CONSTANT_BUFFER,"bounded per-draw float4 constants");*out=nullptr;
    if(d->site==Site::BufferFail)return E_FAIL;if(d->site==Site::BufferNull)return S_OK;
    d->cb.refs=1;*out=reinterpret_cast<ID3D11Buffer*>(&d->cb);
    if(d->site==Site::BufferFault)fault();return d->site==Site::BufferPartialFail?E_FAIL:S_OK;
}
struct Context {void** table;Device* d=nullptr;ID3D11DeviceContext* real=nullptr;Site site=Site::Good;Child classes[2]{};unsigned setters=0;};
inline void STDMETHODCALLTYPE getDevice(Context* c,ID3D11Device** out){*out=reinterpret_cast<ID3D11Device*>(c->d);add(c->d);if(c->d->site==Site::GetDevice)fault();}
inline void once(Context* c,Site s){if(c->site==s){c->site=Site::Good;fault();}}
inline void STDMETHODCALLTYPE getShader(Context* c,ID3D11PixelShader** ps,ID3D11ClassInstance** a,UINT* n){
    c->real->PSGetShader(ps,a,n);
    if(c->site==Site::Classes&&a&&n&&*n==0){for(unsigned i=0;i<2;++i){++c->classes[i].refs;a[i]=reinterpret_cast<ID3D11ClassInstance*>(&c->classes[i]);}*n=2;}
    once(c,Site::GetShader);
}
inline void STDMETHODCALLTYPE getCb(Context* c,UINT s,UINT n,ID3D11Buffer** b){c->real->PSGetConstantBuffers(s,n,b);once(c,Site::GetBuffer);}
inline void STDMETHODCALLTYPE update(Context* c,ID3D11Resource* r,UINT s,const D3D11_BOX* b,const void* data,UINT row,UINT slice){c->real->UpdateSubresource(r,s,b,data,row,slice);once(c,Site::Update);}
inline void STDMETHODCALLTYPE setCb(Context* c,UINT s,UINT n,ID3D11Buffer*const* b){++c->setters;once(c,Site::BeforeBuffer);if(c->site==Site::PersistentBeforeBuffer||c->site==Site::PersistentBeforeBoth)fault();c->real->PSSetConstantBuffers(s,n,b);once(c,Site::SetBuffer);}
inline void STDMETHODCALLTYPE setPs(Context* c,ID3D11PixelShader* ps,ID3D11ClassInstance*const* a,UINT n){++c->setters;once(c,Site::BeforeShader);if(c->site==Site::PersistentBeforeShader||c->site==Site::PersistentBeforeBoth)fault();c->real->PSSetShader(ps,a,n);once(c,Site::SetShader);}

// Binding::settle and settleAtBoundary: the frame-boundary catch-up for a restore() that failed twice. Real WARP
// state; the fake context injects persistent setter faults and one-shot getter faults exactly as above.
// The public reference count is the leak meter: binding a shader or buffer adds none, the saved references do.
inline ULONG refsOf(IUnknown* u){u->AddRef();return u->Release();}
inline void settleCases(void** ctxV,ID3D11DeviceContext* real,ID3D11PixelShader* original,ID3D11PixelShader* patched,
                        ID3D11Buffer* constants,ID3D11Buffer* oldCb,const std::vector<BYTE>& bytes){
    ComPtr<ID3D11Device> dev;real->GetDevice(&dev);
    ComPtr<ID3D11PixelShader> gamePs;ck(dev->CreatePixelShader(bytes.data(),bytes.size(),nullptr,&gamePs));
    D3D11_BUFFER_DESC bd{};bd.ByteWidth=16;bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
    ComPtr<ID3D11Buffer> b13,gameCb;ck(dev->CreateBuffer(&bd,nullptr,&b13));ck(dev->CreateBuffer(&bd,nullptr,&gameCb));
    auto fake=[](Context& c){return reinterpret_cast<ID3D11DeviceContext*>(&c);};
    auto game=[&](ID3D11PixelShader* ps,ID3D11Buffer* cb){real->PSSetShader(ps,nullptr,0);real->PSSetConstantBuffers(13,1,&cb);};
    auto bound=[&](ID3D11PixelShader* ps,ID3D11Buffer* cb){ComPtr<ID3D11PixelShader> p;ComPtr<ID3D11Buffer> b;
        real->PSGetShader(&p,nullptr,nullptr);real->PSGetConstantBuffers(13,1,&b);return p.Get()==ps&&b.Get()==cb;};
    game(original,b13.Get());
    const ULONG psRefs=refsOf(original),cbRefs=refsOf(b13.Get()),patchedRefs=refsOf(patched),constantsRefs=refsOf(constants);
    auto quiet=[&]{return refsOf(original)==psRefs&&refsOf(b13.Get())==cbRefs&&refsOf(patched)==patchedRefs&&refsOf(constants)==constantsRefs;};
    // EDVR's patched PS and constants bound over the game's original PS and b13, both restore attempts having
    // faulted: the state a failed finish() leaves, the saved references retained.
    auto strand=[&](Context& context,Binding& binding){
        game(original,b13.Get());
        check(binding.begin(fake(context),patched,constants,{.4f,.4f,0,0},directSetPs,original),"settle scene: EDVR's PS and b13 bound over the game's");
        context.site=Site::PersistentBeforeBoth;const auto result=binding.finish(fake(context));context.site=Site::Good;
        check(result.retried&&!result.restored&&binding.needsRestore(),"settle scene: a restore that failed twice retains the saved state");
        check(bound(patched,constants)&&refsOf(original)==psRefs+1&&refsOf(b13.Get())==cbRefs+1,"settle scene: EDVR's PS and b13 still bound, the saved references retained");
    };
    {   // nothing to settle
        Binding idle;check(idle.settle(nullptr)&&!idle.needsRestore(),"settle: a binding that never moved anything settles trivially");
        Context context{ctxV,nullptr,real};Binding binding;strand(context,binding);
        check(!binding.settle(nullptr)&&binding.needsRestore()&&bound(patched,constants),"settle: no context reports failure and retains the state");
        check(binding.settle(fake(context))&&quiet(),"settle: the retained state settles once a context is given");
        const unsigned setters=context.setters;
        check(binding.settle(fake(context))&&context.setters==setters,"settle: a settled binding is idempotent and issues nothing");
    }
    {   // both slots still EDVR's: put back
        Context context{ctxV,nullptr,real};Binding binding;strand(context,binding);
        check(binding.settle(fake(context)),"settle: slots that still hold EDVR's objects are put back");
        check(bound(original,b13.Get())&&!binding.needsRestore()&&quiet(),"settle: the game's PS and b13 are back, the saved references released");
    }
    {   // the game rebound both since: left alone, saved references released
        Context context{ctxV,nullptr,real};Binding binding;strand(context,binding);
        game(gamePs.Get(),gameCb.Get());const unsigned setters=context.setters;
        check(binding.settle(fake(context)),"settle: slots the game rebound hold the game's own state already");
        check(context.setters==setters&&bound(gamePs.Get(),gameCb.Get()),"settle: the game's own rebinding is left alone, no setter runs");
        check(!binding.needsRestore()&&quiet(),"settle: a rebound slot's saved reference is released");
    }
    {   // one slot rebound, the other still EDVR's, both ways round
        Context context{ctxV,nullptr,real};Binding binding;strand(context,binding);
        real->PSSetShader(gamePs.Get(),nullptr,0);
        check(binding.settle(fake(context))&&bound(gamePs.Get(),b13.Get())&&!binding.needsRestore()&&quiet(),"settle: a rebound PS stays, b13 still EDVR's goes back");
        Binding other;strand(context,other);
        real->PSSetConstantBuffers(13,1,gameCb.GetAddressOf());
        check(other.settle(fake(context))&&bound(original,gameCb.Get())&&!other.needsRestore()&&quiet(),"settle: a rebound b13 stays, PS still EDVR's goes back");
    }
    {   // setters that fault: report false, keep the references and the state
        Context context{ctxV,nullptr,real};Binding binding;strand(context,binding);
        context.site=Site::PersistentBeforeBoth;
        check(!binding.settle(fake(context)),"settle: faulting setters report failure");
        check(binding.needsRestore()&&bound(patched,constants)&&refsOf(original)==psRefs+1&&refsOf(b13.Get())==cbRefs+1,"settle: failed setters retain the references and change nothing");
        context.site=Site::PersistentBeforeShader;   // b13 now settles, the PS still faults
        check(!binding.settle(fake(context))&&binding.needsRestore()&&bound(patched,b13.Get()),"settle: one slot settling does not hide the other's failure");
        check(refsOf(original)==psRefs+1&&refsOf(b13.Get())==cbRefs,"settle: only the resolved slot's reference is released");
        context.site=Site::Good;
        check(binding.settle(fake(context))&&bound(original,b13.Get())&&!binding.needsRestore()&&quiet(),"settle: the next attempt puts the remaining slot back");
    }
    for(Site getter:{Site::GetShader,Site::GetBuffer}){   // a getter that faults after taking a reference
        Context context{ctxV,nullptr,real};Binding binding;strand(context,binding);
        context.site=getter;
        check(!binding.settle(fake(context))&&binding.needsRestore(),"settle: a faulting getter reports failure and retains the binding");
        check(refsOf(patched)==patchedRefs&&refsOf(constants)==constantsRefs,"settle: the reference a faulting getter took is released");
        check(binding.settle(fake(context))&&bound(original,b13.Get())&&!binding.needsRestore()&&quiet(),"settle: the next attempt completes");
    }
    {   // the boundary rule: hold for seven failures, fail open on the eighth, refs and state untouched
        Context context{ctxV,nullptr,real};Binding binding;strand(context,binding);unsigned failed=0;
        context.site=Site::PersistentBeforeBoth;
        for(unsigned i=1;i<kSettleBoundaries;++i)
            check(settleAtBoundary(binding,fake(context),directSetPs,failed)==Settle::kHold&&failed==i,"boundary: a failing settle holds the fence");
        check(settleAtBoundary(binding,fake(context),directSetPs,failed)==Settle::kFailOpen&&failed==0,"boundary: the eighth failed boundary in a row fails open");
        check(binding.needsRestore()&&bound(patched,constants)&&refsOf(original)==psRefs+1&&refsOf(b13.Get())==cbRefs+1,"boundary: failing open keeps the references and leaves the state");
        context.site=Site::Good;
        check(settleAtBoundary(binding,fake(context),directSetPs,failed)==Settle::kSettled&&bound(original,b13.Get())&&quiet(),"boundary: a later boundary still settles what fail-open retained");
    }
    {   // success clears the binding and resets the count
        Context context{ctxV,nullptr,real};Binding binding;strand(context,binding);unsigned failed=0;
        context.site=Site::PersistentBeforeBoth;
        check(settleAtBoundary(binding,fake(context),directSetPs,failed)==Settle::kHold&&settleAtBoundary(binding,fake(context),directSetPs,failed)==Settle::kHold&&failed==2,"boundary: two failures hold");
        context.site=Site::Good;
        check(settleAtBoundary(binding,fake(context),directSetPs,failed)==Settle::kSettled&&failed==0,"boundary: success lifts the fence and resets the count");
        check(!binding.needsRestore()&&bound(original,b13.Get())&&quiet(),"boundary: the binding is clear and the game's state is back");
        check(binding.begin(fake(context),patched,constants,{.4f,.4f,0,0},directSetPs,original)&&binding.finish(fake(context)).restored&&bound(original,b13.Get()),"boundary: a settled binding is reusable");
        binding.clear();
    }
    game(original,oldCb);
}

inline void run(ID3D11DeviceContext* real,ID3D11PixelShader* original,ID3D11PixelShader* patched,
                ID3D11Buffer* constants,const std::vector<BYTE>& bytes){
    void* childV[3]{};childV[2]=reinterpret_cast<void*>(&childRelease);
    void* deviceV[43]{};deviceV[1]=reinterpret_cast<void*>(&add);deviceV[2]=reinterpret_cast<void*>(&drop);
    deviceV[3]=reinterpret_cast<void*>(&createCb);deviceV[15]=reinterpret_cast<void*>(&createPs);
    void* ctxV[128]{};ctxV[3]=reinterpret_cast<void*>(&getDevice);ctxV[9]=reinterpret_cast<void*>(&setPs);
    ctxV[16]=reinterpret_cast<void*>(&setCb);ctxV[48]=reinterpret_cast<void*>(&update);
    ctxV[74]=reinterpret_cast<void*>(&getShader);ctxV[77]=reinterpret_cast<void*>(&getCb);
    const uint64_t ps=hash(bytes.data(),bytes.size());Identity old{};UINT length=sizeof(old);
    ck(original->GetPrivateData(identityGuid(),&length,&old));
    for(Site site:{Site::Good,Site::GetDevice,Site::CreateFail,Site::CreatePartialFail,Site::CreateFault,
            Site::CreateNull,Site::BufferFail,Site::BufferPartialFail,Site::BufferFault,Site::BufferNull}){
        Device device{deviceV,1,site,{childV,0},{childV,0}};Context context{ctxV,&device};Cache cache;
        check(cache.remember(original,ps,bytes.data(),bytes.size(),false),"failure cache retains exact source");
        const Identity marker{ps,reinterpret_cast<uintptr_t>(&device)};ck(original->SetPrivateData(identityGuid(),sizeof(marker),&marker));
        auto made=cache.prepare(reinterpret_cast<ID3D11DeviceContext*>(&context),original,ps);
        check((made!=nullptr)==(site==Site::Good),"failed/partial/null creation never admits a shader");
        if(site==Site::Good){check(cache.constants()!=nullptr,"successful creation includes constants");
            check(cache.prepare(reinterpret_cast<ID3D11DeviceContext*>(&context),original,ps)==made&&device.creates==1&&device.buffers==1,"warm cache creates nothing again");}
        else if(site!=Site::GetDevice){check(!cache.prepare(reinterpret_cast<ID3D11DeviceContext*>(&context),original,ps)&&device.creates==1,"failed device slot is bounded, no repeated preparation");}
        cache.reset();check(device.refs==1&&device.ps.refs==0&&device.cb.refs==0,"partial COM publication and device lease fully reclaimed");
        cache.reset();check(device.refs==1,"reset is idempotent");
    }
    ck(original->SetPrivateData(identityGuid(),sizeof(old),&old));
    real->PSSetShader(original,nullptr,0);ComPtr<ID3D11Buffer> oldCb;real->PSGetConstantBuffers(13,1,&oldCb);
    for(Site site:{Site::Good,Site::GetShader,Site::GetBuffer,Site::Update,Site::SetBuffer,Site::SetShader,Site::Classes}){
        Context context{ctxV,nullptr,real,site,{{childV,1},{childV,1}}};Binding binding;bool ok=false;
        const bool ran=edvr::guarded("ui_holo_test.partial",[&]{ok=binding.begin(reinterpret_cast<ID3D11DeviceContext*>(&context),patched,constants,{.4f,.4f,.125f,-.25f},directSetPs,original);});
        check(ran==(site==Site::Good||site==Site::Classes)&&ok==(site==Site::Good),"production partial begin reports failure");
        check(binding.restore(reinterpret_cast<ID3D11DeviceContext*>(&context)),"all saved shader/constant state restores after partial begin");binding.clear();
        ComPtr<ID3D11PixelShader> psNow;ComPtr<ID3D11Buffer> cbNow;UINT classes=0;
        real->PSGetShader(&psNow,nullptr,&classes);real->PSGetConstantBuffers(13,1,&cbNow);
        check(psNow.Get()==original&&cbNow.Get()==oldCb.Get()&&classes==0,"complete stock PS/b13 state preserved after normal/failure/SEH");
        check(context.classes[0].refs==1&&context.classes[1].refs==1,"all unsupported class getter leases reclaimed");
        if(site==Site::Classes)check(context.setters==0,"dynamic linkage declines before any state mutation");
    }
    Binding empty;
    for(Site site:{Site::BeforeShader,Site::BeforeBuffer,Site::PersistentBeforeShader}){
        Context context{ctxV,nullptr,real};Binding binding;
        check(binding.begin(reinterpret_cast<ID3D11DeviceContext*>(&context),patched,constants,{.4f,.4f,0,0}),"restore failure starts with actually moved rendering state");
        context.site=site;const auto result=binding.finish(reinterpret_cast<ID3D11DeviceContext*>(&context));
        check(result.retried&&result.restored==(site!=Site::PersistentBeforeShader),"before-real restore fault gets exactly one bounded retry, persistent failure remains untrusted");
        check(binding.needsRestore()==!result.restored,"persistent failure retains original references and blocks fallback and all replay issues");
        ComPtr<ID3D11PixelShader> restoredPs;ComPtr<ID3D11Buffer> restoredCb;real->PSGetShader(&restoredPs,nullptr,nullptr);real->PSGetConstantBuffers(13,1,&restoredCb);
        check(restoredCb.Get()==oldCb.Get(),"CB restore still runs after a shader restore fault");
        check(restoredPs.Get()==(result.restored?original:patched),"failed restoration never reports complete stock shader state");
        // The production WriteBackBegin gate uses result.restored retained in
        // Draw even after saved refs clear. No replay is safe on false.
        binding.clear();real->PSSetShader(original,nullptr,0);
    }
    settleCases(ctxV,real,original,patched,constants,oldCb.Get(),bytes);
    check(!empty.begin(real,patched,constants,{NAN,1,0,0})&&!empty.begin(real,patched,constants,{1,0,0,0}),"invalid constant map refuses before state capture");
    Params p{};check(!params(0,72,160,160,0,0,p)&&!params(96,72,0,160,0,0,p)&&!params(96,72,160,160,INFINITY,0,p),"invalid dimensions/jitter refuse");
    check(params(96,72,240,180,.125f,-.25f,p)&&p.x==.4f&&p.y==.4f&&p.bx==.125f&&p.by==-.25f,"production inverse viewport and jitter float4");
    auto chunks=parseContainer(bytes.data(),bytes.size(),0x50);
    for(const auto& c:chunks)if(c.tag==0x58454853||c.tag==0x52444853){
        std::vector<uint32_t> t(c.bytes.size()/4);std::memcpy(t.data(),c.bytes.data(),c.bytes.size());
        for(size_t at=2;at<t.size();at+=instructionLength(t,at))if((t[at]&2047)==89){t[at+2]=13;break;}
        bool rejected=false;try{patchProgram(t);}catch(const std::exception&){rejected=true;}check(rejected,"occupied shader b13 refuses, never rewritten");
    }
}
} // namespace holoFailure
