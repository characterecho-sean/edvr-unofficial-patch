#pragma once
#include "dxbc_container.h"
#include "../common/guard.h"
#include <d3d11.h>
#include <array>
#include <algorithm>
#include <cmath>
#include <mutex>

// The layer's SV_Position remap. A pixel shader that derives an address from the rasterizer's pixel position
// reads, inside the layer, a position over the LAYER's pixels (4032x3898, say) where the game's own draw would
// have read one over its target's (2620x2533). Each program below is remapped by an exact-bytecode transform that
// gives that one address the position the game's draw would have had: game = layer * (1/scale) + jitter. The
// programs are admitted by size and FNV hash of their bytes, never by a name or a family. Two kinds:
//  - the hologram sphere pair (2026-09-28) loads the scene depth at ftoi(SV_Position);
//  - the frosted base under every station-menu panel (2026-09-30) samples a blurred copy of the scene at
//    SV_Position * a game constant. Left in the game's own frame it was fine; taken into the layer (the after-UI
//    identity follows it there) the lookup ran past 1.0 by the layer's scale. Sean's report reads as a mirror-
//    addressed sampler folding the blurred scene back on itself; no census line records the mode, so the first
//    take logs it.
namespace edvr::ui_holo_remap {
inline constexpr uint64_t kVs=0x5559BD94B6852E83ull;
enum class Kind : uint8_t {kHologramDepth,kFrostedBase};
struct Program {uint64_t ps;uint32_t bytes;Kind kind;};
inline constexpr Program kPrograms[]={
    {0xEA02FAC2BD6C643Cull,7092,Kind::kHologramDepth},
    {0xE95634B0F61D218Full,11428,Kind::kHologramDepth},
    {0x0146ABCC53240479ull,2292,Kind::kFrostedBase},
};
inline constexpr size_t kProgramCount=sizeof(kPrograms)/sizeof(kPrograms[0]);
inline constexpr uint64_t kFrostedPs=0x0146ABCC53240479ull;
static_assert(kPrograms[2].ps==kFrostedPs,"frosted base slot");
inline int index(uint64_t ps) {
    for(size_t i=0;i<kProgramCount;++i)if(kPrograms[i].ps==ps)return static_cast<int>(i);
    return -1;
}
inline Kind kindOf(int slot) {return kPrograms[static_cast<size_t>(slot)].kind;} // slot from index(), never negative
inline const char* kindName(Kind k) {return k==Kind::kFrostedBase?"frosted base":"hologram depth";}
inline bool pair(uint64_t vs,uint64_t ps) {
    const int slot=index(ps);return vs==kVs&&slot>=0&&kindOf(slot)==Kind::kHologramDepth;
}
// Which taken draws need a remapped shader. The sphere VS into the HDR HUD layer: always asked for, and a PS that
// is not one of the two exact hologram programs is refused to stock by prepare(), never taken unremapped. The
// frosted base: whatever draws it, and whichever layer it lands in.
inline bool needsRemap(uint64_t vs,uint64_t ps,bool hdrTake) {
    if(hdrTake&&vs==kVs)return true;
    const int slot=index(ps);return slot>=0&&kindOf(slot)==Kind::kFrostedBase;
}
inline uint64_t hash(const void* data,size_t n) {
    auto p=static_cast<const BYTE*>(data);uint64_t h=1469598103934665603ull;
    for(size_t i=0;i<n;++i)h=(h^p[i])*1099511628211ull;return h;
}
inline void require(bool ok,const char* why) {if(!ok)throw std::runtime_error(why);}

// One exact edit per kind. The program has ONE instruction that reads SV_Position (v2) as its first source:
// `target`, the exact tokens of it. `follow` is what must come straight after it, so the edit can only land on the
// intended lookup. `operand` is the operand token that replaces v2 in the target: the new temp, with the swizzle
// the target read v2 through. The remap itself is the same for both kinds, one instruction inserted before the
// target: temp.xy = SV_Position.xy * cb13[0].xy + cb13[0].zw.
struct Recipe {
    uint32_t target[8];size_t targetLength;
    uint32_t follow[17];size_t followLength;
    uint32_t operand;
    const char* notFound,*notFollowed;
};
inline constexpr Recipe kRecipes[]={
    // ftoi r1.xy, v2.xyxx  then  mov r1.zw, l(0,0,0,0); ld_aoffimmi(0,0,0) r1.x, r1.xyzw, t1.xyzw
    {{0x0500001b,0x00100032,1,0x00101046,2},5,
     {0x08000036,0x001000c2,1,0x00004002,0,0,0,0,
      0x8900002d,0x800000c2,0x00155543,0x00100012,1,0x00100e46,1,0x00107e46,1},17,
     0x00100046,"one SV_Position ftoi","exact t1 load"},
    // mul r0.zw, v2.xxxy, cb1[332].zzzw  then  sample_indexable r1.xyz, r0.zwzz, t0.xyzw, s1
    {{0x08000038,0x001000c2,0,0x00101406,2,0x00208ea6,1,332},8,
     {0x8b000045,0x800000c2,0x00155543,0x00100072,1,0x00100ae6,0,0x00107e46,0,0x00106000,1},11,
     0x00100406,"one SV_Position mul","exact t0 lookup"},
};
static_assert(sizeof(kRecipes)/sizeof(kRecipes[0])==2,"one recipe per kind");

// Restricted actual-DXBC transform: only the one address the position feeds. Every material/UV/derivative
// instruction, signature and resource stays stock.
inline std::vector<uint32_t> patchProgram(const std::vector<uint32_t>& t,Kind kind=Kind::kHologramDepth) {
    using namespace dxbc_container;
    const Recipe& r=kRecipes[static_cast<size_t>(kind)];
    size_t target=0,tempAt=0;uint32_t temp=0;unsigned found=0;
    require(t.size()>2&&t[0]==0x50&&t[1]==t.size(),"pixel SM5 program");
    for(size_t a=2;a<t.size();a+=instructionLength(t,a)) {
        const unsigned op=t[a]&2047,n=instructionLength(t,a);
        if(op==89)require(n==4&&t[a+2]!=13,"private b13 occupied");
        if(op==104){require(n==2&&!tempAt,"temps declaration");tempAt=a;temp=t[a+1];}
        if(n==r.targetLength&&std::equal(r.target,r.target+n,t.begin()+a)){target=a;++found;}
    }
    require(found==1&&tempAt&&temp<4096&&tempAt<target,r.notFound);
    require(target+r.targetLength+r.followLength<=t.size()&&
        std::equal(r.follow,r.follow+r.followLength,t.begin()+target+r.targetLength),r.notFollowed);
    const uint32_t cb[]={0x04000059,0x00208e46,13,1};
    const uint32_t mad[]={0x0b000032,0x00100032,temp,0x00101046,2,0x00208046,13,0,0x00208ae6,13,0};
    std::vector<uint32_t> out(t.begin(),t.begin()+tempAt);out.insert(out.end(),cb,cb+4);
    out.insert(out.end(),t.begin()+tempAt,t.begin()+target);out[tempAt+5]=temp+1;
    out.insert(out.end(),mad,mad+11);out.insert(out.end(),t.begin()+target,t.end());
    out[target+18]=r.operand;out[target+19]=temp;out[1]=static_cast<uint32_t>(out.size());
    require(out.size()==t.size()+15,"bounded remap");
    for(size_t i=0;i<t.size();++i){if(i==1||i==tempAt+1||i==target+3||i==target+4)continue;
        require(out[i+(i>=tempAt?4:0)+(i>=target?11:0)]==t[i],"untouched instruction");}
    return out;
}
inline bool patch(const void* data,size_t n,uint64_t ps,std::vector<BYTE>& output,std::string& why) {
    output.clear();
    try {
        const int slot=index(ps);
        require(slot>=0&&data&&n==kPrograms[slot].bytes&&hash(data,n)==ps,"unknown pixel bytes");
        auto chunks=dxbc_container::parseContainer(data,n,0x50);bool position=false;
        for(const auto& c:chunks)if(c.tag==0x4e475349){
            for(const auto& e:dxbc_container::parseSignature(c.bytes))if(e.systemValue==1){
                require(!position&&e.registerIndex==2&&dxbc_container::equalName(e.name,"SV_Position"),"SV_Position signature");position=true;
            }
        }
        require(position,"SV_Position missing");unsigned changed=0;
        for(auto& c:chunks)if(c.tag==0x58454853||c.tag==0x52444853){
            std::vector<uint32_t> t(c.bytes.size()/4);std::memcpy(t.data(),c.bytes.data(),c.bytes.size());
            const auto out=patchProgram(t,kPrograms[slot].kind);c.bytes.resize(out.size()*4);std::memcpy(c.bytes.data(),out.data(),c.bytes.size());++changed;
        }
        require(changed==1,"one program");output=dxbc_container::makeContainer(chunks);return true;
    } catch(const std::exception& e) {why=e.what();return false;}
}
// The private cb13 float4 the remapped shader reads: game position = layer position * (x,y) + (bx,by). x,y is the
// game target's size over the layer's; bx,by the frame's jitter in the game's pixels (the layer's viewport moved
// by -jitter * scale, ui_layer_math.h's uiLayerJitterCancel, so a layer pixel stands for game position
// pixel / scale + jitter). The layer's map has no offset, so there is no other term.
struct Params {float x,y,bx,by;};
inline bool params(uint32_t sourceW,uint32_t sourceH,uint32_t layerW,uint32_t layerH,float jx,float jy,Params& out) {
    if(!sourceW||!sourceH||!layerW||!layerH||!std::isfinite(jx)||!std::isfinite(jy))return false;
    const float ax=float(layerW)/sourceW,ay=float(layerH)/sourceH;
    out={1.0f/ax,1.0f/ay,jx,jy};
    return std::isfinite(out.x)&&std::isfinite(out.y)&&out.x>0&&out.y>0;
}
inline bool depthSource(ID3D11ShaderResourceView* srv,uint32_t w,uint32_t h) {
    if(!srv||!w||!h)return false;D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
    ID3D11Resource* r=nullptr;ID3D11Texture2D* t=nullptr;bool ok=false;
    const bool ran=guarded("ui.holo.depth",[&]{srv->GetDesc(&sd);
        if(sd.Format!=DXGI_FORMAT_R32_FLOAT||sd.ViewDimension!=D3D11_SRV_DIMENSION_TEXTURE2D||sd.Texture2D.MostDetailedMip!=0)return;
        srv->GetResource(&r);if(r&&SUCCEEDED(r->QueryInterface(__uuidof(ID3D11Texture2D),reinterpret_cast<void**>(&t)))){
        D3D11_TEXTURE2D_DESC td{};t->GetDesc(&td);ok=td.Width==w&&td.Height==h&&td.ArraySize==1&&td.SampleDesc.Count==1;
    }});
    if(t){auto p=t;t=nullptr;guarded("ui.holo.depth.release",[&]{p->Release();});}
    if(r){auto p=r;r=nullptr;guarded("ui.holo.depth.release",[&]{p->Release();});}
    return ran&&ok;
}
template<class T> inline void release(T*& value) {
    auto p=value;value=nullptr;if(p)guarded("ui.holo.release",[&]{p->Release();});
}
inline const GUID& identityGuid(){static const GUID guid={0x941dd2b1,0x85ee,0x4f5a,{0xa5,0xe8,0xb1,0x6a,0x72,0x28,0x00,0x13}};return guid;}
struct Identity {uint64_t ps;uintptr_t device;};

// Creation remembers only one immutable patched blob per program; no driver call under
// its mutex. GPU state belongs exclusively to the render owner, one device.
class Cache {
    std::mutex mutex_;std::array<std::vector<BYTE>,kProgramCount> bytes_;
    ID3D11Device* device_=nullptr;ID3D11PixelShader* shaders_[kProgramCount]{};ID3D11Buffer* cb_=nullptr;
    bool attempted_[kProgramCount]{};
public:
    ~Cache(){reset();}
    bool remember(ID3D11PixelShader* shader,uint64_t ps,const void* data,size_t n,bool linked) {
        if(!shader||linked||index(ps)<0)return false;std::vector<BYTE> remapped;std::string why;
        if(!patch(data,n,ps,remapped,why))return false;
        {std::lock_guard<std::mutex> lock(mutex_);auto& old=bytes_[index(ps)];if(!old.empty()&&old!=remapped)return false;if(old.empty())old=std::move(remapped);}
        ID3D11Device* device=nullptr;HRESULT hr=E_FAIL;
        const bool ran=guarded("ui.holo.identity",[&]{shader->GetDevice(&device);if(device){
            const Identity identity{ps,reinterpret_cast<uintptr_t>(device)};
            hr=shader->SetPrivateData(identityGuid(),sizeof(identity),&identity);
        }});
        release(device);return ran&&SUCCEEDED(hr);
    }
    void reset() {for(auto& s:shaders_)release(s);release(cb_);release(device_);for(auto& a:attempted_)a=false;}
    ID3D11PixelShader* prepare(ID3D11DeviceContext* ctx,ID3D11PixelShader* original,uint64_t ps) {
        const int slot=index(ps);if(!ctx||!original||slot<0)return nullptr;
        Identity identity{};UINT length=sizeof(identity);HRESULT tagHr=E_FAIL;
        if(!guarded("ui.holo.identity.read",[&]{tagHr=original->GetPrivateData(identityGuid(),&length,&identity);})||
            FAILED(tagHr)||length!=sizeof(identity)||identity.ps!=ps)return nullptr;
        ID3D11Device* d=nullptr;
        const bool ran=guarded("ui.holo.device",[&]{ctx->GetDevice(&d);});
        if(!ran||!d){release(d);return nullptr;}
        if(identity.device!=reinterpret_cast<uintptr_t>(d)){release(d);return nullptr;}
        if(d!=device_){reset();device_=d;d=nullptr;}release(d);
        if(attempted_[slot])return shaders_[slot]&&cb_?shaders_[slot]:nullptr;
        attempted_[slot]=true;std::vector<BYTE> bytes;
        try{std::lock_guard<std::mutex> lock(mutex_);bytes=bytes_[slot];}catch(const std::exception&){return nullptr;}
        if(bytes.empty())return nullptr;
        HRESULT hr=E_FAIL;bool made=guarded("ui.holo.create",[&]{hr=device_->CreatePixelShader(bytes.data(),bytes.size(),nullptr,&shaders_[slot]);});
        if(!made||FAILED(hr)||!shaders_[slot]){release(shaders_[slot]);return nullptr;}
        if(!cb_){D3D11_BUFFER_DESC bd{};bd.ByteWidth=sizeof(Params);bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
            made=guarded("ui.holo.constants",[&]{hr=device_->CreateBuffer(&bd,nullptr,&cb_);});
            if(!made||FAILED(hr)||!cb_){release(cb_);release(shaders_[slot]);return nullptr;}
        }
        return shaders_[slot];
    }
    ID3D11Buffer* constants() const {return cb_;}
};
using SetPs=void(*)(ID3D11DeviceContext*,ID3D11PixelShader*,ID3D11ClassInstance*const*,uint32_t);
inline void directSetPs(ID3D11DeviceContext* c,ID3D11PixelShader* p,ID3D11ClassInstance*const* a,uint32_t n){c->PSSetShader(p,a,n);}
class Binding {
    ID3D11PixelShader* ps_=nullptr;ID3D11Buffer* cb_=nullptr;
    // EDVR's own PS and b13 buffer, kept for IDENTITY only: settle() puts the game's state back into a slot
    // only while that slot still holds these. Never dereferenced or released here; the Cache owns both.
    const void* patched_=nullptr;const void* constants_=nullptr;
    ID3D11ClassInstance* classes_[D3D11_SHADER_MAX_INTERFACES]{};UINT count_=D3D11_SHADER_MAX_INTERFACES;
    bool shaderSaved_=false,bufferSaved_=false,modified_=false;
    // One slot of settle(): read what is bound now (releasing whatever the getter took, even when it faulted
    // after taking it), put the saved original back ONLY if the slot still holds EDVR's object, then let go of
    // the saved reference. A slot the game has rebound holds the game's own state already. False leaves the
    // slot, and its saved reference, for the next attempt.
    bool settleShader(ID3D11DeviceContext* c,SetPs setPs){
        ID3D11PixelShader* bound=nullptr;
        const bool read=guarded("ui.holo.settle.get.ps",[&]{c->PSGetShader(&bound,nullptr,nullptr);});
        const bool ours=bound&&static_cast<const void*>(bound)==patched_;release(bound);
        if(!read)return false;
        if(ours&&!guarded("ui.holo.settle.ps",[&]{setPs(c,ps_,classes_,count_);}))return false;
        release(ps_);for(auto& p:classes_)release(p);count_=D3D11_SHADER_MAX_INTERFACES;shaderSaved_=false;return true;
    }
    bool settleBuffer(ID3D11DeviceContext* c){
        ID3D11Buffer* bound=nullptr;
        const bool read=guarded("ui.holo.settle.get.cb",[&]{c->PSGetConstantBuffers(13,1,&bound);});
        const bool ours=bound&&static_cast<const void*>(bound)==constants_;release(bound);
        if(!read)return false;
        if(ours&&!guarded("ui.holo.settle.cb",[&]{c->PSSetConstantBuffers(13,1,&cb_);}))return false;
        release(cb_);bufferSaved_=false;return true;
    }
public:
    ~Binding(){clear();}
    bool needsRestore() const {return modified_;}
    void clear(){release(ps_);release(cb_);for(auto& p:classes_)release(p);count_=D3D11_SHADER_MAX_INTERFACES;shaderSaved_=bufferSaved_=modified_=false;patched_=constants_=nullptr;}
    bool begin(ID3D11DeviceContext* c,ID3D11PixelShader* patched,ID3D11Buffer* constants,const Params& p,SetPs setPs=directSetPs,ID3D11PixelShader* expected=nullptr){
        if(!c||!patched||!constants||modified_||shaderSaved_||bufferSaved_||
            !std::isfinite(p.x)||!std::isfinite(p.y)||!std::isfinite(p.bx)||!std::isfinite(p.by)||p.x<=0||p.y<=0)return false;
        c->PSGetShader(&ps_,classes_,&count_);shaderSaved_=true;
        c->PSGetConstantBuffers(13,1,&cb_);bufferSaved_=true;
        if(count_!=0||(expected&&ps_!=expected))return false; // actual variants have no dynamic linkage
        c->UpdateSubresource(constants,0,nullptr,&p,0,0);
        patched_=patched;constants_=constants; // what settle() will recognise as ours, recorded before either setter can publish
        modified_=true; // either setter can partially publish before a fault
        c->PSSetConstantBuffers(13,1,&constants);setPs(c,patched,nullptr,0);return true;
    }
    bool restore(ID3D11DeviceContext* c,SetPs setPs=directSetPs){
        if(!modified_)return true;bool ok=true;
        if(shaderSaved_)ok=guarded("ui.holo.restore.ps",[&]{setPs(c,ps_,classes_,count_);})&&ok;
        if(bufferSaved_)ok=guarded("ui.holo.restore.cb",[&]{c->PSSetConstantBuffers(13,1,&cb_);})&&ok;
        modified_=!ok;return ok;
    }
    struct RestoreResult {bool restored,retried;};
    RestoreResult finish(ID3D11DeviceContext* c,SetPs setPs=directSetPs){
        if(restore(c,setPs))return {true,false};
        return {restore(c,setPs),true}; // one bounded attempt, retaining original refs
    }
    // The catch-up for a finish() that failed twice (the saved references are retained, needsRestore() is
    // still true), run later on the same context: for each saved slot, put the saved original back ONLY if
    // the slot still holds EDVR's own object; a slot the game has rebound since holds the game's own state
    // already, so its saved reference is just released. True when nothing of EDVR's remains bound (every
    // slot resolved, saved references gone); false keeps whatever is unresolved, references included, for
    // another attempt. Each getter and setter runs under its own guard, the CB slot even after a PS fault.
    bool settle(ID3D11DeviceContext* c,SetPs setPs=directSetPs){
        if(!modified_)return true;
        if(!c)return false;
        bool ok=true;
        if(shaderSaved_)ok=settleShader(c,setPs)&&ok;
        if(bufferSaved_)ok=settleBuffer(c)&&ok;
        if(ok)modified_=false;
        return ok;
    }
};

// The frame-boundary rule for the fence a failed restore raises (ui_layer.cpp's g_uiLayerIssueBlocked, which
// drops the game's own draws on the owner context): one settle() per boundary, and a fence that never settles
// must not black out the game for good. kSettled: nothing of EDVR's is bound, the binding is cleared, lift the
// fence. kHold: still unresolved, keep the fence. kFailOpen: the kSettleBoundaries-th boundary in a row
// failed; lift the fence anyway, the saved references stay retained (needsRestore() stays true) and the
// caller says so loudly. `failed` is the caller's running count; it is reset whenever the fence lifts.
inline constexpr unsigned kSettleBoundaries=8;
enum class Settle {kHold,kSettled,kFailOpen};
inline Settle settleAtBoundary(Binding& binding,ID3D11DeviceContext* c,SetPs setPs,unsigned& failed){
    if(binding.settle(c,setPs)){binding.clear();failed=0;return Settle::kSettled;}
    if(++failed<kSettleBoundaries)return Settle::kHold;
    failed=0;return Settle::kFailOpen;
}
} // namespace edvr::ui_holo_remap
