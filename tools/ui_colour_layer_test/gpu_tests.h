// Direct GPU tests of production MRT blending and current-UI reconstruction.
#include "../../src/d3d11/ui_resolve.h"
#include <cmath>
#include <cstdlib>
static unsigned checks=0;
static void check(bool ok,const char* why){++checks;if(!ok){std::fprintf(stderr,"FAIL: %s\n",why);std::exit(1);}}
static void hr(HRESULT h){check(SUCCEEDED(h),"D3D operation");}
static ComPtr<ID3DBlob> compile(const char* text,const char* profile){
 ComPtr<ID3DBlob>b,e;auto h=D3DCompile(text,strlen(text),nullptr,nullptr,nullptr,"main",profile,D3DCOMPILE_ENABLE_STRICTNESS,0,&b,&e);
 if(FAILED(h)&&e)std::fprintf(stderr,"%s",static_cast<const char*>(e->GetBufferPointer()));hr(h);return b;
}
struct Surface{
 ComPtr<ID3D11Texture2D> tex;ComPtr<ID3D11RenderTargetView> rtv;ComPtr<ID3D11ShaderResourceView> srv;ComPtr<ID3D11UnorderedAccessView> uav;
};
static Surface surface(ID3D11Device*d,DXGI_FORMAT format,bool compute=false){
 Surface s;D3D11_TEXTURE2D_DESC td{};td.Width=td.Height=8;td.MipLevels=td.ArraySize=1;td.SampleDesc.Count=1;td.Format=format;
 td.BindFlags=D3D11_BIND_SHADER_RESOURCE|(compute?D3D11_BIND_UNORDERED_ACCESS:D3D11_BIND_RENDER_TARGET);
 hr(d->CreateTexture2D(&td,nullptr,&s.tex));hr(d->CreateShaderResourceView(s.tex.Get(),nullptr,&s.srv));
 if(compute)hr(d->CreateUnorderedAccessView(s.tex.Get(),nullptr,&s.uav));else hr(d->CreateRenderTargetView(s.tex.Get(),nullptr,&s.rtv));return s;
}
static std::vector<BYTE> read(ID3D11Device*d,ID3D11DeviceContext*c,ID3D11Resource*res){
 ComPtr<ID3D11Texture2D> t;hr(res->QueryInterface(IID_PPV_ARGS(&t)));D3D11_TEXTURE2D_DESC td{};t->GetDesc(&td);
 unsigned stride=td.Format==DXGI_FORMAT_R32G32B32A32_FLOAT?16:4;td.Usage=D3D11_USAGE_STAGING;td.BindFlags=td.MiscFlags=0;td.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
 ComPtr<ID3D11Texture2D>s;hr(d->CreateTexture2D(&td,nullptr,&s));c->CopyResource(s.Get(),t.Get());D3D11_MAPPED_SUBRESOURCE m{};hr(c->Map(s.Get(),0,D3D11_MAP_READ,0,&m));
 std::vector<BYTE> out(64*stride);for(unsigned y=0;y<8;++y)memcpy(out.data()+y*8*stride,static_cast<BYTE*>(m.pData)+y*m.RowPitch,8*stride);c->Unmap(s.Get(),0);return out;
}
static ComPtr<ID3D11BlendState> blend(ID3D11Device*d,bool enabled,D3D11_BLEND dest,D3D11_BLEND src=D3D11_BLEND_ONE){
 D3D11_BLEND_DESC bd{};auto&r=bd.RenderTarget[0];r.BlendEnable=enabled;r.SrcBlend=src;r.DestBlend=dest;r.BlendOp=D3D11_BLEND_OP_ADD;
 r.SrcBlendAlpha=D3D11_BLEND_ONE;r.DestBlendAlpha=D3D11_BLEND_ZERO;r.BlendOpAlpha=D3D11_BLEND_OP_ADD;r.RenderTargetWriteMask=7;
 ComPtr<ID3D11BlendState>b;hr(d->CreateBlendState(&bd,&b));return b;
}
static void runGpu(ID3D11Device*d,ID3D11DeviceContext*c){
 auto code=compile("cbuffer C:register(b0){float4 colour;float4 cut;}float4 main(float4 p:SV_Position):SV_Target{if(p.x<cut.x)discard;return colour;}","ps_5_0");
 std::vector<BYTE> patched;std::string why;check(edvr::uiColourFanout(code->GetBufferPointer(),code->GetBufferSize(),patched,why),why.c_str());
 ComPtr<ID3D11PixelShader>original,fanout;hr(d->CreatePixelShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&original));hr(d->CreatePixelShader(patched.data(),patched.size(),nullptr,&fanout));
 auto constant=compile("float4 main():SV_Target{return float4(1,.5,.25,1);}","ps_5_0");
 check(edvr::uiColourFanout(constant->GetBufferPointer(),constant->GetBufferSize(),patched,why),"constant shader without temps");
 ComPtr<ID3D11PixelShader>ps;hr(d->CreatePixelShader(patched.data(),patched.size(),nullptr,&ps));
 auto mrt=compile("struct O{float4 a:SV_Target0;float4 b:SV_Target1;};O main(){O o;o.a=1;o.b=0;return o;}","ps_5_0");
 check(!edvr::uiColourFanout(mrt->GetBufferPointer(),mrt->GetBufferSize(),patched,why),"existing MRT shader rejected");
 auto vsCode=compile("float4 main(uint i:SV_VertexID):SV_Position{return float4(i==2?3:-1,i==1?3:-1,.5,1);}","vs_5_0");
 ComPtr<ID3D11VertexShader>vs;hr(d->CreateVertexShader(vsCode->GetBufferPointer(),vsCode->GetBufferSize(),nullptr,&vs));c->VSSetShader(vs.Get(),nullptr,0);c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
 D3D11_VIEWPORT vp{0,0,8,8,0,1};c->RSSetViewports(1,&vp);D3D11_RASTERIZER_DESC rd{};rd.FillMode=D3D11_FILL_SOLID;rd.CullMode=D3D11_CULL_NONE;rd.ScissorEnable=TRUE;rd.DepthClipEnable=TRUE;
 ComPtr<ID3D11RasterizerState>rs;hr(d->CreateRasterizerState(&rd,&rs));c->RSSetState(rs.Get());D3D11_BUFFER_DESC bd{};bd.ByteWidth=32;bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
 ComPtr<ID3D11Buffer>cb;hr(d->CreateBuffer(&bd,nullptr,&cb));auto*rawCb=cb.Get();c->PSSetConstantBuffers(0,1,&rawCb);
 auto over=blend(d,true,D3D11_BLEND_INV_SRC_ALPHA),opaque=blend(d,false,D3D11_BLEND_ZERO),add=blend(d,true,D3D11_BLEND_ONE);
 auto game=surface(d,DXGI_FORMAT_R11G11B10_FLOAT),baseline=surface(d,DXGI_FORMAT_R11G11B10_FLOAT),world=surface(d,DXGI_FORMAT_R11G11B10_FLOAT);
 const float bg[4]={.25f,.5f,.75f,1};for(auto*s:{&game,&baseline,&world})c->ClearRenderTargetView(s->rtv.Get(),bg);
 edvr::UiColourLayer layer;check(layer.seed(c,game.rtv.Get()),"seed");
 struct Draw{float rgba[4];float cut;LONG right;ID3D11BlendState* blend;bool ui;};
 Draw sequence[]={{{.2f,.4f,.6f,1.2f},2,8,over.Get(),true},{{.1f,.2f,.3f,1},0,4,opaque.Get(),false},{{.05f,.1f,.15f,.25f},0,8,over.Get(),false},{{.02f,.04f,.08f,1},0,8,add.Get(),false}};
 auto draw=[&](Surface&s,const Draw&x,bool separate){
  auto*rt=s.rtv.Get();c->OMSetRenderTargets(1,&rt,nullptr);c->OMSetBlendState(x.blend,nullptr,~0u);c->PSSetShader(original.Get(),nullptr,0);
  float values[8]={x.rgba[0],x.rgba[1],x.rgba[2],x.rgba[3],x.cut,0,0,0};c->UpdateSubresource(cb.Get(),0,nullptr,values,0,0);D3D11_RECT rect{0,0,x.right,8};c->RSSetScissorRects(1,&rect);
  if(separate)check(layer.begin(c,fanout.Get(),x.ui),"MRT begin");c->Draw(3,0);if(separate)layer.end(c);
  ComPtr<ID3D11PixelShader>got;ComPtr<ID3D11BlendState>bs;ComPtr<ID3D11RenderTargetView>target;
  c->PSGetShader(&got,nullptr,nullptr);c->OMGetBlendState(&bs,nullptr,nullptr);c->OMGetRenderTargets(1,&target,nullptr);
  check(got.Get()==original.Get()&&bs.Get()==x.blend&&target.Get()==rt,"state restored");
 };
 for(auto&x:sequence){draw(baseline,x,false);if(!x.ui)draw(world,x,false);draw(game,x,true);}
 c->OMSetRenderTargets(0,nullptr,nullptr);
 check(read(d,c,game.tex.Get())==read(d,c,baseline.tex.Get()),"RT0 bit-identical to original sequence");
 check(read(d,c,layer.colour())==read(d,c,world.tex.Get()),"all later world draws preserved");
 check(read(d,c,game.tex.Get())!=read(d,c,world.tex.Get()),"nonzero UI colour");
 ComPtr<ID3D11Resource>aResource;layer.influenceView()->GetResource(&aResource);auto bytes=read(d,c,aResource.Get());
 for(unsigned y=0;y<8;++y)for(unsigned x=0;x<8;++x){float a;memcpy(&a,bytes.data()+4*(y*8+x),4);check(std::abs(a-(x<4?0.f:.9f))<1e-5f,"unclamped influence and world occlusion");}
 auto out=surface(d,DXGI_FORMAT_R32G32B32A32_FLOAT,true),history=surface(d,DXGI_FORMAT_R32G32B32A32_FLOAT,true);
 auto csCode=compile(edvr::kUiResolve,"cs_5_0");ComPtr<ID3D11ComputeShader>cs;hr(d->CreateComputeShader(csCode->GetBufferPointer(),csCode->GetBufferSize(),nullptr,&cs));
 struct Params{int region[4]={0,0,8,8};int size[2]={8,8};int texSize[2]={8,8};float now[4]{},prev[4]{},jit[4]{};}params;
 bd.ByteWidth=sizeof(params);ComPtr<ID3D11Buffer>pcb;D3D11_SUBRESOURCE_DATA initial{&params,0,0};hr(d->CreateBuffer(&bd,&initial,&pcb));rawCb=pcb.Get();c->CSSetConstantBuffers(0,1,&rawCb);
 ID3D11ShaderResourceView*srvs[9]={layer.colourView(),world.srv.Get(),nullptr,nullptr,nullptr,nullptr,nullptr,game.srv.Get(),layer.influenceView()};
 ID3D11UnorderedAccessView*uavs[2]={out.uav.Get(),history.uav.Get()},*nullUav[2]{};ID3D11ShaderResourceView*nullSrv[9]{};
 auto dispatch=[&](){c->CSSetShader(cs.Get(),nullptr,0);c->CSSetShaderResources(0,9,srvs);c->CSSetUnorderedAccessViews(0,2,uavs,nullptr);c->Dispatch(1,1,1);c->CSSetShaderResources(0,9,nullSrv);c->CSSetUnorderedAccessViews(0,2,nullUav,nullptr);};
 dispatch();auto actual=read(d,c,out.tex.Get());srvs[0]=srvs[1]=game.srv.Get();srvs[7]=srvs[8]=nullptr;dispatch();
 check(actual==read(d,c,out.tex.Get()),"production composite identity restores current full colour");
 layer.frameBoundary();check(!layer.ready()&&!layer.colourView(),"frame boundary invalidates clean colour");check(layer.seed(c,game.rtv.Get()),"next frame reseed");
 bytes=read(d,c,aResource.Get());for(auto byte:bytes)check(byte==0,"influence reset");
 auto*rt=game.rtv.Get();c->OMSetRenderTargets(1,&rt,nullptr);auto unsupported=blend(d,true,D3D11_BLEND_INV_SRC_ALPHA,D3D11_BLEND_DEST_COLOR);c->OMSetBlendState(unsupported.Get(),nullptr,~0u);
 check(!layer.begin(c,fanout.Get(),false),"destination-dependent source blend rejected");c->OMSetBlendState(over.Get(),nullptr,~0u);
 ID3D11RenderTargetView*multiple[2]={game.rtv.Get(),world.rtv.Get()};c->OMSetRenderTargets(2,multiple,nullptr);check(!layer.begin(c,fanout.Get(),true),"existing MRT state rejected");
 rt=world.rtv.Get();c->OMSetRenderTargets(1,&rt,nullptr);check(!layer.begin(c,fanout.Get(),true),"wrong source identity rejected");
 printf("Production MRT/composite GPU checks: %u passed.\n",checks);
}

