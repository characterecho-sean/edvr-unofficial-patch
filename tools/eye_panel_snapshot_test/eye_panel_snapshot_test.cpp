#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "../../src/d3d11/eye_panel_snapshot.h"
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <array>
#include <string>
using Microsoft::WRL::ComPtr;
static void hr(HRESULT x,const char* s){if(FAILED(x)){std::fprintf(stderr,"FAIL %s 0x%08lx\n",s,(unsigned long)x);std::exit(1);}}
static void ok(bool x,const char* s){if(!x){std::fprintf(stderr,"FAIL %s\n",s);std::exit(1);}}
static void event(ID3D11Device*d,ID3D11DeviceContext*c){ComPtr<ID3D11Query>q;D3D11_QUERY_DESC dsc{D3D11_QUERY_EVENT,0};hr(d->CreateQuery(&dsc,&q),"event");c->End(q.Get());c->Flush();for(ULONGLONG end=GetTickCount64()+15000;;){BOOL done=0;HRESULT x=c->GetData(q.Get(),&done,sizeof(done),D3D11_ASYNC_GETDATA_DONOTFLUSH);if(x==S_OK&&done)return;if(x!=S_FALSE||GetTickCount64()>end)hr(x==S_FALSE?DXGI_ERROR_WAS_STILL_DRAWING:x,"event wait");Sleep(1);}}
static ComPtr<ID3D11Buffer> buf(ID3D11Device*d,const void*p,size_t n,UINT bind,UINT misc=0,UINT stride=0){D3D11_BUFFER_DESC b{};ok(n<=UINT_MAX,"fixture buffer size");b.ByteWidth=UINT(n);b.Usage=D3D11_USAGE_DEFAULT;b.BindFlags=bind;b.MiscFlags=misc;b.StructureByteStride=stride;D3D11_SUBRESOURCE_DATA i{p,0,0};ComPtr<ID3D11Buffer>o;hr(d->CreateBuffer(&b,&i,&o),"buffer");return o;}
static std::vector<uint8_t> fill(size_t n,uint8_t seed){std::vector<uint8_t>x(n);for(size_t i=0;i<n;++i)x[i]=uint8_t(seed+i*13);return x;}
static ComPtr<ID3D11ShaderResourceView> srv(ID3D11Device*d,ID3D11Resource*r,DXGI_FORMAT f,UINT m,UINT first=0){D3D11_SHADER_RESOURCE_VIEW_DESC v{};v.Format=f;v.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D;v.Texture2D.MostDetailedMip=first;v.Texture2D.MipLevels=m;ComPtr<ID3D11ShaderResourceView>o;hr(d->CreateShaderResourceView(r,&v,&o),"texture SRV");return o;}
static ComPtr<ID3D11ShaderResourceView> structured(ID3D11Device*d,ID3D11Buffer*b,UINT count){D3D11_SHADER_RESOURCE_VIEW_DESC v{};v.Format=DXGI_FORMAT_UNKNOWN;v.ViewDimension=D3D11_SRV_DIMENSION_BUFFER;v.Buffer.FirstElement=1;v.Buffer.NumElements=count-1;ComPtr<ID3D11ShaderResourceView>o;hr(d->CreateShaderResourceView(b,&v,&o),"structured SRV");return o;}
static ComPtr<ID3D11Texture2D> tex(ID3D11Device*d,UINT w,UINT h,UINT m,DXGI_FORMAT f,const std::vector<D3D11_SUBRESOURCE_DATA>& init,UINT bind){D3D11_TEXTURE2D_DESC x{};x.Width=w;x.Height=h;x.MipLevels=m;x.ArraySize=1;x.Format=f;x.SampleDesc.Count=1;x.BindFlags=bind;ComPtr<ID3D11Texture2D>o;hr(d->CreateTexture2D(&x,init.empty()?nullptr:init.data(),&o),"texture");return o;}
static void writePath(edvr::EyePanelSnapshot&s,ID3D11DeviceContext*c,const wchar_t*p){ok(s.write(c,p),"snapshot write");}
int wmain(int argc,wchar_t**argv){if(argc!=2){std::puts("fixture path required");return 2;}
 ComPtr<ID3D11Device>d;ComPtr<ID3D11DeviceContext>c;hr(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&d,nullptr,&c),"WARP");constexpr UINT W=1504,H=1504;
 D3D11_TEXTURE2D_DESC rd{};rd.Width=W;rd.Height=H;rd.MipLevels=1;rd.ArraySize=1;rd.Format=DXGI_FORMAT_R11G11B10_FLOAT;rd.SampleDesc.Count=1;rd.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;ComPtr<ID3D11Texture2D>target;hr(d->CreateTexture2D(&rd,nullptr,&target),"target");D3D11_RENDER_TARGET_VIEW_DESC rv{};rv.Format=rd.Format;rv.ViewDimension=D3D11_RTV_DIMENSION_TEXTURE2D;ComPtr<ID3D11RenderTargetView>rtv;hr(d->CreateRenderTargetView(target.Get(),&rv,&rtv),"rtv");
 D3D11_TEXTURE2D_DESC dd{};dd.Width=W;dd.Height=H;dd.MipLevels=1;dd.ArraySize=1;dd.Format=DXGI_FORMAT_R32G8X24_TYPELESS;dd.SampleDesc.Count=1;dd.BindFlags=D3D11_BIND_DEPTH_STENCIL;ComPtr<ID3D11Texture2D>depth;hr(d->CreateTexture2D(&dd,nullptr,&depth),"depth");D3D11_DEPTH_STENCIL_VIEW_DESC dv{};dv.Format=DXGI_FORMAT_D32_FLOAT_S8X24_UINT;dv.ViewDimension=D3D11_DSV_DIMENSION_TEXTURE2D;ComPtr<ID3D11DepthStencilView>dsv;hr(d->CreateDepthStencilView(depth.Get(),&dv,&dsv),"dsv");
 auto t0a=fill(16*16*8,0x11),t0b=fill(8*8*8,0x22),t0c=fill(4*4*8,0x33),t0d=fill(2*2*8,0x44),t0e=fill(8,0x55);std::vector<D3D11_SUBRESOURCE_DATA>i0{{t0a.data(),16*8,16*16*8},{t0b.data(),8*8,8*8*8},{t0c.data(),4*8,4*4*8},{t0d.data(),2*8,2*2*8},{t0e.data(),8,8}};auto t0=tex(d.Get(),16,16,5,DXGI_FORMAT_R16G16B16A16_TYPELESS,i0,D3D11_BIND_SHADER_RESOURCE);auto q0=srv(d.Get(),t0.Get(),DXGI_FORMAT_R16G16B16A16_UNORM,UINT_MAX,1);
 auto t1a=fill(4*8,0x61),t1b=fill(1*8,0x62),t1c=fill(1*8,0x63),t1d=fill(1*8,0x64);std::vector<D3D11_SUBRESOURCE_DATA>i1{{t1a.data(),16,32},{t1b.data(),8,8},{t1c.data(),8,8},{t1d.data(),8,8}};auto t1=tex(d.Get(),8,8,4,DXGI_FORMAT_BC1_TYPELESS,i1,D3D11_BIND_SHADER_RESOURCE);auto q1=srv(d.Get(),t1.Get(),DXGI_FORMAT_BC1_UNORM,UINT_MAX);
 auto t2a=fill(7*7*4,0x71),t2b=fill(3*3*4,0x72),t2c=fill(4,0x73);std::vector<D3D11_SUBRESOURCE_DATA>i2{{t2a.data(),7*4,7*7*4},{t2b.data(),3*4,3*3*4},{t2c.data(),4,4}};auto t2=tex(d.Get(),7,7,3,DXGI_FORMAT_R8G8B8A8_TYPELESS,i2,D3D11_BIND_SHADER_RESOURCE);auto q2=srv(d.Get(),t2.Get(),DXGI_FORMAT_R8G8B8A8_UNORM,UINT_MAX);
 const char*vsText="struct I{float3 p:POSITION;float2 uv:TEXCOORD0;float4 j:INSTANCE0;};struct O{float4 p:SV_Position;float2 uv:TEXCOORD0;};O main(I i){O o;o.p=float4(i.p.xy+i.j.xy,0,1);o.uv=i.uv;return o;}";const char*psText="Texture2D<float4>a:register(t0);Texture2D<float4>b:register(t1);Texture2D<float4>c:register(t2);SamplerState s0:register(s0);SamplerState s1:register(s1);float4 main(float4 p:SV_Position,float2 uv:TEXCOORD0):SV_Target{return a.Sample(s0,uv)*0.0001+c.Sample(s1,uv)*0.4;}";ComPtr<ID3DBlob>vcode,pcode,e;hr(D3DCompile(vsText,strlen(vsText),"panel-fixture",nullptr,nullptr,"main","vs_5_0",0,0,&vcode,&e),"compile VS");hr(D3DCompile(psText,strlen(psText),"panel-fixture",nullptr,nullptr,"main","ps_5_0",0,0,&pcode,&e),"compile PS");ComPtr<ID3D11VertexShader>vs;ComPtr<ID3D11PixelShader>ps;hr(d->CreateVertexShader(vcode->GetBufferPointer(),vcode->GetBufferSize(),nullptr,&vs),"VS");hr(d->CreatePixelShader(pcode->GetBufferPointer(),pcode->GetBufferSize(),nullptr,&ps),"PS");
 D3D11_INPUT_ELEMENT_DESC ie[]={{"POSITION",0,DXGI_FORMAT_R32G32B32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0},{"TEXCOORD",0,DXGI_FORMAT_R32G32_FLOAT,0,12,D3D11_INPUT_PER_VERTEX_DATA,0},{"INSTANCE",0,DXGI_FORMAT_R32G32B32A32_FLOAT,1,0,D3D11_INPUT_PER_INSTANCE_DATA,1}};ComPtr<ID3D11InputLayout>layout;hr(d->CreateInputLayout(ie,3,vcode->GetBufferPointer(),vcode->GetBufferSize(),&layout),"layout");edvr::EyePanelSnapshot::rememberShader(edvr::EyePanelSnapshot::kVs,vcode->GetBufferPointer(),vcode->GetBufferSize(),vs.Get());edvr::EyePanelSnapshot::rememberShader(edvr::EyePanelSnapshot::kPs,pcode->GetBufferPointer(),pcode->GetBufferSize(),ps.Get());edvr::EyePanelSnapshot::rememberLayout(layout.Get(),ie,3,edvr::EyePanelSnapshot::kVs);
 auto cb0=fill(192,1),cb1=fill(5376,2),cb2=fill(64,3),pcb1=fill(5232,4),pcb2=fill(208,5);auto b0=buf(d.Get(),cb0.data(),cb0.size(),D3D11_BIND_CONSTANT_BUFFER),b1=buf(d.Get(),cb1.data(),cb1.size(),D3D11_BIND_CONSTANT_BUFFER),b2=buf(d.Get(),cb2.data(),cb2.size(),D3D11_BIND_CONSTANT_BUFFER),pb1=buf(d.Get(),pcb1.data(),pcb1.size(),D3D11_BIND_CONSTANT_BUFFER),pb2=buf(d.Get(),pcb2.data(),pcb2.size(),D3D11_BIND_CONSTANT_BUFFER);
 auto s33=fill(336*4,6),s38=fill(48*8,7);auto t33=buf(d.Get(),s33.data(),s33.size(),D3D11_BIND_SHADER_RESOURCE,D3D11_RESOURCE_MISC_BUFFER_STRUCTURED,336),t38=buf(d.Get(),s38.data(),s38.size(),D3D11_BIND_SHADER_RESOURCE,D3D11_RESOURCE_MISC_BUFFER_STRUCTURED,48);auto v33=structured(d.Get(),t33.Get(),4),v38=structured(d.Get(),t38.Get(),8);
 auto vb0data=fill(1024,8);auto vb1data=fill(512,9);std::vector<uint16_t>ibdata{999,999,999,0,1,2,3};
 const float vertices[4][6]={{-.6f,-.6f,0,0,1,0},{-.6f,.6f,0,0,0,0},{.6f,-.6f,0,1,1,0},{.6f,.6f,0,1,0,0}};
 for(UINT j=0;j<4;++j)memcpy(vb0data.data()+12+(j+1)*24,vertices[j],24);
 for(UINT j=0;j<2;++j){float instance[4]={j?.1f:-.1f,0,0,0};memcpy(vb1data.data()+16+(j+3)*16,instance,16);}
auto vb0=buf(d.Get(),vb0data.data(),vb0data.size(),D3D11_BIND_VERTEX_BUFFER),vb1=buf(d.Get(),vb1data.data(),vb1data.size(),D3D11_BIND_VERTEX_BUFFER),ib=buf(d.Get(),ibdata.data(),(UINT)ibdata.size()*2,D3D11_BIND_INDEX_BUFFER);
 D3D11_SAMPLER_DESC sm{};sm.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;sm.AddressU=sm.AddressV=sm.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;sm.ComparisonFunc=D3D11_COMPARISON_NEVER;sm.MaxLOD=D3D11_FLOAT32_MAX;ComPtr<ID3D11SamplerState>samp0,samp1;hr(d->CreateSamplerState(&sm,&samp0),"sampler0");sm.Filter=D3D11_FILTER_MIN_MAG_MIP_POINT;hr(d->CreateSamplerState(&sm,&samp1),"sampler1");
 D3D11_DEPTH_STENCIL_DESC dsd{};dsd.DepthEnable=TRUE;dsd.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ZERO;dsd.DepthFunc=D3D11_COMPARISON_ALWAYS;dsd.StencilEnable=TRUE;dsd.StencilReadMask=dsd.StencilWriteMask=255;
 dsd.FrontFace.StencilFailOp=dsd.FrontFace.StencilDepthFailOp=dsd.FrontFace.StencilPassOp=D3D11_STENCIL_OP_KEEP;
 dsd.BackFace=dsd.FrontFace;dsd.FrontFace.StencilFunc=D3D11_COMPARISON_ALWAYS;dsd.BackFace.StencilFunc=D3D11_COMPARISON_ALWAYS;ComPtr<ID3D11DepthStencilState>depthState;hr(d->CreateDepthStencilState(&dsd,&depthState),"depth state");
 c->OMSetRenderTargets(1,rtv.GetAddressOf(),dsv.Get());c->OMSetDepthStencilState(depthState.Get(),5);D3D11_VIEWPORT vp{0,0,(float)W,(float)H,0,1};c->RSSetViewports(1,&vp);
 D3D11_RASTERIZER_DESC rasterDesc{};rasterDesc.FillMode=D3D11_FILL_SOLID;rasterDesc.CullMode=D3D11_CULL_NONE;rasterDesc.DepthClipEnable=TRUE;
 ComPtr<ID3D11RasterizerState> raster;hr(d->CreateRasterizerState(&rasterDesc,&raster),"raster");c->RSSetState(raster.Get());
 D3D11_RECT scissor{0,0,W,H};c->RSSetScissorRects(1,&scissor);c->IASetInputLayout(layout.Get());c->VSSetShader(vs.Get(),nullptr,0);c->PSSetShader(ps.Get(),nullptr,0);ID3D11Buffer*vc[]={b0.Get(),b1.Get(),b2.Get()};c->VSSetConstantBuffers(0,3,vc);ID3D11Buffer*pc[]={pb1.Get(),pb2.Get()};c->PSSetConstantBuffers(1,2,pc);ID3D11ShaderResourceView*psr[]={q0.Get(),q1.Get(),q2.Get()};c->PSSetShaderResources(0,3,psr);ID3D11ShaderResourceView*vsr[]={v33.Get()};c->VSSetShaderResources(33,1,vsr);vsr[0]=v38.Get();c->VSSetShaderResources(38,1,vsr);ID3D11SamplerState*ss[]={samp0.Get(),samp1.Get()};c->PSSetSamplers(0,2,ss);UINT strides[]={24,16},offsets[]={12,16};ID3D11Buffer*vb[]={vb0.Get(),vb1.Get()};c->IASetVertexBuffers(0,2,vb,strides,offsets);c->IASetIndexBuffer(ib.Get(),DXGI_FORMAT_R16_UINT,4);c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
 FLOAT clear[4]={0,0,0,1};c->ClearRenderTargetView(rtv.Get(),clear);c->ClearDepthStencilView(dsv.Get(),D3D11_CLEAR_DEPTH|D3D11_CLEAR_STENCIL,0.25f,5);
 edvr::EyePanelSnapshot snap;snap.captureRequested(c.Get(),9000,9010,9001,37,edvr::EyePanelSnapshot::kVs,edvr::EyePanelSnapshot::kPs,'X',4,2,3,1,1);ok(snap.count()==1,"capture accepted");c->DrawIndexedInstanced(4,2,1,1,3);event(d.Get(),c.Get());c->OMSetRenderTargets(0,nullptr,nullptr);snap.end(c.Get());c->OMSetRenderTargets(1,rtv.GetAddressOf(),dsv.Get());event(d.Get(),c.Get());

 // Every input is overwritten after the captured draw. The producer must
 // retain the draw-local version, including all mips and structured buffers.
 auto poisonBuffer=[&](ID3D11Buffer* b){D3D11_BUFFER_DESC z{};b->GetDesc(&z);std::vector<uint8_t> poison(z.ByteWidth,0xEF);c->UpdateSubresource(b,0,nullptr,poison.data(),0,0);};
 for(auto b:{b0.Get(),b1.Get(),b2.Get(),pb1.Get(),pb2.Get(),t33.Get(),t38.Get(),vb0.Get(),vb1.Get(),ib.Get()})poisonBuffer(b);
 auto poisonTexture=[&](ID3D11Texture2D* t,UINT unit,bool bc){D3D11_TEXTURE2D_DESC z{};t->GetDesc(&z);for(UINT m=0;m<z.MipLevels;++m){UINT w=std::max(1u,z.Width>>m),h=std::max(1u,z.Height>>m);UINT row=(bc?(w+3)/4:w)*unit,rows=bc?(h+3)/4:h;std::vector<uint8_t> poison(row*rows,0xEF);c->UpdateSubresource(t,m,nullptr,poison.data(),row,row*rows);}};
 poisonTexture(t0.Get(),8,false);poisonTexture(t1.Get(),8,true);poisonTexture(t2.Get(),4,false);
 clear[0]=.7f;clear[1]=.8f;clear[2]=.9f;c->ClearRenderTargetView(rtv.Get(),clear);
 c->ClearDepthStencilView(dsv.Get(),D3D11_CLEAR_DEPTH|D3D11_CLEAR_STENCIL,.9f,99);snap.end(c.Get());
 // A later frame and a different eye cannot add snapshots or replace target.
 snap.captureRequested(c.Get(),9000,9010,9002,38,edvr::EyePanelSnapshot::kVs,edvr::EyePanelSnapshot::kPs,'X',4,2,3,1,1);
 auto smallDesc=rd;smallDesc.Width=smallDesc.Height=16;ComPtr<ID3D11Texture2D> other;
 hr(d->CreateTexture2D(&smallDesc,nullptr,&other),"other eye");ComPtr<ID3D11RenderTargetView> otherRtv;
 hr(d->CreateRenderTargetView(other.Get(),&rv,&otherRtv),"other eye RTV");c->OMSetRenderTargets(1,otherRtv.GetAddressOf(),nullptr);
 snap.captureRequested(c.Get(),9000,9010,9001,39,edvr::EyePanelSnapshot::kVs,edvr::EyePanelSnapshot::kPs,'X',4,2,3,1,1);
 ok(snap.count()==1 && snap.ignoredOtherEye==1 && snap.ignoredOtherFrame==1,"one frame and eye policy");
 c->OMSetRenderTargets(1,rtv.GetAddressOf(),dsv.Get());event(d.Get(),c.Get());writePath(snap,c.Get(),argv[1]);ok(snap.failures==0 && snap.declined==0,"complete fixture readback");snap.reset();
 // Restore valid indexed/instanced geometry for incomplete-state fixtures.
 c->UpdateSubresource(ib.Get(),0,nullptr,ibdata.data(),0,0);c->UpdateSubresource(vb0.Get(),0,nullptr,vb0data.data(),0,0);c->UpdateSubresource(vb1.Get(),0,nullptr,vb1data.data(),0,0);
 edvr::EyePanelSnapshot pending;pending.captureRequested(c.Get(),9000,9010,9001,40,edvr::EyePanelSnapshot::kVs,edvr::EyePanelSnapshot::kPs,'X',4,2,3,1,1);
 event(d.Get(),c.Get());std::wstring pp=std::wstring(argv[1])+L".pending.bin";writePath(pending,c.Get(),pp.c_str());ok(pending.failures==1,"missing end is one failed payload");pending.reset();
 ID3D11Buffer* nullcb=nullptr;c->PSSetConstantBuffers(1,1,&nullcb);edvr::EyePanelSnapshot incomplete;
 incomplete.captureRequested(c.Get(),9000,9010,9001,41,edvr::EyePanelSnapshot::kVs,edvr::EyePanelSnapshot::kPs,'X',4,2,3,1,1);
 incomplete.end(c.Get());event(d.Get(),c.Get());std::wstring ip=std::wstring(argv[1])+L".incomplete.bin";
 writePath(incomplete,c.Get(),ip.c_str());ok(incomplete.declined==1,"missing PS b1 is incomplete");incomplete.reset();
 ID3D11Buffer* restored=pb1.Get();c->PSSetConstantBuffers(1,1,&restored);
 // No GPU draw is issued with this deliberately invalid index. The parser
 // must reject replay geometry even though every captured byte is present.
 ibdata[4]=60000;c->UpdateSubresource(ib.Get(),0,nullptr,ibdata.data(),0,0);edvr::EyePanelSnapshot range;
 range.captureRequested(c.Get(),9000,9010,9001,42,edvr::EyePanelSnapshot::kVs,edvr::EyePanelSnapshot::kPs,'X',4,2,3,1,1);
 range.end(c.Get());event(d.Get(),c.Get());std::wstring rp=std::wstring(argv[1])+L".range.bin";writePath(range,c.Get(),rp.c_str());range.reset();
 c->OMSetRenderTargets(1,otherRtv.GetAddressOf(),nullptr);edvr::EyePanelSnapshot cap;
 for(UINT n=0;n<17;++n){cap.captureRequested(c.Get(),9000,9010,9001,50+n,edvr::EyePanelSnapshot::kVs,edvr::EyePanelSnapshot::kPs,'X',4,2,3,1,1);cap.end(c.Get());}
 ok(cap.count()==16 && cap.declined==1,"sixteen draw cap");cap.reset();event(d.Get(),c.Get());
 std::printf("WARP panel fixture PASS (production capture, source overwrites, missing data and caps) count_after_reset=%u failures=%u output=%ls\n",snap.count(),snap.failures,argv[1]);return 0;}
