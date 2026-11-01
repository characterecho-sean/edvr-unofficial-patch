#pragma once
// THE RELIEF (ui_holo_remap.h; issue 140, 2026-11-01: the radar's terrain darker and flat with fix.ui_quality on). The
// hologram programs' 8-tap Sobel radius is r = max(cb1[120].z, 1) * cb2[21].y in the pixels of the target they rasterize
// into; in the layer, k times denser, the taps sat k times closer and the gradient term fell by about k. The production
// edit makes the `max` read cb13[1].x (k * max(cb1[120].z, 1)) against l(0.0). Here: the edit's position and tokens are
// pinned in both programs (and absent from the frosted base), the factor and the scene-block tee are run, and on the
// rig's adapter the edited program at twice the density gives the game-density gradient term the stock program gives,
// where the stock program at twice the density gives about half of it.
namespace reliefCases {
using namespace edvr::ui_holo_remap;
inline std::vector<uint32_t> program(const std::vector<BYTE>& container) {
    for(const auto& c:parseContainer(container.data(),container.size(),0x50))if(c.tag==0x58454853||c.tag==0x52444853){
        std::vector<uint32_t> t(c.bytes.size()/4);std::memcpy(t.data(),c.bytes.data(),c.bytes.size());return t;}
    throw std::runtime_error("no program chunk");
}
inline unsigned occurrences(const std::vector<uint32_t>& t,const uint32_t* seq,size_t n,size_t& at){
    unsigned count=0;for(size_t i=0;i+n<=t.size();++i)if(std::equal(seq,seq+n,t.begin()+i)){at=i;++count;}return count;
}
// The stock token index of the relief pair (max, then mul) and of dcl_temps in each program, from fxc /dumpbin of the
// dumped originals: EA02FAC2's max r1.x at 146, E95634B0's max r1.w at 389; dcl_temps at 79 in both.
inline constexpr size_t kStockAt[2]={146,389},kTempsAt=79;

inline void pins(int program01,const std::vector<BYTE>& stockBytes,const std::vector<BYTE>& patchedBytes){
    const auto stock=program(stockBytes),patched=program(patchedBytes);
    size_t at=0,other=0;
    check(occurrences(stock,kReliefStock[program01],kReliefLength,at)==1&&at==kStockAt[program01],"relief pin: the stock max/mul pair once, at its token index");
    check(occurrences(stock,kReliefStock[1-program01],kReliefLength,other)==0,"relief pin: the other program's pair is not in this one");
    check(patched.size()==stock.size()+15,"relief pin: the edit is the same size (only the declaration and the position mad are inserted)");
    uint32_t edited[kReliefLength];std::memcpy(edited,kReliefStock[program01],sizeof(edited));
    edited[3]=0x0020800a;edited[4]=13;edited[5]=1;edited[7]=0;   // cb13[1].x, l(0.0)
    size_t editedAt=0,stale=0;
    check(occurrences(patched,edited,kReliefLength,editedAt)==1&&editedAt==kStockAt[program01]+15,"relief pin: the edited pair once, 15 tokens on (declaration 4, mad 11)");
    check(occurrences(patched,kReliefStock[program01],kReliefLength,stale)==0,"relief pin: no stock pair left");
    const uint32_t dcl[]={0x04000059,0x00208e46,13,2};size_t dclAt=0;
    check(occurrences(patched,dcl,4,dclAt)==1&&dclAt==kTempsAt,"relief pin: b13 declared with two registers, before dcl_temps");
    // control: a program whose max reads another register is refused, not half-edited
    auto mutant=stock;mutant[kStockAt[program01]+5]=121;bool refused=false;
    try{patchProgram(mutant,Kind::kHologramDepth);}catch(const std::exception& e){refused=std::strcmp(e.what(),"one relief radius after the position")==0;}
    check(refused,"relief control: no relief pair refuses the remap");
    auto twice=stock;bool twiceRefused=false;   // control: a second copy of the pair (inserted before the closing ret) is refused
    check(twice.back()==0x0100003e,"relief control: the program ends with ret");
    twice.insert(twice.end()-1,kReliefStock[program01],kReliefStock[program01]+kReliefLength);twice[1]=static_cast<uint32_t>(twice.size());
    try{patchProgram(twice,Kind::kHologramDepth);}catch(const std::exception& e){twiceRefused=std::strcmp(e.what(),"one relief radius after the position")==0;}
    check(twiceRefused,"relief control: two relief pairs refuse the remap");
}
inline void frostedPins(){
    auto source=bytes("tools/ui_holo_test/fixtures/ps_0146ABCC53240479.dxbc");check(!source.empty(),"frosted fixture readable");
    std::vector<BYTE> out;std::string why;check(patch(source.data(),source.size(),edvrHash(source),out,why),why.c_str());
    const auto t=program(out);const uint32_t dcl[]={0x04000059,0x00208e46,13,1},edited[]={0x0020800a,13,1};size_t at=0;
    check(occurrences(t,dcl,4,at)==1,"relief pin: the frosted base keeps a one-register b13");
    check(occurrences(t,edited,3,at)==0,"relief pin: the frosted base has no relief edit");
}
inline void unitCases(){
    Params half{.5f,.5f,0,0},one{1,1,0,0};
    check(half.relief==1.0f&&one.relief==1.0f&&sizeof(Params)==32,"Params: two registers, the relief factor 1 unless set");
    check(std::fabs(reliefRadius(half,1.3666f)-2.0f*1.3666f)<1e-6f,"relief factor: k 2 times cb1[120].z above 1");
    check(reliefRadius(half,.5f)==2.0f&&reliefRadius(half,NAN)==2.0f&&reliefRadius(one,1.25f)==1.25f,"relief factor: the game's floor of 1, a non-finite z as 1, k 1 exact");
    Params wide{.25f,.5f,0,0};check(std::fabs(reliefRadius(wide,1)-std::sqrt(8.0f))<1e-5f,"relief factor: the geometric mean of the two axes");
    SceneRelief tee;float z=0;
    check(tee.find(nullptr,true,z)==SceneRelief::kNone&&z==1.0f,"tee: nothing written is z 1");
    std::vector<float> block(121*4,0.0f),other(121*4,0.0f);block[120*4+2]=1.3666f;other[120*4+2]=2.5f;
    int a=0,b=0,c=0;
    tee.note(&a,block.data(),120*16);check(tee.find(&a,true,z)==SceneRelief::kNone,"tee: a block short of row 120 is not read");
    tee.note(&a,block.data(),121*16);tee.note(&b,other.data(),121*16);
    check(tee.find(&a,true,z)==SceneRelief::kBound&&z==1.3666f,"tee: the bound buffer's own last write");
    check(tee.find(&b,true,z)==SceneRelief::kBound&&z==2.5f,"tee: by buffer");
    check(tee.find(&c,true,z)==SceneRelief::kLatest&&z==2.5f,"tee: a buffer not seen written takes the latest write");
    check(tee.find(&a,false,z)==SceneRelief::kLatest&&z==2.5f,"tee: a b1 bound past its first constant takes the latest write");
    block[120*4+2]=1.5f;tee.note(&a,block.data(),121*16);check(tee.find(&a,true,z)==SceneRelief::kBound&&z==1.5f,"tee: a rewrite replaces the value");
    tee.reset();check(tee.find(&a,true,z)==SceneRelief::kNone,"tee: reset forgets");
}

// One square target of side w drawn with ps over the rig's full-target triangle, no blend, no depth target; with p, through the
// production Binding into `constants` (b13), else the program alone. RGBA back.
inline std::vector<float> render(ID3D11Device* d,ID3D11DeviceContext* c,ID3D11PixelShader* ps,unsigned w,ID3D11ShaderResourceView* depthView,ID3D11Buffer* constants,const Params* p){
    D3D11_TEXTURE2D_DESC t{};t.Width=t.Height=w;t.MipLevels=t.ArraySize=1;t.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;t.SampleDesc.Count=1;t.BindFlags=D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> target;ck(d->CreateTexture2D(&t,nullptr,&target));ComPtr<ID3D11RenderTargetView> rtv;ck(d->CreateRenderTargetView(target.Get(),nullptr,&rtv));
    const float zero[4]={0,0,0,0};c->ClearRenderTargetView(rtv.Get(),zero);auto r=rtv.Get();c->OMSetRenderTargets(1,&r,nullptr);
    const D3D11_VIEWPORT vp{0,0,float(w),float(w),0,1};const D3D11_RECT sc{0,0,LONG(w),LONG(w)};c->RSSetViewports(1,&vp);c->RSSetScissorRects(1,&sc);
    c->PSSetShaderResources(1,1,&depthView);
    if(p){Binding binding;check(binding.begin(c,ps,constants,*p),"relief: production binding begins");c->Draw(3,0);check(binding.restore(c),"relief: production binding restores");binding.clear();}
    else{ComPtr<ID3D11PixelShader> old;c->PSGetShader(&old,nullptr,nullptr);c->PSSetShader(ps,nullptr,0);c->Draw(3,0);c->PSSetShader(old.Get(),nullptr,0);}
    c->OMSetRenderTargets(0,nullptr,nullptr);t.BindFlags=0;t.Usage=D3D11_USAGE_STAGING;t.CPUAccessFlags=D3D11_CPU_ACCESS_READ;ComPtr<ID3D11Texture2D> stage;ck(d->CreateTexture2D(&t,nullptr,&stage));
    c->CopyResource(stage.Get(),target.Get());D3D11_MAPPED_SUBRESOURCE m{};ck(c->Map(stage.Get(),0,D3D11_MAP_READ,0,&m));
    std::vector<float> out(w*w*4);for(unsigned y=0;y<w;++y)std::memcpy(out.data()+y*w*4,(BYTE*)m.pData+y*m.RowPitch,w*16);c->Unmap(stage.Get(),0);return out;
}
// The alpha (the gradient term: cb2[22].z is 0, so the centre mask adds nothing) over the interior [lo, hi) of a square target of side w.
struct Alpha {double mean=0,lo=1e30,hi=-1e30;};
inline Alpha alpha(const std::vector<float>& v,unsigned w,unsigned from,unsigned to){
    Alpha a;unsigned n=0;for(unsigned y=from;y<to;++y)for(unsigned x=from;x<to;++x){const double q=v[(y*w+x)*4+3];a.mean+=q;a.lo=std::min(a.lo,q);a.hi=std::max(a.hi,q);++n;}
    a.mean/=n;return a;
}

// On the rig's adapter: a linear ramp at t4 (linear filtering) has the same Sobel gradient everywhere, so the gradient term
// is a constant over the interior; the stock program at the game's density (64) gives the reference.
inline void gradientCases(ID3D11Device* d,ID3D11DeviceContext* c,ID3D11PixelShader* stock,ID3D11PixelShader* repaired,ID3D11Buffer* constants,const char* name){
    ComPtr<ID3D11Buffer> oldCb[2];ComPtr<ID3D11ShaderResourceView> oldT1,oldT4;ComPtr<ID3D11SamplerState> oldS1;ComPtr<ID3D11BlendState> oldBlend;float oldFactor[4];UINT oldMask=0;
    D3D11_VIEWPORT oldVp[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];UINT vpN=D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    D3D11_RECT oldSc[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];UINT scN=D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    ID3D11Buffer* rawCb[2]={};c->PSGetConstantBuffers(1,2,rawCb);oldCb[0].Attach(rawCb[0]);oldCb[1].Attach(rawCb[1]);
    c->PSGetShaderResources(1,1,&oldT1);c->PSGetShaderResources(4,1,&oldT4);c->PSGetSamplers(1,1,&oldS1);c->OMGetBlendState(&oldBlend,oldFactor,&oldMask);
    c->RSGetViewports(&vpN,oldVp);c->RSGetScissorRects(&scN,oldSc);
    c->OMSetBlendState(nullptr,nullptr,~0u);
    D3D11_SAMPLER_DESC sd{};sd.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;sd.AddressU=sd.AddressV=sd.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;sd.ComparisonFunc=D3D11_COMPARISON_ALWAYS;sd.MaxLOD=D3D11_FLOAT32_MAX;
    ComPtr<ID3D11SamplerState> linear;ck(d->CreateSamplerState(&sd,&linear));auto s1=linear.Get();c->PSSetSamplers(1,1,&s1);
    std::vector<float> ramp(64*64*4);for(unsigned y=0;y<64;++y)for(unsigned x=0;x<64;++x)for(unsigned k=0;k<4;++k)ramp[(y*64+x)*4+k]=1.f+.1f*x+.07f*y;
    auto picture=texture(d,64,64,ramp,true);auto t4=picture.Get();c->PSSetShaderResources(4,1,&t4);
    auto depth64=texture(d,64,64,std::vector<float>(64*64*4,3.f),true),depth128=texture(d,128,128,std::vector<float>(128*128*4,3.f),true);
    for(float z:{1.25f,.5f}){
        std::vector<float> b1(280*4,0),b2(23*4,0);b1[279*4+2]=1;b1[120*4+2]=z;
        b2[20*4+3]=1;b2[21*4]=1e6f;b2[21*4+1]=1;b2[21*4+2]=.1f;b2[21*4+3]=1;b2[13*4]=1;b2[13*4+1]=1;b2[19*4+1]=1;   // no cap, radius 1, alpha scale .1; cb2[22] all 0
        auto cb1=cb(d,b1),cb2=cb(d,b2);ID3D11Buffer* both[]={cb1.Get(),cb2.Get()};c->PSSetConstantBuffers(1,2,both);
        Params game{1,1,0,0};game.relief=reliefRadius(game,z);
        Params layer{.5f,.5f,0,0};layer.relief=reliefRadius(layer,z);
        Params noK{.5f,.5f,0,0};noK.relief=z>1?z:1.f;   // mutant: the game's own factor without k
        const auto s64=render(d,c,stock,64,depth64.Get(),constants,nullptr),e64=render(d,c,repaired,64,depth64.Get(),constants,&game);
        const auto s128=render(d,c,stock,128,depth128.Get(),constants,nullptr),e128=render(d,c,repaired,128,depth64.Get(),constants,&layer);
        const auto m128=render(d,c,repaired,128,depth64.Get(),constants,&noK);
        check(exact(s64,e64),"relief: at k 1 the edited program is the game's own, bit for bit");
        const Alpha g=alpha(s64,64,8,56),bug=alpha(s128,128,16,112),fixedA=alpha(e128,128,16,112),mutant=alpha(m128,128,16,112);
        check(g.mean>.2&&g.mean<.9&&g.hi-g.lo<.02*g.mean,"relief: the stock gradient term at the game's density is a nontrivial constant");
        check(bug.mean<.6*g.mean,"relief: the stock program at twice the density about halves the gradient term (the darker, flat radar)");
        check(std::fabs(fixedA.mean-g.mean)<.02*g.mean&&fixedA.lo>.95*g.mean&&fixedA.hi<1.05*g.mean,"relief: the edited program at twice the density gives the game-density gradient term");
        check(mutant.mean<.6*g.mean,"relief control: the factor without k leaves the gradient term halved");
        std::printf("%s relief z=%.2f: game %.4f, stock at 2x %.4f, edited at 2x %.4f (min %.4f max %.4f), without k %.4f\n",name,z,g.mean,bug.mean,fixedA.mean,fixedA.lo,fixedA.hi,mutant.mean);
    }
    ID3D11Buffer* restore[]={oldCb[0].Get(),oldCb[1].Get()};c->PSSetConstantBuffers(1,2,restore);
    auto t1=oldT1.Get();c->PSSetShaderResources(1,1,&t1);auto t4old=oldT4.Get();c->PSSetShaderResources(4,1,&t4old);auto s1old=oldS1.Get();c->PSSetSamplers(1,1,&s1old);
    c->OMSetBlendState(oldBlend.Get(),oldFactor,oldMask);c->RSSetViewports(vpN,oldVp);c->RSSetScissorRects(scN,oldSc);
}
inline void run(ID3D11Device* d,ID3D11DeviceContext* c,int program01,ID3D11PixelShader* stock,ID3D11PixelShader* repaired,ID3D11Buffer* constants,const std::vector<BYTE>& stockBytes){
    std::vector<BYTE> patched;std::string why;check(patch(stockBytes.data(),stockBytes.size(),edvrHash(stockBytes),patched,why),why.c_str());
    pins(program01,stockBytes,patched);
    gradientCases(d,c,stock,repaired,constants,program01==0?"EA02":"E956");
}
} // namespace reliefCases
