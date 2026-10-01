#pragma once
// The frosted base under every station-menu panel: PS 0146ABCC53240479, the game's own 2292 bytes
// (fixtures/), drawn on the rig's adapter through the production Cache, Binding and Params.
//
// The defect (flight 2026-09-30 07:18, build 1cc64235): the base looks up a blurred copy of the scene at
// SV_Position * cb1[332].zw, the game's own position-to-UV scale for its eye target. Taken into the layer,
// SV_Position runs over the LAYER's pixels (4032x3898 for the game's 2620x2533), the lookup runs past 1.0 by
// the layer's scale, and a mirror-addressed sampler (Sean's word; the mode is logged at the first take) shows
// the blurred scene folded back on itself.
//
// How the rig reads it. The constants below neutralise the tone path, so the shader's colour is the blurred
// scene's own texel; the scene here is a ramp whose texel value IS its own UV. o0.xy therefore reads back the
// exact UV the lookup ran at, pixel by pixel, and no reference image is needed that could share a mistake
// with the code under test. The truth for a layer pixel comes from the GEOMETRY: the game-space position of
// the point that pixel shows, found by inverting the production viewport map (ui_layer_math.h's
// uiLayerMapViewport with the jitter cancel), which knows nothing of Params.
#include "../../src/d3d11/ui_layer_math.h"
#include <cfloat>
#include <string>
namespace frostedBase {
using namespace edvr::ui_holo_remap;
constexpr unsigned kRamp=256;
constexpr unsigned kGw=96,kGh=72;   // the rig's default game target
// The float chain plus the sampler's 8-bit bilinear weights stay under 2e-5; a jitter of the sign that is
// wrong moves the reading by 4e-3 and a wrong scale by 0.1.
constexpr double kTol=5e-5;
constexpr const char* kFixture="tools/ui_holo_test/fixtures/ps_0146ABCC53240479.dxbc";

struct Scene {unsigned gw,gh,lw,lh;float jx,jy;D3D11_VIEWPORT vp;D3D11_RECT sc;D3D11_TEXTURE_ADDRESS_MODE mode;};
struct Mapped {D3D11_VIEWPORT vp;D3D11_RECT sc;float cx,cy;};
// The game's viewport and scissor through the production map and jitter cancel, as ui_layer.cpp does them.
inline Mapped mapScene(const Scene& s) {
    const auto m=edvr::uiLayerMapFromRegion(0,0,static_cast<float>(s.gw),static_cast<float>(s.gh),s.lw,s.lh);
    Mapped r{};edvr::uiLayerJitterCancel(s.jx,s.jy,m,&r.cx,&r.cy);
    const edvr::UiViewport g{s.vp.TopLeftX,s.vp.TopLeftY,s.vp.Width,s.vp.Height,0,1};
    const edvr::UiViewport o=edvr::uiLayerMapViewport(m,g,r.cx,r.cy);r.vp={o.x,o.y,o.w,o.h,0,1};
    const edvr::UiRect q=edvr::uiLayerMapScissor(m,edvr::UiRect{s.sc.left,s.sc.top,s.sc.right,s.sc.bottom},r.cx,r.cy,s.lw,s.lh);
    r.sc={q.l,q.t,q.r,q.b};return r;
}
inline std::vector<float> rampValues(unsigned n) {
    std::vector<float> v(size_t(n)*n*4);
    for(unsigned y=0;y<n;++y)for(unsigned x=0;x<n;++x){float* p=&v[(size_t(y)*n+x)*4];p[0]=(x+.5f)/n;p[1]=(y+.5f)/n;p[2]=.5f;p[3]=1;}
    return v;
}
// cb1: the lookup branch on, the 0.6 the tone curve applies first undone, and the game's own position-to-UV
// scale for its eye target (cb1[332].zw = 1/size). Every other row stays zero.
inline std::vector<float> gameConstants(unsigned gw,unsigned gh) {
    std::vector<float> v(333*4,0.f);v[49*4]=1.f;v[90*4+2]=static_cast<float>(1.0/0.6);
    v[332*4+2]=1.f/gw;v[332*4+3]=1.f/gh;return v;
}
// cb2: the tone path as the identity. Exposure 1, the rational branch with numerator x and denominator 1, gamma
// 1 through the log/exp pair, saturation 1, output gamma 2.2 undone, alpha 1, no light from t3.
inline std::vector<float> toneConstants() {
    std::vector<float> v(10*4,0.f);auto at=[&](int row,int col,float x){v[row*4+col]=x;};
    at(0,0,1);at(0,1,1);at(2,2,1);at(3,1,1);at(3,3,1);at(4,0,1);at(4,1,1);at(4,2,1);at(4,3,2.2f);
    at(6,0,1);at(6,1,1);at(8,3,1);at(9,0,1);at(9,1,1);at(9,2,1);at(9,3,1);return v;
}
struct Pipe {
    ComPtr<ID3D11VertexShader> vs;ComPtr<ID3D11RasterizerState> raster;ComPtr<ID3D11DepthStencilState> ds;
    ComPtr<ID3D11SamplerState> point,mirror,clamp,wrap;ComPtr<ID3D11Buffer> tone,sentinel;
    ComPtr<ID3D11ShaderResourceView> ramp,ones,zeros;
    ID3D11SamplerState* sampler(D3D11_TEXTURE_ADDRESS_MODE m) const {
        return m==D3D11_TEXTURE_ADDRESS_MIRROR?mirror.Get():m==D3D11_TEXTURE_ADDRESS_CLAMP?clamp.Get():wrap.Get();
    }
};
inline Pipe makePipe(ID3D11Device* d,ID3D11DeviceContext* c) {
    Pipe p;c->ClearState();
    // The PS reads TEXCOORD0 (xyzw), TEXCOORD7 (xy) and SV_Position; nothing else about the vertex matters.
    const char* vs=R"(struct O{float4 a:TEXCOORD0;float2 b:TEXCOORD7;float4 p:SV_POSITION;};O main(uint id:SV_VertexID){O o;float2 q=float2((id<<1)&2,id&2);o.p=float4(q*float2(2,-2)+float2(-1,1),0,1);o.a=float4(q,q);o.b=q;return o;})";
    ComPtr<ID3DBlob> code,error;ck(D3DCompile(vs,std::strlen(vs),nullptr,nullptr,nullptr,"main","vs_5_0",0,0,&code,&error));
    ck(d->CreateVertexShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&p.vs));
    D3D11_RASTERIZER_DESC rs{};rs.FillMode=D3D11_FILL_SOLID;rs.CullMode=D3D11_CULL_NONE;rs.DepthClipEnable=rs.ScissorEnable=TRUE;ck(d->CreateRasterizerState(&rs,&p.raster));
    D3D11_DEPTH_STENCIL_DESC ds{};ck(d->CreateDepthStencilState(&ds,&p.ds));
    D3D11_SAMPLER_DESC sd{};sd.Filter=D3D11_FILTER_MIN_MAG_MIP_POINT;sd.AddressU=sd.AddressV=sd.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;sd.ComparisonFunc=D3D11_COMPARISON_ALWAYS;sd.MaxLOD=FLT_MAX;
    ck(d->CreateSamplerState(&sd,&p.point));
    sd.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU=sd.AddressV=sd.AddressW=D3D11_TEXTURE_ADDRESS_MIRROR;ck(d->CreateSamplerState(&sd,&p.mirror));
    sd.AddressU=sd.AddressV=sd.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;ck(d->CreateSamplerState(&sd,&p.clamp));
    sd.AddressU=sd.AddressV=sd.AddressW=D3D11_TEXTURE_ADDRESS_WRAP;ck(d->CreateSamplerState(&sd,&p.wrap));
    p.tone=cb(d,toneConstants());p.sentinel=cb(d,{7,8,9,10});
    p.ramp=texture(d,kRamp,kRamp,rampValues(kRamp));p.ones=texture(d,4,4,std::vector<float>(4*4*4,1));p.zeros=texture(d,4,4,std::vector<float>(4*4*4,0));
    return p;
}
// One draw over a w x h float target and its readback. `remap` set draws `patched` through the production
// Binding over the game's own PS (`stock`), exactly as ui_layer.cpp does around a taken draw, and checks the
// game's PS and its b13 come back; otherwise `stock` itself is drawn.
inline std::vector<float> render(ID3D11Device* d,ID3D11DeviceContext* c,const Pipe& p,unsigned w,unsigned h,
        const D3D11_VIEWPORT& vp,const D3D11_RECT& sc,D3D11_TEXTURE_ADDRESS_MODE mode,ID3D11Buffer* game,
        ID3D11PixelShader* stock,ID3D11PixelShader* patched,ID3D11Buffer* remapCb,const Params* remap) {
    D3D11_TEXTURE2D_DESC t{};t.Width=w;t.Height=h;t.MipLevels=t.ArraySize=1;t.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;t.SampleDesc.Count=1;t.BindFlags=D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> target;ck(d->CreateTexture2D(&t,nullptr,&target));
    ComPtr<ID3D11RenderTargetView> rtv;ck(d->CreateRenderTargetView(target.Get(),nullptr,&rtv));
    auto r=rtv.Get();const float clear[4]={-.25f,.125f,.375f,.5f};c->ClearRenderTargetView(r,clear);
    c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);c->VSSetShader(p.vs.Get(),nullptr,0);
    c->RSSetState(p.raster.Get());c->OMSetDepthStencilState(p.ds.Get(),0);c->OMSetBlendState(nullptr,nullptr,~0u);
    c->OMSetRenderTargets(1,&r,nullptr);c->RSSetViewports(1,&vp);c->RSSetScissorRects(1,&sc);
    ID3D11Buffer* buffers[2]={game,p.tone.Get()};c->PSSetConstantBuffers(1,2,buffers);
    auto sent=p.sentinel.Get();c->PSSetConstantBuffers(13,1,&sent);
    ID3D11ShaderResourceView* views[4]={p.ramp.Get(),p.ones.Get(),p.ones.Get(),p.zeros.Get()};c->PSSetShaderResources(0,4,views);
    ID3D11SamplerState* samplers[2]={p.point.Get(),p.sampler(mode)};c->PSSetSamplers(0,2,samplers);
    c->PSSetShader(stock,nullptr,0);
    if(remap){
        ComPtr<ID3D11PixelShader> oldPs;ComPtr<ID3D11Buffer> oldCb;c->PSGetShader(&oldPs,nullptr,nullptr);c->PSGetConstantBuffers(13,1,&oldCb);
        Binding binding;check(binding.begin(c,patched,remapCb,*remap,directSetPs,stock),"production binding begins over the game's own PS");
        c->Draw(3,0);check(binding.restore(c),"production binding restores");binding.clear();
        ComPtr<ID3D11PixelShader> nowPs;ComPtr<ID3D11Buffer> nowCb;c->PSGetShader(&nowPs,nullptr,nullptr);c->PSGetConstantBuffers(13,1,&nowCb);
        check(nowPs.Get()==oldPs.Get()&&nowCb.Get()==oldCb.Get(),"the game's PS and its b13 are back after the draw");
    } else c->Draw(3,0);
    c->OMSetRenderTargets(0,nullptr,nullptr);t.BindFlags=0;t.Usage=D3D11_USAGE_STAGING;t.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> stage;ck(d->CreateTexture2D(&t,nullptr,&stage));c->CopyResource(stage.Get(),target.Get());
    D3D11_MAPPED_SUBRESOURCE m{};ck(c->Map(stage.Get(),0,D3D11_MAP_READ,0,&m));
    std::vector<float> out(size_t(w)*h*4);for(unsigned y=0;y<h;++y)std::memcpy(out.data()+size_t(y)*w*4,static_cast<BYTE*>(m.pData)+size_t(y)*m.RowPitch,size_t(w)*16);
    c->Unmap(stage.Get(),0);return out;
}
struct Reading {unsigned interior=0,missing=0,stray=0;double worst=0;bool folded=false;};
// Per pixel: inside the mapped viewport and scissor (by more than half a pixel either way) it must be drawn and
// read the UV of the game position its geometry stands at; outside them it must not be drawn. The counts of
// pixels that broke either rule, and the worst UV error over the rest, come back for the caller to assert once.
inline Reading read(const Scene& s,const Mapped& m,const std::vector<float>& px) {
    Reading r;const double margin=2.0/kRamp;
    auto inside=[&](double v,double lo,double hi,double slack){return v>=lo+slack&&v<=hi-slack;};
    for(unsigned y=0;y<s.lh;++y)for(unsigned x=0;x<s.lw;++x){
        const float* p=&px[(size_t(y)*s.lw+x)*4];const bool drawn=p[3]>.9f;
        const double cx=x+.5,cy=y+.5,vx0=m.vp.TopLeftX,vx1=vx0+m.vp.Width,vy0=m.vp.TopLeftY,vy1=vy0+m.vp.Height;
        const bool sure=inside(cx,vx0,vx1,.51)&&inside(cy,vy0,vy1,.51)&&cx>=m.sc.left+.51&&cx<=m.sc.right-.51&&cy>=m.sc.top+.51&&cy<=m.sc.bottom-.51;
        const bool out=cx<vx0-.51||cx>vx1+.51||cy<vy0-.51||cy>vy1+.51||cx<m.sc.left-.51||cx>m.sc.right+.51||cy<m.sc.top-.51||cy>m.sc.bottom+.51;
        if(out){if(drawn)++r.stray;continue;}
        if(!sure)continue;
        if(!drawn){++r.missing;continue;}
        // The game-space point this layer pixel shows: the production viewport transform, inverted.
        const double gx=s.vp.TopLeftX+(cx-vx0)/m.vp.Width*s.vp.Width,gy=s.vp.TopLeftY+(cy-vy0)/m.vp.Height*s.vp.Height;
        const double u=gx/s.gw,v=gy/s.gh;
        if(u<margin||u>1-margin||v<margin||v>1-margin)continue;
        ++r.interior;r.worst=std::max({r.worst,std::fabs(p[0]-u),std::fabs(p[1]-v)});
        if(x+1<s.lw&&px[(size_t(y)*s.lw+x+1)*4]<p[0]-1e-3f&&px[(size_t(y)*s.lw+x+1)*4+3]>.9f)r.folded=true;
    }
    return r;
}
inline std::vector<Scene> scenes() {
    std::vector<Scene> v;
    auto add=[&](unsigned lw,unsigned lh,float jx,float jy,bool sub,D3D11_TEXTURE_ADDRESS_MODE mode,unsigned w=kGw,unsigned h=kGh){
        Scene s{};s.gw=w;s.gh=h;s.lw=lw;s.lh=lh;s.jx=jx;s.jy=jy;
        s.vp={sub?12.f:0.f,sub?7.f:0.f,static_cast<float>(w)-(sub?24.f:0.f),static_cast<float>(h)-(sub?14.f:0.f),0,1};
        s.sc={sub?17:0,sub?11:0,static_cast<LONG>(w)-(sub?19:0),static_cast<LONG>(h)-(sub?13:0)};s.mode=mode;v.push_back(s);
    };
    const D3D11_TEXTURE_ADDRESS_MODE modes[]={D3D11_TEXTURE_ADDRESS_MIRROR,D3D11_TEXTURE_ADDRESS_CLAMP,D3D11_TEXTURE_ADDRESS_WRAP};
    for(auto mode:modes){
        for(float ratio:{.5f,.75f})for(float ui:{1.f,1.25f})for(unsigned eye:{0u,1u})for(unsigned sub:{0u,1u})for(unsigned jit:{0u,1u})
            add(static_cast<unsigned>(kGw/ratio*ui),static_cast<unsigned>(kGh/ratio*ui),jit?(eye?.3125f:-.1875f):0.f,jit?(eye?-.21875f:.15625f):0.f,sub!=0,mode);
        add(150,101,.3125f,-.21875f,false,mode);add(150,101,-.1875f,.15625f,true,mode);   // the two axes scale differently
        add(202,195,.3125f,-.21875f,false,mode,131,127);add(202,195,-.1875f,.15625f,true,mode,131,127); // the flight's 2620x2533 -> 4032x3898, at a twentieth
        add(96,72,.3125f,-.21875f,false,mode);add(96,72,0,0,true,mode);                    // the layer the game's own size: identity, jitter only
        add(72,54,.3125f,-.21875f,false,mode);                                           // a layer smaller than the game's target (Elite's supersampling above 1)
    }
    return v;
}
// The patched program's container against the stock one: ISGN and OSGN untouched, only SHEX larger, by exactly
// the cb13 declaration, the raised temp count and the one inserted mad.
inline void structure(const std::vector<BYTE>& stock,const std::vector<BYTE>& hologram) {
    std::vector<BYTE> patched;std::string why;
    const bool patchedOk=edvr::ui_holo_remap::patch(stock.data(),stock.size(),kFrostedPs,patched,why);
    check(patchedOk,why.c_str());
    check(patched.size()==stock.size()+60,"the frosted program grows by exactly 15 tokens");
    const auto a=edvr::dxbc_container::parseContainer(stock.data(),stock.size(),0x50),b=edvr::dxbc_container::parseContainer(patched.data(),patched.size(),0x50);
    check(a.size()==3&&b.size()==a.size(),"three chunks before and after");
    unsigned grown=0;
    for(size_t i=0;i<a.size();++i){
        check(a[i].tag==b[i].tag,"chunk order kept");
        if(a[i].tag==0x58454853){check(b[i].bytes.size()==a[i].bytes.size()+60,"SHEX grows by 60 bytes");++grown;}
        else check(a[i].bytes==b[i].bytes,"signature chunks untouched");
    }
    check(grown==1,"one program chunk");
    // The other program's bytes are refused under this hash, this one's under theirs, and one flipped bit anywhere.
    auto refused=[&](const std::vector<BYTE>& bytes,uint64_t hash,const char* what){std::vector<BYTE> out;std::string w;check(!edvr::ui_holo_remap::patch(bytes.data(),bytes.size(),hash,out,w)&&out.empty(),what);};
    refused(hologram,kFrostedPs,"hologram bytes under the frosted hash");refused(stock,kPrograms[0].ps,"frosted bytes under a hologram hash");
    for(size_t at:{size_t(8),size_t(100),stock.size()/2,stock.size()-1}){auto bad=stock;bad[at]^=1;refused(bad,kFrostedPs,"one flipped bit");}
    // Each recipe finds only its own instruction.
    auto tokens=[](const std::vector<BYTE>& bytes){for(const auto& c:edvr::dxbc_container::parseContainer(bytes.data(),bytes.size(),0x50))if(c.tag==0x58454853){std::vector<uint32_t> t(c.bytes.size()/4);std::memcpy(t.data(),c.bytes.data(),c.bytes.size());return t;}return std::vector<uint32_t>();};
    auto throws=[&](const std::vector<uint32_t>& t,Kind k,const char* what){bool threw=false;try{patchProgram(t,k);}catch(const std::exception&){threw=true;}check(threw,what);};
    const auto ft=tokens(stock),ht=tokens(hologram);
    check(ft.size()==519&&patchProgram(ft,Kind::kFrostedBase).size()==534,"the frosted recipe patches the frosted tokens");
    throws(ft,Kind::kHologramDepth,"the hologram recipe finds nothing in the frosted program");
    throws(ht,Kind::kFrostedBase,"the frosted recipe finds nothing in a hologram program");
    // The follow check binds the edit to the lookup: break one token of the sample after the mul.
    auto broken=ft;for(size_t a2=2;a2+1<broken.size();a2+=edvr::dxbc_container::instructionLength(broken,a2)){
        if((broken[a2]&2047)==56&&broken[a2+3]==0x00101406){broken[a2+8+4]^=1;break;}}
    throws(broken,Kind::kFrostedBase,"the edit refuses a program whose lookup is not the one it expects");
}
inline void slots(ID3D11Device* d,ID3D11DeviceContext* c) {
    const char* names[3]={"EA02FAC2BD6C643C","E95634B0F61D218F","0146ABCC53240479"};
    std::vector<BYTE> code[3];ComPtr<ID3D11PixelShader> stock[3],prepared[3];
    for(int i=0;i<3;++i){code[i]=bytes((std::string("tools/ui_holo_test/fixtures/ps_")+names[i]+".dxbc").c_str());check(!code[i].empty(),"fixture readable");
        ck(d->CreatePixelShader(code[i].data(),code[i].size(),nullptr,&stock[i]));}
    Cache cache;
    for(int i=0;i<3;++i){const uint64_t h=edvrHash(code[i]);check(index(h)==i,"the program table lists the three in order");
        check(!cache.remember(stock[i].Get(),h,code[i].data(),code[i].size(),true),"a linked original is refused");
        check(cache.remember(stock[i].Get(),h,code[i].data(),code[i].size(),false),"every listed program is captured");
        check(cache.remember(stock[i].Get(),h,code[i].data(),code[i].size(),false),"capture is idempotent");}
    for(int i=0;i<3;++i){prepared[i]=cache.prepare(c,stock[i].Get(),edvrHash(code[i]));check(prepared[i]&&cache.constants(),"every program prepares against one shared b13 buffer");}
    check(prepared[0]!=prepared[1]&&prepared[1]!=prepared[2]&&prepared[0]!=prepared[2],"three distinct patched programs");
    for(int i=0;i<3;++i)check(cache.prepare(c,stock[i].Get(),edvrHash(code[i]))==prepared[i].Get(),"a prepared program is reused from its own slot");
    for(int i=0;i<3;++i)for(int j=0;j<3;++j)if(i!=j)check(!cache.prepare(c,stock[i].Get(),edvrHash(code[j])),"a program never prepares under another's identity");
    check(!cache.remember(stock[2].Get(),0x1234,code[2].data(),code[2].size(),false)&&!cache.prepare(c,stock[2].Get(),0x1234),"an unlisted hash is neither captured nor prepared");
    // The decision: which taken draws ask for the remap at all.
    const uint64_t other=0x1234567890ABCDEFull;
    check(needsRemap(kVs,kPrograms[0].ps,true)&&needsRemap(kVs,kPrograms[1].ps,true),"a sphere-VS draw into the HDR layer asks for the remap");
    check(needsRemap(kVs,other,true),"a sphere-VS draw with an unknown PS still asks, so prepare() can refuse it to stock");
    check(!needsRemap(kVs,kPrograms[0].ps,false)&&!needsRemap(other,kPrograms[1].ps,true)&&!needsRemap(other,other,true)&&!needsRemap(other,other,false),"hologram programs off their take, and unlisted draws, never ask");
    check(needsRemap(other,kFrostedPs,false)&&needsRemap(other,kFrostedPs,true)&&needsRemap(kVs,kFrostedPs,false),"the frosted base asks whatever draws it and wherever it lands");
    check(pair(kVs,kPrograms[0].ps)&&!pair(kVs,kFrostedPs)&&!pair(other,kPrograms[0].ps),"pair() stays the hologram pair");
}
// The wiring nothing else can see: ui_layer.cpp asks needsRemap before it prepares, and the old sphere-VS-only
// shape is gone. A text scan, the way the after-UI rig scans uiLayerNoteOther (the rig runs from the repo root).
inline void wiring() {
    std::ifstream in("src/d3d11/ui_layer.cpp",std::ios::binary);check(bool(in),"src/d3d11/ui_layer.cpp is readable from the working directory");
    const std::string text((std::istreambuf_iterator<char>(in)),std::istreambuf_iterator<char>());
    const size_t decide=text.find("bool uiLayerDecide(ID3D11DeviceContext* ctx, int familyInt");
    check(decide!=std::string::npos,"uiLayerDecide found");
    const size_t need=text.find("ui_holo_remap::needsRemap(",decide),prep=text.find("g_holoCache.prepare(",decide);
    check(need!=std::string::npos&&prep!=std::string::npos&&need<prep,"uiLayerDecide asks needsRemap before it prepares a program");
    check(text.find("== ui_holo_remap::kVs")==std::string::npos,"no hologram-only gate is left in front of the remap");
    check(text.substr(need,prep-need).find("f.crispHdr")!=std::string::npos,"needsRemap is told whether the draw is an HDR take");
}
inline void run(ID3D11Device* d,ID3D11DeviceContext* c) {
    const auto stockBytes=bytes(kFixture),hologramBytes=bytes("tools/ui_holo_test/fixtures/ps_EA02FAC2BD6C643C.dxbc");
    check(stockBytes.size()==2292&&edvrHash(stockBytes)==kFrostedPs,"the saved frosted PS is the game's 2292 bytes");
    structure(stockBytes,hologramBytes);slots(d,c);wiring();
    const Pipe pipe=makePipe(d,c);
    ComPtr<ID3D11PixelShader> stock;ck(d->CreatePixelShader(stockBytes.data(),stockBytes.size(),nullptr,&stock));
    Cache cache;check(cache.remember(stock.Get(),kFrostedPs,stockBytes.data(),stockBytes.size(),false),"actual bytes captured");
    ComPtr<ID3D11PixelShader> patched=cache.prepare(c,stock.Get(),kFrostedPs);check(patched&&cache.constants(),"production cache prepares the frosted base");
    // The game's own frame first: the stock program at its own target reads (pixel + .5) / size, so the
    // constants above are right and the reading is what the shader really does.
    {
        const Scene g{96,72,96,72,0,0,{0,0,96,72,0,1},{0,0,96,72},D3D11_TEXTURE_ADDRESS_MIRROR};
        auto cb1=cb(d,gameConstants(g.gw,g.gh));const auto px=render(d,c,pipe,g.lw,g.lh,g.vp,g.sc,g.mode,cb1.Get(),stock.Get(),nullptr,nullptr,nullptr);
        double worst=0;unsigned undrawn=0;
        for(unsigned y=0;y<g.lh;++y)for(unsigned x=0;x<g.lw;++x){const float* p=&px[(size_t(y)*g.lw+x)*4];if(p[3]<=.9f)++undrawn;
            const double u=(x+.5)/g.gw,v=(y+.5)/g.gh;if(u<2.0/kRamp||u>1-2.0/kRamp||v<2.0/kRamp||v>1-2.0/kRamp)continue;worst=std::max({worst,std::fabs(p[0]-u),std::fabs(p[1]-v)});}
        check(undrawn==0,"the game's own draw covers its target");
        check(worst<kTol,"the stock program at the game's own target reads (pixel + .5) / size");
    }
    unsigned draws=0,pixels=0,defects=0,folds=0;double worstPatched=0,worstStock=0;
    for(const Scene& s:scenes()){
        const Mapped m=mapScene(s);Params p{};check(params(s.gw,s.gh,s.lw,s.lh,s.jx,s.jy,p),"production params from the game's size, the layer's and the jitter");
        auto cb1=cb(d,gameConstants(s.gw,s.gh));
        const auto fixed=render(d,c,pipe,s.lw,s.lh,m.vp,m.sc,s.mode,cb1.Get(),stock.Get(),patched.Get(),cache.constants(),&p);
        const auto bad=render(d,c,pipe,s.lw,s.lh,m.vp,m.sc,s.mode,cb1.Get(),stock.Get(),nullptr,nullptr,nullptr);
        const Reading rf=read(s,m,fixed),rb=read(s,m,bad);++draws;
        check(rf.interior>=100,"the remapped draw reads a real rectangle");
        check(rf.missing==0&&rf.stray==0,"the remapped draw covers exactly the mapped viewport and scissor");
        check(rf.worst<kTol,"the remapped program reads the game-space UV of every pixel of the layer's rect");
        pixels+=rf.interior;worstPatched=std::max(worstPatched,rf.worst);
        const double scale=std::max(double(s.lw)/s.gw,double(s.lh)/s.gh);
        if(scale>1.2){
            check(rb.worst>.05,"the stock program in the layer reads a UV that is off (the flight's overshoot)");
            ++defects;worstStock=std::max(worstStock,rb.worst);
            if(s.mode==D3D11_TEXTURE_ADDRESS_MIRROR){check(rb.folded,"a mirror-addressed lookup folds the blurred scene back on itself");++folds;}
        }
    }
    std::printf("frosted base: %u layer draws x remapped/stock; stock reads up to %.3f of UV off in %u scenes (%u fold back under a mirror sampler); remapped worst error %.2e over %u pixels\n",draws,worstStock,defects,folds,worstPatched,pixels);
}
} // namespace frostedBase
