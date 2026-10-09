#pragma once
#include "../../src/d3d11/eye_final_capture.h"
#include <vector>
namespace finalCaptureFaultFixture {
struct Object{void** table;unsigned releases=0;};
inline Object *device=nullptr,*partial=nullptr;inline unsigned mode=0;
inline ULONG STDMETHODCALLTYPE release(Object* object){++object->releases;if(mode==4&&object==device)RaiseException(0xE042ED74u,0,0,nullptr);return 1;}
inline void STDMETHODCALLTYPE getDevice(Object*,ID3D11Device** out){*out=reinterpret_cast<ID3D11Device*>(device);if(mode==1)RaiseException(0xE042ED71u,0,0,nullptr);}
inline void STDMETHODCALLTYPE getOwner(Object*,ID3D11Device** out){*out=reinterpret_cast<ID3D11Device*>(device);if(mode==5)RaiseException(0xE042ED75u,0,0,nullptr);}
inline void STDMETHODCALLTYPE getDesc(Object*,D3D11_TEXTURE2D_DESC* d){*d={};d->Width=32;d->Height=24;d->MipLevels=d->ArraySize=d->SampleDesc.Count=1;d->Format=DXGI_FORMAT_R8G8B8A8_UNORM;}
inline HRESULT STDMETHODCALLTYPE create(Object*,const D3D11_TEXTURE2D_DESC*,const D3D11_SUBRESOURCE_DATA*,ID3D11Texture2D** out){*out=reinterpret_cast<ID3D11Texture2D*>(partial);if(mode==2)RaiseException(0xE042ED72u,0,0,nullptr);return mode==6?S_OK:E_FAIL;}
inline void STDMETHODCALLTYPE copy(Object*,ID3D11Resource*,UINT,UINT,UINT,UINT,ID3D11Resource*,UINT,const D3D11_BOX*){RaiseException(0xE042ED76u,0,0,nullptr);}
inline void run(){
  void* contextTable[47]{};contextTable[3]=reinterpret_cast<void*>(&getDevice);contextTable[46]=reinterpret_cast<void*>(&copy);
  void* deviceTable[6]{};deviceTable[2]=reinterpret_cast<void*>(&release);deviceTable[5]=reinterpret_cast<void*>(&create);
  void* textureTable[11]{};textureTable[2]=reinterpret_cast<void*>(&release);textureTable[3]=reinterpret_cast<void*>(&getOwner);textureTable[10]=reinterpret_cast<void*>(&getDesc);
  Object context{contextTable},dev{deviceTable},source{textureTable},value{textureTable};device=&dev;partial=&value;
  const uint32_t region[4]={0,0,32,24};
  for(unsigned cause:{1u,2u,3u,4u,5u,6u}){
    edvr::eye_final_capture::Run capture;capture.arm();capture.schedule(0,1);mode=cause;dev.releases=value.releases=0;
    capture.capture(1,1,0,reinterpret_cast<ID3D11DeviceContext*>(&context),reinterpret_cast<ID3D11Texture2D*>(&source),region,false,false,false);
    check(!strcmp(capture.rows[0].eye[0].status,cause==3?"stage_failed":"capture_fault"),"production final stage faults and failed HRESULTs are explicit");
    check(!capture.pendingDevice&&!capture.pendingOwner&&bool(capture.captureDevice)==(cause==6),"published device references are cleaned or retained with actual staged artifacts");
    check(dev.releases==(cause==1?2u:cause==6?3u:4u),"GetDevice outputs are released exactly once for crop and overview failure");
    check(capture.bytes==(cause==2||cause==6?2ull*32*24*4:0),"published-then-fault staging stays charged while failed HRESULT releases reservation");
    check(!capture.rows[0].eye[0].writable()&&!capture.overview[0].writable(),"writer refuses retained published staging when capture faulted before copy");
    if(cause==2||cause==6)check(capture.rows[0].eye[0].staging&&!strcmp(capture.rows[0].eye[0].status,"capture_fault"),"unwritable before-copy/copy fault retains explicit failure with nonnull staging");
    capture.reset();check(value.releases==(cause==2||cause==3||cause==6?2u:0u),"partial staged artifacts release once across fault and reset");
  }
  mode=0;device=partial=nullptr;
}
}
inline void runFinalCaptureTests(Device& device,Device& foreign) {
  finalCaptureFaultFixture::run();
  using edvr::eye_final_capture::Run;
  Run run;auto source=device.texture();const uint32_t region[4]={4,3,28,20};
  run.capture(100,1000,0,nullptr,nullptr,nullptr,false,false,false);
  check(!run.count&&!run.bytes&&!run.unmatched,"unarmed final capture touches no context or texture");
  run.arm();run.schedule(0,100);run.schedule(0,100);
  check(run.count==1&&run.duplicates==1,"duplicate schedule preserves original slot");
  run.capture(99,999,0,nullptr,nullptr,nullptr,false,false,false);
  check(run.unmatched==1&&!run.rows[0].eye[0].sequence,"wrong scene never fills pending frame");
  std::vector<uint32_t> pixels(32*24);
  for(unsigned i=0;i<pixels.size();++i)pixels[i]=0xff000000u|i;
  device.context->UpdateSubresource(source.Get(),0,nullptr,pixels.data(),32*4,0);
  ComPtr<ID3D11ShaderResourceView> sentinel;
  require(SUCCEEDED(device.device->CreateShaderResourceView(source.Get(),nullptr,&sentinel)),"final capture state sentinel SRV");
  ID3D11ShaderResourceView* view=sentinel.Get();device.context->PSSetShaderResources(7,1,&view);
  const D3D11_VIEWPORT viewport={2,3,19,17,0.2f,0.8f};device.context->RSSetViewports(1,&viewport);
  run.capture(100,1000,0,device.context.Get(),source.Get(),region,true,true,false);
  auto& image=run.rows[0].eye[0];
  check(image.writable(),"writer admits only a successfully completed staging copy");
  check(image.staging&&image.sequence==1000&&image.composite&&image.flipU&&!image.flipV&&
      image.crop[0]==4&&image.crop[1]==3&&image.crop[2]==28&&image.crop[3]==20,"final crop records exact native region and flipped submit metadata");
  D3D11_MAPPED_SUBRESOURCE map{};
  require(SUCCEEDED(device.context->Map(image.staging,0,D3D11_MAP_READ,0,&map)),"final WARP staging maps");
  for(unsigned y=0;y<17;++y)for(unsigned x=0;x<24;++x)
      check(reinterpret_cast<const uint32_t*>(static_cast<const char*>(map.pData)+y*map.RowPitch)[x]==pixels[(y+3)*32+x+4],"final captured pixel matches actual source crop");
  device.context->Unmap(image.staging,0);
  ComPtr<ID3D11ShaderResourceView> bound;device.context->PSGetShaderResources(7,1,&bound);
  D3D11_VIEWPORT restored{};UINT viewportCount=1;device.context->RSGetViewports(&viewportCount,&restored);
  check(bound.Get()==sentinel.Get()&&viewportCount==1&&!memcmp(&restored,&viewport,sizeof(viewport)),"capture copies preserve immediate shader resource and raster viewport state");
  const auto bytes=run.bytes;
  run.capture(100,1000,0,device.context.Get(),source.Get(),region,false,false,false);
  check(run.bytes==bytes&&run.duplicates==2,"duplicate final callback allocates and copies nothing");
  run.ready=true;check(!run.complete(),"old temporal flush does not discard pending final right");
  for(auto& p:pixels)p^=0x00ffffff;
  device.context->UpdateSubresource(source.Get(),0,nullptr,pixels.data(),32*4,0);
  run.capture(100,1000,1,device.context.Get(),source.Get(),region,false,false,true);
  check(run.complete()&&!run.rows[0].eye[1].composite&&run.rows[0].eye[1].flipV,"passthrough final right completes independent queue after old flush");
  require(SUCCEEDED(device.context->Map(run.rows[0].eye[1].staging,0,D3D11_MAP_READ,0,&map)),"latest-content final staging maps");
  check(*static_cast<const uint32_t*>(map.pData)==pixels[3*32+4],"final callback captures newly updated source contents");
  device.context->Unmap(run.rows[0].eye[1].staging,0);
  run.reset();check(!run.armed&&!run.count&&!run.bytes&&!run.rows[0].eye[0].staging,"shutdown/reset releases capture lifetime");
  run.arm();
  for(unsigned i=0;i<16;++i){run.schedule(i,200+i);run.capture(200+i,2000+i,0,device.context.Get(),source.Get(),region,false,false,false);if(i<15)run.capture(200+i,2000+i,1,device.context.Get(),source.Get(),region,true,false,false);}
  run.ready=true;check(run.count==16&&!run.complete(),"sixteenth final right remains pending after all temporal frames");
  run.capture(215,2015,1,device.context.Get(),source.Get(),region,true,false,false);
  check(run.complete()&&run.rows[15].eye[1].staging,"sixteenth matching right is actually copied");
  run.arm();edvr::eye_final_capture::Clock clock;
  clock.boundary(false);check(clock.epoch==0,"idle capture boundary clock performs no advance");
  const uint32_t frozenScene=42; // actual temporal scene does not advance with AA disabled
  for(unsigned i=0;i<16;++i){
    clock.boundary(run.armed); // same production seam called before the AA-off return
    run.schedule(i,clock.epoch,frozenScene);
    const unsigned first=i%2; // both submission orders
    run.capture(clock.epoch,6000+i,first,device.context.Get(),source.Get(),region,false,false,false);
    if(i<15)run.capture(clock.epoch,6000+i,1-first,device.context.Get(),source.Get(),region,false,false,false);
    check(run.rows[i].epoch==i+1&&run.rows[i].scene==frozenScene&&run.rows[i].eye[first].sequence==6000+i,"active boundary epoch separates AA-off slots with identical actual scene metadata");
  }
  run.ready=true;check(!run.complete(),"AA-off final sixteenth stereo partner remains pending after old flush");
  run.capture(clock.epoch,6015,0,device.context.Get(),source.Get(),region,false,false,false);
  check(run.complete()&&run.rows[15].eye[0].staging,"AA-off matching final partner completes exact boundary and native sequence");
  run.reset();clock.reset();clock.boundary(run.armed);check(clock.epoch==0,"shutdown/rearm clock reset retains no stale boundary key");
  run.arm();check(run.armed&&!run.count&&!run.bytes,"rearm clears old staging and scene/sequence identity");
  run.schedule(0,300);run.capture(300,3000,0,device.context.Get(),source.Get(),region,false,false,false);
  run.capture(300,3001,1,device.context.Get(),source.Get(),region,false,false,false);
  check(!strcmp(run.rows[0].eye[1].status,"sequence_mismatch")&&!run.rows[0].eye[1].staging,"mismatched native stereo sequence never copies wrong final eye");
  run.arm();run.schedule(0,400);auto other=foreign.texture();
  run.capture(400,4000,0,device.context.Get(),other.Get(),region,false,false,false);
  check(!strcmp(run.rows[0].eye[0].status,"device_mismatch")&&!run.bytes,"foreign device declines without allocation or copy");
  run.capture(400,4000,1,nullptr,nullptr,nullptr,false,false,false);
  check(!strcmp(run.rows[0].eye[1].status,"source_missing"),"queue reached with missing source is distinct from never called");
  run.arm();run.schedule(0,450);run.capture(450,4500,0,device.context.Get(),source.Get(),region,false,false,false);
  run.capture(450,4500,1,foreign.context.Get(),other.Get(),region,false,false,false);
  check(!strcmp(run.rows[0].eye[1].status,"device_changed")&&!run.rows[0].eye[1].staging,"device replacement cannot mix devices in a final capture run");
  run.arm();run.schedule(0,500);run.bytes=edvr::eye_final_capture::Budget;
  run.capture(500,5000,0,device.context.Get(),source.Get(),region,false,false,false);
  check(!strcmp(run.rows[0].eye[0].status,"budget")&&!run.rows[0].eye[0].staging,"bounded capture budget refuses before staging creation");
  run.arm();run.schedule(0,600);const uint32_t invalid[4]={0,0,40,24};
  run.capture(600,6000,0,device.context.Get(),source.Get(),invalid,false,false,false);
  check(!strcmp(run.rows[0].eye[0].status,"bounds_invalid")&&!run.bytes,"invalid bounds are recorded without GPU copy");
  run.ready=true;check(!run.complete()&&!strcmp(run.rows[0].eye[1].status,"not_reached"),"missing callback remains distinguishable from copy success");
  check(edvr::eye_final_capture::pixelBytes(DXGI_FORMAT_R32_FLOAT)==0,"unsupported BMP source encoding refused before copy");
  D3D11_TEXTURE2D_DESC d{};source->GetDesc(&d);d.ArraySize=2;
  edvr::eye_final_capture::Image failure;
  run.stage(failure,device.context.Get(),source.Get(),d,region);
  check(!strcmp(failure.status,"source_shape")&&!failure.staging,"array source shape is declined before allocation");
  d.ArraySize=1;d.Width=d.Height=5000;const uint32_t huge[4]={0,0,5000,5000};
  run.stage(failure,device.context.Get(),source.Get(),d,huge);
  check(!strcmp(failure.status,"budget")&&!failure.staging,"single artifact exceeding64MiB is declined before allocation");
  d.Width=4032;d.Height=3896;d.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;const uint32_t halfFloatOverview[4]={0,0,4032,3896};
  run.stage(failure,device.context.Get(),source.Get(),d,halfFloatOverview);
  check(!strcmp(failure.status,"budget")&&!failure.staging,"RGBA16F4032x3896 overview explicitly declines64MiB artifact cap");
  ID3D11ShaderResourceView* unbound=nullptr;device.context->PSSetShaderResources(7,1,&unbound);
  run.reset();
}
