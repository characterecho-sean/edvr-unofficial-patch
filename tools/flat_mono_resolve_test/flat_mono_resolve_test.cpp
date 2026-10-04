// WARP exercises the shipped mono shaders/renderer. Backend stubs inspect their
// real GPU inputs and deliberately clobber state; SDK image quality is separate.
#include "../../src/d3d11/flat_mono_resolve.h"
#include "../../src/d3d11/flat_projection_math.h"
#include "../../src/d3d11/dlaa.h"
#include "../../src/d3d11/fsr3_engine.h"
#include "../../src/d3d11/engine_velocity_emit.h"
#include "../../src/d3d11/flat_hdr_crumbs.h"
#include "../../src/d3d11/flat_context_isolation.h"
#include "../../src/d3d11/flat_context_state.h"
#include "../hdr_crumb_trail.h"
#include <d3d11_1.h>
#include <d3d11sdklayers.h>
#include <wrl/client.h>
#include <algorithm>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <string>
#include <vector>
#include <cmath>
#include "../../src/common/config.h"
#include "../../src/common/log.h"
#include "../../src/common/system_d3d11.h"
using Microsoft::WRL::ComPtr;
namespace {
int failures=0,backendCalls=0;
bool backendFail=false,backendReset=false,infiniteSeen=false;
std::vector<std::string> resetEvents;
// What the HDR route's breadcrumbs (flat_hdr_crumbs.h) were handed to breadcrumb(), in order: the lines edvr_breadcrumbs.txt would hold.
std::vector<std::string> crumbLines;
// The resolver's one log line per initialisation that says which isolation it chose (flat_context_isolation.h), in order.
std::vector<std::string> isolationLines;
// Set by flat_context_isolation_gpu_tests.h: what the stub backend leaves bound, after its own ClearState. Null dirties nothing.
void (*backendDirtyHook)(ID3D11DeviceContext*)=nullptr;
float expectedJx=0,expectedJy=0;
float observedMotion=0,observedMotionY=0,observedDepth=0;unsigned observedReject=0;
// The whole motion texture the SDK was handed, decoded, and a hash over its raw bits: the shader's complete
// output for the frame, so "bit-identical" can be asserted rather than sampled at one pixel.
std::vector<float> observedMotionAll;std::vector<unsigned char> observedMaskAll;uint64_t observedMotionHash=0;uint32_t observedMotionW=0;
std::vector<uint64_t> motionHashLog; // one entry per backend call, in call order: the key-off golden comparison reads it
// The upscaler slot the resolver handed each backend call (FlatMonoResolveFrame::slot), in call order, and the first-person lines
// the resolver logged ("flat resolve: first-person ..."): the world route's two new seams.
std::vector<int> backendSlots;
std::vector<std::string> firstPersonLines;
std::vector<std::string> weaponFootprintLines;
uint32_t observedInW=0,observedInH=0,observedOutW=0,observedOutH=0;
// What the stub backends were handed on the HDR route (section 81): the flag and the formats of the textures it names.
bool observedHdr=false;DXGI_FORMAT observedColourFormat=DXGI_FORMAT_UNKNOWN,observedOutFormat=DXGI_FORMAT_UNKNOWN;
uint32_t observedBackendCenter=0,observedBackendOutside=0;
bool observedBackendPixels=false;
// Mutation runs (flat_first_person_gpu_tests.h) count a failed check here instead of failing the rig: a scenario run against a
// shader with one rule flipped is SUPPOSED to fail, and the rig fails only if it does not.
int* mutationFailures=nullptr;std::string mutationFirst;
void check(bool ok,const char* text){if(!ok){if(mutationFailures){if(!*mutationFailures)mutationFirst=text;++*mutationFailures;return;}std::printf("FAIL: %s\n",text);++failures;}}
bool readPixel(ID3D11DeviceContext* context,ID3D11Texture2D* texture,void* out,size_t bytes,UINT x=8,UINT y=8) {
    ComPtr<ID3D11Device> device;context->GetDevice(device.GetAddressOf());
    D3D11_TEXTURE2D_DESC d{};texture->GetDesc(&d);d.Usage=D3D11_USAGE_STAGING;d.BindFlags=0;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;d.MiscFlags=0;
    ComPtr<ID3D11Texture2D> staging;if(FAILED(device->CreateTexture2D(&d,nullptr,staging.GetAddressOf())))return false;
    context->CopyResource(staging.Get(),texture);D3D11_MAPPED_SUBRESOURCE map{};
    if(FAILED(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&map)))return false;
    std::memcpy(out,static_cast<unsigned char*>(map.pData)+size_t(y)*map.RowPitch+size_t(x)*bytes,bytes);
    context->Unmap(staging.Get(),0);return true;
}
float half(uint16_t value) {
    const unsigned exponent=(value>>10)&31,mantissa=value&1023;
    const float result=exponent?std::ldexp(1.0f+mantissa/1024.0f,int(exponent)-15):std::ldexp(float(mantissa),-24);
    return value&0x8000?-result:result;
}
// Every texel of a small texture, rows packed, or false.
bool readWhole(ID3D11DeviceContext* context,ID3D11Texture2D* texture,std::vector<unsigned char>& out,size_t bytesPerTexel) {
    ComPtr<ID3D11Device> device;context->GetDevice(device.GetAddressOf());
    D3D11_TEXTURE2D_DESC d{};texture->GetDesc(&d);d.Usage=D3D11_USAGE_STAGING;d.BindFlags=0;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;d.MiscFlags=0;
    ComPtr<ID3D11Texture2D> staging;if(FAILED(device->CreateTexture2D(&d,nullptr,staging.GetAddressOf())))return false;
    context->CopyResource(staging.Get(),texture);D3D11_MAPPED_SUBRESOURCE map{};
    if(FAILED(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&map)))return false;
    out.resize(size_t(d.Width)*d.Height*bytesPerTexel);
    for(UINT y=0;y<d.Height;++y)
        std::memcpy(out.data()+size_t(y)*d.Width*bytesPerTexel,static_cast<unsigned char*>(map.pData)+size_t(y)*map.RowPitch,size_t(d.Width)*bytesPerTexel);
    context->Unmap(staging.Get(),0);return true;
}
uint64_t fnv1a(const unsigned char* bytes,size_t count,uint64_t h=1469598103934665603ull) {
    for(size_t i=0;i<count;++i){h^=bytes[i];h*=1099511628211ull;}
    return h;
}
bool backend(ID3D11DeviceContext* c,ID3D11Texture2D* depth,ID3D11Texture2D* mv,ID3D11Texture2D* mask,
             ID3D11Texture2D* out,float jx,float jy,bool reset,const char** reason) {
    ++backendCalls;backendReset=reset;
    check(jx==expectedJx && jy==expectedJy,"backend receives actual rendered phase");
    uint16_t motion[2]{};unsigned char reject=0;
    check(readPixel(c,mv,motion,sizeof(motion)) && readPixel(c,depth,&observedDepth,sizeof(float)) &&
          readPixel(c,mask,&reject,1),"backend inputs readable");
    observedMotion=half(motion[0]);observedMotionY=half(motion[1]);observedReject=reject;
    {std::vector<unsigned char> all,maskAll,depthAll;D3D11_TEXTURE2D_DESC md{};mv->GetDesc(&md);
     if(readWhole(c,mv,all,4) && readWhole(c,mask,maskAll,1) && readWhole(c,depth,depthAll,4)) {
         // One hash over every byte the SDK was handed: motion, the reject mask and depth.
         observedMotionHash=fnv1a(depthAll.data(),depthAll.size(),fnv1a(maskAll.data(),maskAll.size(),fnv1a(all.data(),all.size())));
         observedMotionW=md.Width;observedMaskAll=maskAll;
         observedMotionAll.resize(all.size()/2);
         for(size_t i=0;i<observedMotionAll.size();++i){uint16_t v;std::memcpy(&v,all.data()+i*2,2);observedMotionAll[i]=half(v);}
     } else {observedMotionHash=0;observedMotionAll.clear();observedMaskAll.clear();}
     motionHashLog.push_back(observedMotionHash);}
    c->ClearState(); // Both successful and refused backends may clobber all stages.
    if(backendDirtyHook)backendDirtyHook(c);   // ...and the isolation tests make it clobber every stage and slot
    if(backendFail){if(reason)*reason="injected-backend-refusal";return false;}
    ComPtr<ID3D11Device> d;c->GetDevice(d.GetAddressOf());ComPtr<ID3D11UnorderedAccessView> uav;
    if(FAILED(d->CreateUnorderedAccessView(out,nullptr,uav.GetAddressOf())))return false;
    const float green[4]={0,1,0,1};c->ClearUnorderedAccessViewFloat(uav.Get(),green);
    return true;
}
ComPtr<ID3D11Texture2D> texture(ID3D11Device* d,UINT w,UINT h,DXGI_FORMAT fmt,UINT binds,const void* bytes=nullptr,UINT pitch=0) {
    D3D11_TEXTURE2D_DESC desc{};desc.Width=w;desc.Height=h;desc.ArraySize=desc.MipLevels=1;desc.SampleDesc.Count=1;
    desc.Format=fmt;desc.BindFlags=binds;desc.Usage=D3D11_USAGE_DEFAULT;
    D3D11_SUBRESOURCE_DATA data{};data.pSysMem=bytes;data.SysMemPitch=pitch;ComPtr<ID3D11Texture2D> out;
    check(SUCCEEDED(d->CreateTexture2D(&desc,bytes?&data:nullptr,out.GetAddressOf())),"fixture texture creation");return out;
}
ComPtr<ID3D11ShaderResourceView> view(ID3D11Device* d,ID3D11Resource* r) {
    ComPtr<ID3D11ShaderResourceView> out;check(SUCCEEDED(d->CreateShaderResourceView(r,nullptr,out.GetAddressOf())),"fixture view creation");return out;
}
void camera(float (&rows)[6][4]) {
    std::memset(rows,0,sizeof(rows));rows[0][0]=rows[1][1]=rows[2][3]=rows[4][2]=1;rows[3][2]=.025f;
}
uint32_t bits(float f){uint32_t v;std::memcpy(&v,&f,4);return v;}
// Real rows: Epic frame 71751, b1[270..275] (also flat_projection_ownership_tests.h), the shape the game itself
// composes for a kind-3 camera. Rows 0..2 hold three column vectors (x, y and the view direction in component 3).
constexpr float kEpicRows[6][4]={
    {.674860716f,-.714774430f,0,.684166729f},
    {-.804393589f,-.069881566f,0,.664024174f},
    {-.240084499f,-1.77504551f,0,-.301641792f},
    {0,0,.0250000004f,0},
    {.684166729f,.664024174f,-.301641792f,0},
    {-21.0930309f,-24.5114784f,-1.11009693f,0}};
// Turn the camera about world z in place: x and y of every column vector (rows 0 and 1) and of the view direction
// (row 4) rotate; z and the position do not.
void yawRows(float (&rows)[6][4],float theta) {
    const float c=std::cos(theta),s=std::sin(theta);
    for(unsigned col=0;col<4;++col){const float x=rows[0][col],y=rows[1][col];rows[0][col]=c*x-s*y;rows[1][col]=s*x+c*y;}
    const float x=rows[4][0],y=rows[4][1];rows[4][0]=c*x-s*y;rows[4][1]=s*x+c*y;
}
} // namespace
namespace edvr {
Config& Config::get() {
    static Config* config=[] {auto* c=new Config;wchar_t exe[32768]{};
        GetModuleFileNameW(nullptr,exe,32768);std::wstring path=exe;
        c->m_logDir=path.substr(0,path.find_last_of(L"\\/"))+L"\\flat-pixel-fixture";return c;}();
    return *config;
}
Log& Log::get() {static auto* log=new Log;return *log;}
void Log::note(const char* fmt,...) {
    // Formatted first: the isolation line is logged through "%s", so its prefix is in the text, not the format.
    constexpr char prefix[]="flat resolve reset event:",firstPersonPrefix[]="flat resolve: first-person",isolationPrefix[]="flat resolver: context isolation";
    char line[1024]{};
    va_list args;va_start(args,fmt);std::vsnprintf(line,sizeof(line),fmt,args);va_end(args);
    if(std::strncmp(line,prefix,sizeof(prefix)-1)==0)resetEvents.emplace_back(line);
    else if(std::strncmp(line,firstPersonPrefix,sizeof(firstPersonPrefix)-1)==0)firstPersonLines.emplace_back(line);
    else if(std::strncmp(line,isolationPrefix,sizeof(isolationPrefix)-1)==0)isolationLines.emplace_back(line);
    else if(std::strncmp(line,"flat weapon footprint:",22)==0)weaponFootprintLines.emplace_back(line);
}
// Stand-in for src\common\proxy.cpp's breadcrumb(): the route's crumbs land here so the rig can read the trail back.
void breadcrumb(const char* stage) {if(stage)crumbLines.emplace_back(stage);}
bool ensureDirectory(const std::wstring& path) {return CreateDirectoryW(path.c_str(),nullptr) || GetLastError()==ERROR_ALREADY_EXISTS;}
thread_local bool g_flatComputeInternal = false;
ID3D11ComputeShader* shaderSwapCreateCs(ID3D11DeviceContext* context,const void* bytecode,size_t size,
                                         const char*,const char*) {
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    context->GetDevice(&device);
    ID3D11ComputeShader* shader=nullptr;
    if(device)device->CreateComputeShader(bytecode,size,nullptr,&shader);
    return shader;
}
bool dlaaAvailable(ID3D11Device*,const char**){return true;}
bool fsr3Available(ID3D11Device*,const char**){return true;}
bool dlaaEvaluate(ID3D11DeviceContext* c,int slot,ID3D11Texture2D* colour,ID3D11Texture2D* depth,ID3D11Texture2D* mv,
    ID3D11Texture2D* out,ID3D11Texture2D* mask,uint32_t w,uint32_t h,uint32_t outW,uint32_t outH,float jx,float jy,bool reset,float,const char** why,bool hdr) {
    backendSlots.push_back(slot);
    observedInW=w;observedInH=h;observedOutW=outW;observedOutH=outH;
    observedHdr=hdr;observedColourFormat=DXGI_FORMAT_UNKNOWN;observedOutFormat=DXGI_FORMAT_UNKNOWN;
    {D3D11_TEXTURE2D_DESC d{};colour->GetDesc(&d);observedColourFormat=d.Format;out->GetDesc(&d);observedOutFormat=d.Format;}
    if(hdr) observedBackendPixels=readPixel(c,colour,&observedBackendCenter,4,8,8) &&
                                  readPixel(c,colour,&observedBackendOutside,4,1,1);
    return backend(c,depth,mv,mask,out,jx,jy,reset,why);
}
bool fsr3Evaluate(ID3D11DeviceContext* c,unsigned slot,ID3D11Texture2D* colour,ID3D11Texture2D* depth,ID3D11Texture2D* mv,
    ID3D11Texture2D* mask,ID3D11Texture2D* out,uint32_t w,uint32_t h,uint32_t outW,uint32_t outH,float jx,float jy,bool reset,float,
    float nearZ,float,float fov,const char** why,bool infinite,bool hdr) {
    backendSlots.push_back(static_cast<int>(slot));
    observedInW=w;observedInH=h;observedOutW=outW;observedOutH=outH;
    observedHdr=hdr;observedColourFormat=DXGI_FORMAT_UNKNOWN;observedOutFormat=DXGI_FORMAT_UNKNOWN;
    {D3D11_TEXTURE2D_DESC d{};colour->GetDesc(&d);observedColourFormat=d.Format;out->GetDesc(&d);observedOutFormat=d.Format;}
    if(hdr) observedBackendPixels=readPixel(c,colour,&observedBackendCenter,4,8,8) &&
                                  readPixel(c,colour,&observedBackendOutside,4,1,1);
    infiniteSeen=infinite;check(nearZ==.025f && std::abs(fov-1.5707963f)<1e-5f,"FSR actual near and FOV");
    return backend(c,depth,mv,mask,out,jx,jy,reset,why);
}
} // namespace edvr
#include "flat_projection_scope_tests.h"
#include "flat_projection_runtime_tests.h"
#include "flat_pixel_capture_gpu_tests.h"
#include "flat_draw_capture_gpu_tests.h"
#include "flat_weapon_footprint_gpu_tests.h"
#include "flat_overlay_layer_gpu_tests.h"
#include "flat_foreground_ownership_gpu_tests.h"
#include "flat_hdr_route_gpu_tests.h"
#include "flat_resolve_fixture.h"
#include "flat_upscaler_slot_gpu_tests.h"
#include "flat_first_person_gpu_tests.h"
#include "flat_first_person_phase_gpu_tests.h"
#include "flat_refusal_gpu_tests.h"
#include "flat_steady_depth_gpu_tests.h"
#include "flat_context_isolation_gpu_tests.h"
int main(int argc,char** argv) {
    const bool printGoldens=argc==2 && !std::strcmp(argv[1],"--print-goldens"); // --self-test plus the recorded key-off hashes, for re-recording
    if(argc!=2 || (std::strcmp(argv[1],"--self-test") && std::strcmp(argv[1],"--dry-run") && !printGoldens)){std::puts("usage: flat_mono_resolve_test --self-test|--dry-run|--print-goldens");return 2;}
    if(!std::strcmp(argv[1],"--dry-run")){std::puts("Would exercise mono resolve WARP shaders, backend inputs and state restoration; writes no files.");return 0;}
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;D3D_FEATURE_LEVEL level{};
    // Windows' own d3d11 through common/system_d3d11.h, never an import: EDVR's proxy sits beside this exe.
    const auto createDevice=edvr::systemD3D11CreateDevice();check(createDevice!=nullptr,"system D3D11 factory");if(!createDevice)return 1;
    HRESULT hr=createDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,D3D11_CREATE_DEVICE_DEBUG,nullptr,0,D3D11_SDK_VERSION,
        device.GetAddressOf(),&level,context.GetAddressOf());
    if(FAILED(hr))hr=createDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,
        device.GetAddressOf(),&level,context.GetAddressOf());
    check(SUCCEEDED(hr),"WARP device");if(FAILED(hr))return 1;
    check(edvr::reportSystemD3D11Only("flat_mono_resolve_test"),"the rig runs on System32's d3d11.dll and on no other d3d11.dll");
    ComPtr<ID3D11InfoQueue> messages;device.As(&messages);
    projectionScopeTests(device.Get(), context.Get());
    projectionRuntimeTests(device.Get(), context.Get());
    const UINT w=16,h=16;std::vector<uint32_t> red(w*h,0xff0000ff);std::vector<float> z(w*h,.01f),slots(w*h*2);
    for(size_t i=0;i<slots.size();i+=2){slots[i]=-1;slots[i+1]=.01f;}
    auto color=texture(device.Get(),w,h,DXGI_FORMAT_R8G8B8A8_UNORM,D3D11_BIND_SHADER_RESOURCE,red.data(),w*4);
    auto depth=texture(device.Get(),w,h,DXGI_FORMAT_R32_FLOAT,D3D11_BIND_SHADER_RESOURCE,z.data(),w*4);
    auto slotTexture=texture(device.Get(),w,h,DXGI_FORMAT_R32G32_FLOAT,D3D11_BIND_SHADER_RESOURCE,slots.data(),w*8);
    auto colorView=view(device.Get(),color.Get()),depthView=view(device.Get(),depth.Get()),slotView=view(device.Get(),slotTexture.Get());
    uint32_t record[84]{};D3D11_BUFFER_DESC bd{};bd.ByteWidth=sizeof(record);bd.StructureByteStride=336;
    bd.Usage=D3D11_USAGE_DEFAULT;bd.BindFlags=D3D11_BIND_SHADER_RESOURCE;bd.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    D3D11_SUBRESOURCE_DATA initial{};initial.pSysMem=record;ComPtr<ID3D11Buffer> pool;
    check(SUCCEEDED(device->CreateBuffer(&bd,&initial,pool.GetAddressOf())),"pool buffer");auto poolView=view(device.Get(),pool.Get());
    // The freshness stamp the prep shader's EN[276].x reads (the emit's
    // present-frame clock in production): the fixture's markers fold it in.
    constexpr uint32_t kFixtureStamp = 77;
    float scene[277][4]{};float cam[6][4];camera(cam);std::memcpy(scene+270,cam,sizeof(cam));
    {const uint32_t stamp=kFixtureStamp;std::memcpy(&scene[276][0],&stamp,4);}
    bd={};bd.ByteWidth=sizeof(scene);bd.Usage=D3D11_USAGE_DEFAULT;bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
    initial.pSysMem=scene;ComPtr<ID3D11Buffer> now,old;
    check(SUCCEEDED(device->CreateBuffer(&bd,&initial,now.GetAddressOf())) &&
          SUCCEEDED(device->CreateBuffer(&bd,&initial,old.GetAddressOf())),"engine camera buffers");
    auto target=texture(device.Get(),32,32,DXGI_FORMAT_R8G8B8A8_UNORM,D3D11_BIND_RENDER_TARGET);
    ComPtr<ID3D11RenderTargetView> targetView;check(SUCCEEDED(device->CreateRenderTargetView(target.Get(),nullptr,targetView.GetAddressOf())),"original RTV");
    D3D11_VIEWPORT viewport{3,4,19,21,.2f,.8f};
    auto bindOriginal=[&]{ID3D11RenderTargetView* rt=targetView.Get();context->OMSetRenderTargets(1,&rt,nullptr);
        ID3D11ShaderResourceView* srv=colorView.Get();context->PSSetShaderResources(0,1,&srv);context->VSSetShaderResources(3,1,&srv);
        ID3D11Buffer* cb=old.Get();context->CSSetConstantBuffers(4,1,&cb);context->RSSetViewports(1,&viewport);};
    auto restored=[&]{ComPtr<ID3D11RenderTargetView> rt;ComPtr<ID3D11ShaderResourceView> ps,vs;ComPtr<ID3D11Buffer> cb;
        context->OMGetRenderTargets(1,rt.GetAddressOf(),nullptr);context->PSGetShaderResources(0,1,ps.GetAddressOf());
        context->VSGetShaderResources(3,1,vs.GetAddressOf());context->CSGetConstantBuffers(4,1,cb.GetAddressOf());
        UINT count=1;D3D11_VIEWPORT current{};context->RSGetViewports(&count,&current);
        return rt.Get()==targetView.Get() && ps.Get()==colorView.Get() && vs.Get()==colorView.Get() && cb.Get()==old.Get() &&
            count==1 && !std::memcmp(&current,&viewport,sizeof(viewport));};
    edvr::FlatMonoResolveFrame f{};f.color=colorView.Get();f.depth=depthView.Get();f.renderWidth=w;f.renderHeight=h;
    f.outputWidth=f.outputHeight=32;f.deltaMs=16;camera(f.camera);camera(f.previousCamera);
    f.engine={slotView.Get(),poolView.Get(),now.Get(),old.Get()};f.mode=edvr::FlatMonoResolveMode::Dlss;f.frame=1;
    edvr::FlatMonoResolvePreflight planned{};planned.renderWidth=w;planned.renderHeight=h;
    planned.outputWidth=f.outputWidth;planned.outputHeight=f.outputHeight;planned.mode=f.mode;
    planned.colorViewFormat=DXGI_FORMAT_R8G8B8A8_UNORM;planned.depthViewFormat=DXGI_FORMAT_R32_FLOAT;
    const auto beforeInvalidPreflight=edvr::flatMonoResolveStats();
    auto invalidPreflight=planned;invalidPreflight.outputWidth=0;
    auto preflight=edvr::flatMonoResolvePreflight(device.Get(),context.Get(),invalidPreflight);
    check(preflight.status==edvr::FlatMonoResolvePreflightStatus::InvalidMetadata &&
          !preflight.readyForRasterJitter() && backendCalls==0 &&
          edvr::flatMonoResolveStats().initializations==beforeInvalidPreflight.initializations &&
          edvr::flatMonoResolveStats().allocations==beforeInvalidPreflight.allocations,
          "bad extent preflight refuses before renderer allocation or backend work");
    preflight=edvr::flatMonoResolvePreflight(device.Get(),context.Get(),planned);
    check(preflight.readyForRasterJitter() && preflight.spatialFallbackReady &&
          preflight.backendAvailable && preflight.backendFeatureCreationDeferred &&
          preflight.reason && !std::strcmp(preflight.reason,"ready-backend-feature-creation-deferred") &&
          backendCalls==0,"valid metadata preallocates spatial output without evaluating a backend feature");
    const auto preflightAllocations=edvr::flatMonoResolveStats().allocations;
    auto run=[&](bool wanted){bindOriginal();ComPtr<ID3D11ShaderResourceView> out;const char* reason=nullptr;
        bool ok=edvr::flatMonoResolve(device.Get(),context.Get(),f,out.GetAddressOf(),&reason);
        if(ok!=wanted)std::printf("info: resolver reason %s\n",reason?reason:"none");
        check(ok==wanted,"resolver result");check(restored(),"complete original pipeline restored");
        check(ok?out!=nullptr:out==nullptr,"owned output only on success");return out;};
    auto pixel=[&](ID3D11ShaderResourceView* srv,UINT x=16,UINT y=16){uint32_t value=0;
        if(!srv)return value;ComPtr<ID3D11Resource> resource;srv->GetResource(resource.GetAddressOf());ComPtr<ID3D11Texture2D> tex;resource.As(&tex);
        check(tex && readPixel(context.Get(),tex.Get(),&value,4,x,y),"resolved pixel readback");return value;};
    if(messages)messages->ClearStoredMessages();
    auto first=run(true);check(backendReset && observedReject==255 && pixel(first.Get())==0xff0000ff,"reset seeds backend but displays current color");
    auto stats=edvr::flatMonoResolveStats();
    check(stats.calls==1 && stats.initializations==1 && stats.allocations==1 && stats.acceptedResets==1 &&
          stats.acceptedContinues==0 && stats.lostHistory==1 && stats.currentContinueRun==0,
          "first frame reports one state build, one texture allocation and one accepted reset");
    check(resetEvents.size()==1 && resetEvents.back().find("frame=1 mode=dlss")!=std::string::npos &&
          resetEvents.back().find("requested=1 lost=1")!=std::string::npos &&
          resetEvents.back().find("camera-cut=0")!=std::string::npos &&
          resetEvents.back().find("delta-ms=16")!=std::string::npos,
          "successful backend reset logs frame, mode, reasons and elapsed time");
    f.reset=false;f.frame=2;f.camera[5][0]=.3125f;
    auto second=run(true);check(!backendReset && std::abs(observedMotion-1)<.001 && observedDepth==.01f,
        "camera translation gives positive one-render-pixel current-to-previous motion and unchanged raw depth");
    stats=edvr::flatMonoResolveStats();
    check(stats.initializations==1 && stats.allocations==1 && stats.acceptedContinues==1 &&
          stats.currentContinueRun==1 && stats.longestContinueRun==1 && stats.contextPointerMismatches==0,
          "continuous frame reuses state and textures without a reset");
    check(resetEvents.size()==1,"continuous backend frame emits no reset event");
    check(pixel(second.Get())==0xff00ff00,"valid pixel uses trained output");
    check(pixel(second.Get(),31,16)==0xff0000ff,"offscreen reprojection displays current spatial color");
    f.jitterX=expectedJx=.25f;f.jitterY=expectedJy=-.375f;
    f.previousJitterX=-.25f;f.previousJitterY=.375f;++f.frame;
    run(true);
    check(!backendReset && std::abs(observedMotion-1)<.001f && std::abs(observedMotionY)<.001f,
          "unjittered camera reconstructs depth at the current raster phase; SDK vector excludes both phases");
    // A matched pixel's own rigid record moves oppositely: exact engine vector
    // must replace the positive camera vector, not estimate it from a heuristic.
    record[1]=record[77]=bits(1);record[2]=record[78]=0x7fff7fff;record[3]=record[79]=0xfffe7fff;
    record[4]=bits(0);record[5]=bits(0);record[6]=bits(2.5f);
    record[73]=bits(-.3125f);record[74]=bits(0);record[75]=bits(2.5f);
    edvr::engine_velocity_emit::Pose np{{record[4],record[5],record[6],record[2],record[3]}};
    edvr::engine_velocity_emit::Pose pp{{record[73],record[74],record[75],record[78],record[79]}};
    record[72]=0x7FC0ED01u^edvr::engine_velocity_emit::markerHash(np,pp,kFixtureStamp);
    context->UpdateSubresource(pool.Get(),0,nullptr,record,0,0);
    slots[(8*w+8)*2]=1;context->UpdateSubresource(slotTexture.Get(),0,nullptr,slots.data(),w*8,0);
    ++f.frame;run(true);
    if(std::abs(observedMotion+1)>=.001 || observedReject!=0)std::printf("info: joined motion=%g reject=%u\n",observedMotion,observedReject);
    check(std::abs(observedMotion+1)<.001 && std::abs(observedMotionY)<.001 && observedReject==0,
          "joined engine pose uses shared exact reprojection at unjittered UV despite raster phase");
    for(unsigned kind=0;kind<3;++kind){
        if(kind==0)slots[(8*w+8)*2]=2; // corrupt even code
        if(kind==1){slots[(8*w+8)*2]=1;slots[(8*w+8)*2+1]=.02f;} // stale depth
        if(kind==2){slots[(8*w+8)*2+1]=.01f;record[72]=0x7FC0ED02u^edvr::engine_velocity_emit::markerHash(np,pp,kFixtureStamp);context->UpdateSubresource(pool.Get(),0,nullptr,record,0,0);}
        context->UpdateSubresource(slotTexture.Get(),0,nullptr,slots.data(),w*8,0);++f.frame;
        auto rejected=run(true);check(observedReject==255 && pixel(rejected.Get())==0xff0000ff,"corrupt/stale/masked pixel displays current color");
    }
    backendFail=true;++f.frame;run(false);backendFail=false;
    bindOriginal();ComPtr<ID3D11ShaderResourceView> recovered;const char* fallbackReason=nullptr;
    check(edvr::flatMonoResolveSpatialFallback(device.Get(),context.Get(),f,recovered.GetAddressOf(),&fallbackReason) &&
          recovered && restored() && pixel(recovered.Get())==0xff0000ff &&
          edvr::flatMonoResolveStats().allocations==preflightAllocations,
          "preflighted spatial fallback restores state and displays current jittered frame after SDK refusal without allocation");
    ++f.frame;run(true);check(backendReset,"backend failure and fallback invalidate history");
    stats=edvr::flatMonoResolveStats();check(stats.backendFailures==1 && stats.currentContinueRun==0,
        "backend failure and following accepted reset are visible in cumulative statistics");
    f.mode=edvr::FlatMonoResolveMode::Taa;f.reset=true;++f.frame;auto taa=run(true);
    check(pixel(taa.Get())==0xff0000ff,"TAA reset spatial upsample");f.reset=false;++f.frame;taa=run(true);
    check(pixel(taa.Get())==0xff0000ff,"TAA static color remains stable");
    f.mode=edvr::FlatMonoResolveMode::Fsr;++f.frame;run(true);check(infiniteSeen,"FSR receives explicit infinite-depth mode");
    f.mode=edvr::FlatMonoResolveMode::Dlss;++f.frame;auto retained=run(true);
    auto beforeInvalidate=edvr::flatMonoResolveStats();
    edvr::flatMonoResolveInvalidateHistory();++f.frame;auto invalidated=run(true);
    check(backendReset && retained.Get()==invalidated.Get(),"history-only invalidation preserves allocated output");
    stats=edvr::flatMonoResolveStats();
    check(stats.invalidations==beforeInvalidate.invalidations+1 && stats.allocations==beforeInvalidate.allocations &&
          stats.initializations==beforeInvalidate.initializations && stats.currentContinueRun==0,
          "history-only invalidation resets accumulation without rebuilding resources");
    // Original copy may decode SRGB. Keep encoded bits during reconstruction and
    // return that same view format, rather than silently changing its transfer.
    std::vector<uint32_t> encoded(w*h,0xff4080a0);
    auto srgbTexture=texture(device.Get(),w,h,DXGI_FORMAT_R8G8B8A8_TYPELESS,D3D11_BIND_SHADER_RESOURCE,encoded.data(),w*4);
    D3D11_SHADER_RESOURCE_VIEW_DESC srgbDesc{};srgbDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    srgbDesc.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D;srgbDesc.Texture2D.MipLevels=1;
    ComPtr<ID3D11ShaderResourceView> srgbView;
    check(SUCCEEDED(device->CreateShaderResourceView(srgbTexture.Get(),&srgbDesc,srgbView.GetAddressOf())),"SRGB copy input fixture");
    f.color=srgbView.Get();f.reset=true;++f.frame;auto srgbResult=run(true);
    D3D11_SHADER_RESOURCE_VIEW_DESC resultDesc{};if(srgbResult)srgbResult->GetDesc(&resultDesc);
    check(resultDesc.Format==DXGI_FORMAT_R8G8B8A8_UNORM_SRGB && pixel(srgbResult.Get())==encoded[0],
          "DLSS reset preserves encoded bytes and original SRGB view transfer");
    stats=edvr::flatMonoResolveStats();check(stats.formatChanges==1,"input view format change is counted");
    f.mode=edvr::FlatMonoResolveMode::Taa;
    for(unsigned i=0;i<2;++i){++f.frame;srgbResult=run(true);if(srgbResult)srgbResult->GetDesc(&resultDesc);
        check(resultDesc.Format==DXGI_FORMAT_R8G8B8A8_UNORM_SRGB && pixel(srgbResult.Get())==encoded[0],
              "both TAA history surfaces preserve SRGB copy contract");f.reset=false;}
    const int callsBefore=backendCalls;f.mode=edvr::FlatMonoResolveMode::Dlaa;run(false);
    check(backendCalls==callsBefore,"DLAA scale mismatch declines before backend");
    f.mode=edvr::FlatMonoResolveMode::Dlss;f.color=colorView.Get();++f.frame;run(true);
    f.frame+=2;run(true);stats=edvr::flatMonoResolveStats();
    check(backendReset && stats.frameGaps>=1 && stats.currentContinueRun==0,"frame gap produces a counted reset");
    ++f.frame;f.camera[5][0]=51;run(true);stats=edvr::flatMonoResolveStats();
    check(backendReset && stats.cameraCuts==1,"camera cut produces a counted reset");
    check(!resetEvents.empty() && resetEvents.back().find("camera-cut=1")!=std::string::npos &&
          resetEvents.back().find("requested=0")!=std::string::npos &&
          resetEvents.back().find("now=(51,0,0) previous=(0,0,0) origin-delta=(51,0,0)")!=std::string::npos &&
          resetEvents.back().find("max-matrix-delta=0")!=std::string::npos,
          "accepted camera cut logs raw origins and separates it from a view-matrix change");
    f.camera[0][0]=0;run(false);check(backendCalls==callsBefore+3,"singular camera declines before backend");
    f.camera[0][0]=1;
    std::vector<uint32_t> step(w*h,0xff000000);
    for(UINT y=0;y<h;++y)for(UINT x=9;x<w;++x)step[y*w+x]=0xff0000ff;
    context->UpdateSubresource(color.Get(),0,nullptr,step.data(),w*4,0);
    f.jitterX=0;f.jitterY=0;
    bindOriginal();ComPtr<ID3D11ShaderResourceView> spatialZero;
    check(edvr::flatMonoResolveSpatialFallback(device.Get(),context.Get(),f,spatialZero.GetAddressOf(),&fallbackReason) &&
          restored() && (pixel(spatialZero.Get())&255)==0,"zero-phase fallback samples the output grid directly");
    f.jitterX=.5f;
    bindOriginal();ComPtr<ID3D11ShaderResourceView> spatialJitter;
    check(edvr::flatMonoResolveSpatialFallback(device.Get(),context.Get(),f,spatialJitter.GetAddressOf(),&fallbackReason) &&
          restored() && (pixel(spatialJitter.Get())&255)>0 && (pixel(spatialJitter.Get())&255)<255,
          "nonzero-phase fallback shifts the spatial sample to undo raster displacement");
    // At output pixel 17, the previous unjittered coordinate is render x=8.75.
    // Its old raster phase +.5 selects depth texel 9. A lookup without that
    // phase would select texel 8 and reject valid history in this fixture.
    f.mode=edvr::FlatMonoResolveMode::Taa;f.reset=true;++f.frame;
    camera(f.camera);camera(f.previousCamera);f.jitterY=f.previousJitterY=0;
    for(UINT y=0;y<h;++y)z[y*w+8]=.02f;
    context->UpdateSubresource(depth.Get(),0,nullptr,z.data(),w*4,0);
    std::fill(step.begin(),step.end(),0xff0000ff);
    context->UpdateSubresource(color.Get(),0,nullptr,step.data(),w*4,0);
    f.jitterX=.5f;f.previousJitterX=0;
    auto phaseHistory=run(true);
    check((pixel(phaseHistory.Get(),17,16)&255)==255,"TAA reset places current color on the unjittered output grid");
    std::fill(step.begin(),step.end(),0xffff0000);
    for(UINT y=0;y<h;++y)step[y*w+8]=0xff0000ff; // keep red within the 3x3 history clamp
    context->UpdateSubresource(color.Get(),0,nullptr,step.data(),w*4,0);
    f.reset=false;++f.frame;f.jitterX=.25f;f.previousJitterX=.5f;
    phaseHistory=run(true);
    check((pixel(phaseHistory.Get(),17,16)&255)>200,
          "TAA compares expected depth at the previous frame's raster phase and retains aligned history");
    // Event output is bounded across the session, with a separate camera-cut
    // allowance even after requested resets exhaust the ordinary budget.
    for(unsigned i=0;i<40;++i){f.reset=true;++f.frame;run(true);}
    unsigned ordinaryLogs=0,cameraLogs=0;
    for(const auto& event:resetEvents)
        if(event.find("camera-cut=1")!=std::string::npos)++cameraLogs;else ++ordinaryLogs;
    check(ordinaryLogs==32 && cameraLogs==1,"ordinary reset events stop at their 32-line session cap");
    f.reset=false;++f.frame;f.camera[5][0]=51;run(true);
    unsigned cameraLogsAfter=0;
    for(const auto& event:resetEvents)if(event.find("camera-cut=1")!=std::string::npos)++cameraLogsAfter;
    check(ordinaryLogs==32 && cameraLogsAfter==2 && resetEvents.size()==34,
          "camera cut still logs after ordinary reset event cap is exhausted");
    f.jitterX=std::nanf("");
    bindOriginal();ComPtr<ID3D11ShaderResourceView> badJitter;
    check(!edvr::flatMonoResolveSpatialFallback(device.Get(),context.Get(),f,badJitter.GetAddressOf(),&fallbackReason) &&
          !badJitter && restored(),"nonfinite jitter cannot silently reach spatial fallback");
    // Gate 2 step 2 (design doc section 72): a supersampled render (R > D) on
    // the NVIDIA route evaluates DLAA at E = R on both axes, and the resolved
    // output view is R-sized so the game's own copy downsamples it to D.
    {
        const UINT w2=w*2,h2=h*2;
        std::vector<uint32_t> red2(w2*h2,0xff0000ff);std::vector<float> z2(w2*h2,.01f);
        std::vector<float> slots2(w2*h2*2);for(size_t i=0;i<slots2.size();i+=2){slots2[i]=-1;slots2[i+1]=.01f;}
        auto color2=texture(device.Get(),w2,h2,DXGI_FORMAT_R8G8B8A8_UNORM,D3D11_BIND_SHADER_RESOURCE,red2.data(),w2*4);
        auto depth2=texture(device.Get(),w2,h2,DXGI_FORMAT_R32_FLOAT,D3D11_BIND_SHADER_RESOURCE,z2.data(),w2*4);
        auto slotTexture2=texture(device.Get(),w2,h2,DXGI_FORMAT_R32G32_FLOAT,D3D11_BIND_SHADER_RESOURCE,slots2.data(),w2*8);
        auto colorView2=view(device.Get(),color2.Get()),depthView2=view(device.Get(),depth2.Get()),slotView2=view(device.Get(),slotTexture2.Get());
        edvr::FlatMonoResolveFrame f2{};f2.color=colorView2.Get();f2.depth=depthView2.Get();
        f2.renderWidth=w2;f2.renderHeight=h2;f2.outputWidth=w;f2.outputHeight=h;f2.deltaMs=16;
        camera(f2.camera);camera(f2.previousCamera);
        f2.engine={slotView2.Get(),poolView.Get(),now.Get(),old.Get()};
        f2.mode=edvr::FlatMonoResolveMode::Dlss;f2.frame=1;
        expectedJx=expectedJy=0;  // this block runs unjittered
        edvr::FlatMonoResolvePreflight planned2{};planned2.renderWidth=w2;planned2.renderHeight=h2;
        planned2.outputWidth=w;planned2.outputHeight=h;planned2.mode=f2.mode;
        planned2.colorViewFormat=DXGI_FORMAT_R8G8B8A8_UNORM;planned2.depthViewFormat=DXGI_FORMAT_R32_FLOAT;
        auto preflight2=edvr::flatMonoResolvePreflight(device.Get(),context.Get(),planned2);
        check(preflight2.readyForRasterJitter() && preflight2.backendAvailable,
              "supersampled preflight is ready with backend creation deferred");
        bindOriginal();ComPtr<ID3D11ShaderResourceView> out2;const char* reason2=nullptr;
        check(edvr::flatMonoResolve(device.Get(),context.Get(),f2,out2.GetAddressOf(),&reason2) && out2,
              "supersampled DLSS frame resolves via DLAA at render size");
        if(reason2)std::printf("info: supersample resolver reason %s\n",reason2);
        check(restored(),"supersampled resolve restores the complete original pipeline");
        check(observedInW==w2 && observedInH==h2 && observedOutW==w2 && observedOutH==h2,
              "backend evaluates the supersample route at render size on both axes");
        ComPtr<ID3D11Resource> outResource2;if(out2)out2->GetResource(outResource2.GetAddressOf());
        ComPtr<ID3D11Texture2D> outTexture2;if(outResource2)outResource2.As(&outTexture2);
        D3D11_TEXTURE2D_DESC outDesc2{};if(outTexture2)outTexture2->GetDesc(&outDesc2);
        check(outDesc2.Width==w2 && outDesc2.Height==h2,
              "the supersampled output view is render-sized for the game's downsample");
        check(pixel(out2.Get(),16,16)==0xff0000ff,"supersampled reset displays current render-size color");
        // FSR mirrors NVIDIA here: Native AA is the 1.0x case of the same
        // upscaler, evaluating at render size for the game's downsample.
        f2.mode=edvr::FlatMonoResolveMode::Fsr;f2.frame=2;
        bindOriginal();ComPtr<ID3D11ShaderResourceView> outFsr;reason2=nullptr;
        check(edvr::flatMonoResolve(device.Get(),context.Get(),f2,outFsr.GetAddressOf(),&reason2) && outFsr,
              "supersampled FSR frame resolves via Native AA at render size");
        if(reason2)std::printf("info: supersample FSR resolver reason %s\n",reason2);
        check(restored(),"supersampled FSR resolve restores the complete original pipeline");
        check(observedInW==w2 && observedInH==h2 && observedOutW==w2 && observedOutH==h2,
              "FSR evaluates the supersample route at render size on both axes");
        // TAA at R > D evaluates on the display grid today (the route's
        // honest report): the resolved view stays D-sized.
        f2.mode=edvr::FlatMonoResolveMode::Taa;f2.frame=3;
        bindOriginal();ComPtr<ID3D11ShaderResourceView> outTaa;reason2=nullptr;
        check(edvr::flatMonoResolve(device.Get(),context.Get(),f2,outTaa.GetAddressOf(),&reason2) && outTaa,
              "supersampled TAA frame resolves on the display grid");
        if(reason2)std::printf("info: supersample TAA resolver reason %s\n",reason2);
        ComPtr<ID3D11Resource> outResTaa;if(outTaa)outTaa->GetResource(outResTaa.GetAddressOf());
        ComPtr<ID3D11Texture2D> outTexTaa;if(outResTaa)outResTaa.As(&outTexTaa);
        D3D11_TEXTURE2D_DESC outDescTaa{};if(outTexTaa)outTexTaa->GetDesc(&outDescTaa);
        check(outDescTaa.Width==w && outDescTaa.Height==h,
              "the display-grid TAA output stays display-sized at supersampling");
    }
    // Gate-2 review F1: the negotiated evaluation size is part of the resolve's
    // resource cache key, and the preflight carries it. A cut E must reallocate
    // at the cut size (never reuse the route-default cache), restoring the
    // default reallocates back, and a preflight carrying the same cut lets the
    // first treated frame hit that cache instead of reallocating.
    {
        expectedJx=expectedJy=0;  // this block runs unjittered
        edvr::FlatMonoResolveFrame fc{};fc.color=colorView.Get();fc.depth=depthView.Get();
        fc.renderWidth=w;fc.renderHeight=h;fc.outputWidth=32;fc.outputHeight=32;fc.deltaMs=16;
        camera(fc.camera);camera(fc.previousCamera);
        fc.engine={slotView.Get(),poolView.Get(),now.Get(),old.Get()};
        fc.mode=edvr::FlatMonoResolveMode::Dlss;fc.frame=1;
        const auto cutBase=edvr::flatMonoResolveStats();
        bindOriginal();ComPtr<ID3D11ShaderResourceView> outDefault;const char* cutReason=nullptr;
        check(edvr::flatMonoResolve(device.Get(),context.Get(),fc,outDefault.GetAddressOf(),&cutReason) && outDefault,
              "default-E frame resolves on the route's evaluation grid");
        if(cutReason)std::printf("info: cut-probe default reason %s\n",cutReason);
        check(restored(),"default-E resolve restores the complete original pipeline");
        check(observedInW==w && observedInH==h && observedOutW==32 && observedOutH==32,
              "backend evaluates the default route at display size");
        check(edvr::flatMonoResolveStats().allocations==cutBase.allocations+1,
              "default-E frame allocates the route-default resource set once");
        ++fc.frame;fc.evalWidth=fc.evalHeight=24;
        bindOriginal();ComPtr<ID3D11ShaderResourceView> outCut;cutReason=nullptr;
        check(edvr::flatMonoResolve(device.Get(),context.Get(),fc,outCut.GetAddressOf(),&cutReason) && outCut,
              "cut-E frame resolves on the negotiated evaluation grid");
        if(cutReason)std::printf("info: cut-probe cut reason %s\n",cutReason);
        check(restored(),"cut-E resolve restores the complete original pipeline");
        check(observedInW==w && observedInH==h && observedOutW==24 && observedOutH==24,
              "backend evaluates the negotiated cut at its own size");
        ComPtr<ID3D11Resource> outCutResource;if(outCut)outCut->GetResource(outCutResource.GetAddressOf());
        ComPtr<ID3D11Texture2D> outCutTexture;if(outCutResource)outCutResource.As(&outCutTexture);
        D3D11_TEXTURE2D_DESC outCutDesc{};if(outCutTexture)outCutTexture->GetDesc(&outCutDesc);
        check(outCutDesc.Width==24 && outCutDesc.Height==24,
              "the cut-E output view is cut-sized for the game's upsample");
        check(edvr::flatMonoResolveStats().allocations==cutBase.allocations+2,
              "a changed E reallocates rather than reusing the route-default cache");
        ++fc.frame;fc.evalWidth=fc.evalHeight=0;
        bindOriginal();ComPtr<ID3D11ShaderResourceView> outBack;cutReason=nullptr;
        check(edvr::flatMonoResolve(device.Get(),context.Get(),fc,outBack.GetAddressOf(),&cutReason) && outBack &&
              observedOutW==32 && observedOutH==32,
              "dropping the override returns to the route's evaluation grid");
        check(edvr::flatMonoResolveStats().allocations==cutBase.allocations+3,
              "returning to the default E reallocates back");
        edvr::FlatMonoResolvePreflight cutPlan{};cutPlan.renderWidth=w;cutPlan.renderHeight=h;
        cutPlan.outputWidth=32;cutPlan.outputHeight=32;cutPlan.mode=fc.mode;
        cutPlan.evalWidth=cutPlan.evalHeight=24;
        cutPlan.colorViewFormat=DXGI_FORMAT_R8G8B8A8_UNORM;cutPlan.depthViewFormat=DXGI_FORMAT_R32_FLOAT;
        auto cutPreflight=edvr::flatMonoResolvePreflight(device.Get(),context.Get(),cutPlan);
        check(cutPreflight.readyForRasterJitter() && cutPreflight.spatialFallbackReady,
              "preflight carrying the negotiated E allocates at the cut size");
        check(edvr::flatMonoResolveStats().allocations==cutBase.allocations+4,
              "the cut-E preflight reallocates from the default-sized cache");
        ++fc.frame;fc.evalWidth=fc.evalHeight=24;
        bindOriginal();ComPtr<ID3D11ShaderResourceView> outPreflighted;cutReason=nullptr;
        check(edvr::flatMonoResolve(device.Get(),context.Get(),fc,outPreflighted.GetAddressOf(),&cutReason) && outPreflighted &&
              observedOutW==24 && observedOutH==24,
              "the preflighted cut-E frame resolves on the negotiated grid");
        check(edvr::flatMonoResolveStats().allocations==cutBase.allocations+4,
              "the first treated frame hits the preflighted cache instead of reallocating");
    }
    // Key-off invariance (docs/design-flat-camera-integration.md, the C3 wiring). Every scenario above ran with
    // rowsJitter = 0 -- the key-off state, where nothing removes a raster phase from the camera rows -- and the WHOLE
    // motion texture, reject mask and depth the SDK was handed (not one pixel of it) is compared bit for bit with the
    // hash recorded from the shader BEFORE the rows-unjitter change existed (2026-09-29, WARP on Windows 11 build
    // 26200, the precompiled bytecode of that day's d3dcompiler). A hash is a property of WARP's arithmetic on this
    // shader: a different WARP or compiler build may legitimately move one, and the way to re-record is to check out
    // the commit before the change and run --print-goldens, never to paste this build's output over a failure.
    // REAL camera rows, recorded the same way. Epic frame 71751's b1[270..275] (the composed camera the game itself
    // produces for a kind-3 camera), moved and turned, then run at zero and at both raster phases and through a joined
    // engine record. The fixture camera above has rows whose unjitter arithmetic is trivially neutral; these do not.
    {
        edvr::flatMonoResolveReset();
        std::fill(z.begin(),z.end(),.01f);context->UpdateSubresource(depth.Get(),0,nullptr,z.data(),w*4,0);
        context->UpdateSubresource(color.Get(),0,nullptr,red.data(),w*4,0);
        for(size_t i=0;i<slots.size();i+=2){slots[i]=-1;slots[i+1]=.01f;}
        context->UpdateSubresource(slotTexture.Get(),0,nullptr,slots.data(),w*8,0);
        auto sceneBuffer=[&](const float (&rows)[6][4]){float s[277][4]{};std::memcpy(s+270,rows,sizeof(rows));
            const uint32_t stamp=kFixtureStamp;std::memcpy(&s[276][0],&stamp,4);
            D3D11_BUFFER_DESC d{};d.ByteWidth=sizeof(s);d.Usage=D3D11_USAGE_DEFAULT;d.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
            D3D11_SUBRESOURCE_DATA init{};init.pSysMem=s;ComPtr<ID3D11Buffer> b;
            check(SUCCEEDED(device->CreateBuffer(&d,&init,b.GetAddressOf())),"real-camera scene buffer");return b;};
        float epicNow[6][4],epicPrev[6][4];std::memcpy(epicPrev,kEpicRows,sizeof(epicPrev));std::memcpy(epicNow,kEpicRows,sizeof(epicNow));
        epicNow[5][0]+=.3125f;yawRows(epicNow,.02f); // moved and turned since the previous frame
        auto sceneNowReal=sceneBuffer(epicNow),scenePrevReal=sceneBuffer(epicPrev);
        edvr::FlatMonoResolveFrame g{};g.color=colorView.Get();g.depth=depthView.Get();g.renderWidth=w;g.renderHeight=h;
        g.outputWidth=g.outputHeight=32;g.deltaMs=16;g.mode=edvr::FlatMonoResolveMode::Dlss;
        g.engine={slotView.Get(),poolView.Get(),sceneNowReal.Get(),scenePrevReal.Get()};
        auto runReal=[&](const char* what){bindOriginal();expectedJx=g.jitterX;expectedJy=g.jitterY;
            ComPtr<ID3D11ShaderResourceView> out;const char* why=nullptr;
            const bool ok=edvr::flatMonoResolve(device.Get(),context.Get(),g,out.GetAddressOf(),&why);
            if(!ok)std::printf("info: real-camera %s reason %s\n",what,why?why:"none");
            check(ok && out && restored(),what);};
        auto anyMotion=[&]{for(float v:observedMotionAll)if(v!=0)return true;return false;};
        g.frame=100;g.reset=true;std::memcpy(g.camera,epicPrev,sizeof(epicPrev));std::memcpy(g.previousCamera,epicPrev,sizeof(epicPrev));
        runReal("real camera: reset frame resolves");
        const size_t realBase=motionHashLog.size()-1;
        g.reset=false;++g.frame;std::memcpy(g.camera,epicNow,sizeof(epicNow));
        runReal("real camera: moved and turned frame resolves");
        check(anyMotion() && motionHashLog.back()!=motionHashLog[realBase],"real camera at zero phase produces motion the reset frame does not");
        g.jitterX=.25f;g.jitterY=-.375f;g.previousJitterX=-.25f;g.previousJitterY=.375f;++g.frame;
        runReal("real camera: both raster phases resolve");
        check(anyMotion() && motionHashLog.back()!=motionHashLog[realBase+1],"real camera at both raster phases differs from zero phase");
        record[1]=record[77]=bits(1);record[2]=record[78]=0x7fff7fff;record[3]=record[79]=0xfffe7fff;
        record[4]=bits(0);record[5]=bits(0);record[6]=bits(2.5f);
        record[73]=bits(-.3125f);record[74]=bits(0);record[75]=bits(2.5f);
        record[72]=0x7FC0ED01u^edvr::engine_velocity_emit::markerHash(np,pp,kFixtureStamp);
        context->UpdateSubresource(pool.Get(),0,nullptr,record,0,0);
        slots[(8*w+8)*2]=1;slots[(8*w+8)*2+1]=.01f;context->UpdateSubresource(slotTexture.Get(),0,nullptr,slots.data(),w*8,0);
        ++g.frame;runReal("real camera: joined engine record resolves");
        check(observedReject==0 && motionHashLog.back()!=motionHashLog[realBase+2],
              "real camera: the joined pixel is treated through the engine rows, not rejected");
        g.jitterX=g.jitterY=g.previousJitterX=g.previousJitterY=0;
        for(size_t i=0;i<slots.size();i+=2){slots[i]=-1;slots[i+1]=.01f;}
        context->UpdateSubresource(slotTexture.Get(),0,nullptr,slots.data(),w*8,0);
        const size_t goldenCalls=motionHashLog.size();
        if(printGoldens){std::printf("goldens: %zu backend calls\n",goldenCalls);
            for(size_t i=0;i<goldenCalls;++i)std::printf("    0x%016llxull,\n",static_cast<unsigned long long>(motionHashLog[i]));}
        // Recorded from the unmodified shader and resolver (HEAD c4bbe484 + this rig's instrumentation only), twice, identical.
        static const uint64_t kGolden[]={
            0xec545fd1f6ed4083ull,0xaf68111fd1178583ull,0xaf68111fd1178583ull,0x27944b19cf418803ull,
            0x117bdfd748229fd8ull,0x117bdfd748229fd8ull,0x117bdfd748229fd8ull,0x117bdfd748229fd8ull,
            0xec545fd1f6ed4083ull,0xec545fd1f6ed4083ull,0xec545fd1f6ed4083ull,0xec545fd1f6ed4083ull,
            0xec545fd1f6ed4083ull,0xec545fd1f6ed4083ull,0xec545fd1f6ed4083ull,0xec545fd1f6ed4083ull,
            0x037fdbc33a15f783ull,0x037fdbc33a15f783ull,0xbaa48aa2dedcd883ull,0xbaa48aa2dedcd883ull,
            0xbaa48aa2dedcd883ull,0xbaa48aa2dedcd883ull,0xec545fd1f6ed4083ull,
            0x364745529d928d5dull,0xbc8fb70f9acf9b13ull,0x64db0bd0d891cf3eull};
        check(goldenCalls==sizeof(kGolden)/sizeof(kGolden[0]),"key-off: the scenarios make the same backend calls as when the goldens were recorded");
        for(size_t i=0;i<goldenCalls && i<sizeof(kGolden)/sizeof(kGolden[0]);++i)
            if(motionHashLog[i]!=kGolden[i]){std::printf("FAIL: key-off golden %zu: got 0x%016llx want 0x%016llx\n",i,
                static_cast<unsigned long long>(motionHashLog[i]),static_cast<unsigned long long>(kGolden[i]));++failures;}

        // ---- Rows that carry the raster phase (the C3 wiring). ----
        // What the game derives under the injector is the legacy scope's forward shift on rows 0..3 (c2_derive_test proves
        // the equality on the derive model); rowsJitter* says the rows carry it, and the shader removes it. The same
        // camera and raster phases three ways: unjittered rows (the truth), phased rows with the phase declared (must
        // equal the truth), and phased rows with the phase NOT declared -- the negative control, which must be wrong by
        // about the phase, or the equality proves nothing. Once through the camera term and once through a joined engine
        // record, whose own scene snapshots carry the phase too.
        struct PhasedRows{float r[6][4];};
        auto phased=[&](const float (&rows)[6][4],float px,float py){PhasedRows out{};std::memcpy(out.r,rows,sizeof(out.r));
            edvr::FlatProjectionJitter j{};float m[4][4];std::memcpy(m,out.r,sizeof(m));
            check(edvr::flatProjectionJitter(px,py,w,h,j) && edvr::flatJitterForwardColumns(m,j),"the test rows accept a phase");
            std::memcpy(out.r,m,sizeof(m));return out;};
        const float jx=.25f,jy=-.375f,pjx=-.25f,pjy=.375f;
        const auto nowPhased=phased(epicNow,jx,jy),prevPhased=phased(epicPrev,pjx,pjy);
        auto sceneNowPhased=sceneBuffer(nowPhased.r),scenePrevPhased=sceneBuffer(prevPhased.r);
        struct Seen{std::vector<float> motion;std::vector<unsigned char> mask;float px=0,py=0;unsigned reject=0;};
        auto resolveWith=[&](const float (&cam)[6][4],const float (&prv)[6][4],ID3D11Buffer* sn,ID3D11Buffer* sp,
                             float rx,float ry,float prx,float pry,const char* what){
            std::memcpy(g.camera,cam,sizeof(g.camera));std::memcpy(g.previousCamera,prv,sizeof(g.previousCamera));
            g.engine.sceneNow=sn;g.engine.scenePrev=sp;
            g.jitterX=jx;g.jitterY=jy;g.previousJitterX=pjx;g.previousJitterY=pjy;
            g.rowsJitterX=rx;g.rowsJitterY=ry;g.previousRowsJitterX=prx;g.previousRowsJitterY=pry;
            g.reset=false;++g.frame;runReal(what);
            return Seen{observedMotionAll,observedMaskAll,observedMotion,observedMotionY,observedReject};};
        // Largest motion difference over the texels BOTH runs accepted (a texel one run rejects has no motion to compare;
        // the reject masks are compared separately), so a control's number is the size of the phase error itself and not
        // a texel flipping to rejected at the image edge.
        auto maxDiff=[&](const Seen& a,const Seen& b){float m=0;
            if(a.motion.size()!=b.motion.size()||a.motion.empty()||a.mask.size()!=b.mask.size()||a.mask.size()*2!=a.motion.size())return 1e9f;
            for(size_t t=0;t<a.mask.size();++t){if(a.mask[t]!=0||b.mask[t]!=0)continue;
                for(size_t c=0;c<2;++c)m=std::max(m,std::abs(a.motion[2*t+c]-b.motion[2*t+c]));}
            return m;};
        const float kSame=2e-3f,kWrong=.25f; // half-float quantum near 1 px is ~1e-3; the phases here are ~.5 and ~.75 px
        // (0) The trivial fixture camera first, the two textbook cases: a STATIC camera has exactly zero motion, a
        // 0.3125-unit translation exactly one render pixel (the existing fixture above), and phased rows with the
        // phase declared must give the same numbers. Undeclared, the static case misses by the phase difference.
        {
            float still[6][4],moved[6][4];camera(still);camera(moved);moved[5][0]=.3125f;
            const auto nowStill=phased(still,jx,jy),prevStill=phased(still,pjx,pjy),nowMoved=phased(moved,jx,jy);
            auto sn=sceneBuffer(still);
            auto stillTruth=resolveWith(still,still,sn.Get(),sn.Get(),0,0,0,0,"rows-jitter: static camera, unjittered rows");
            auto stillFixed=resolveWith(nowStill.r,prevStill.r,sn.Get(),sn.Get(),jx,jy,pjx,pjy,"rows-jitter: static camera, phased rows, phase declared");
            auto stillWrong=resolveWith(nowStill.r,prevStill.r,sn.Get(),sn.Get(),0,0,0,0,"rows-jitter: static camera, phased rows, phase undeclared");
            float stillMax=0;for(float v:stillTruth.motion)stillMax=std::max(stillMax,std::abs(v));
            check(stillMax==0,"rows-jitter: a static camera has exactly zero motion, texture-wide");
            check(maxDiff(stillTruth,stillFixed)<=kSame && stillTruth.mask==stillFixed.mask,
                  "rows-jitter: a static camera with phased rows and the phase declared still has zero motion");
            check(maxDiff(stillTruth,stillWrong)>=kWrong,
                  "rows-jitter negative control: a static camera with the phase undeclared misses by about the phase difference");
            auto moveTruth=resolveWith(moved,still,sn.Get(),sn.Get(),0,0,0,0,"rows-jitter: translated camera, unjittered rows");
            auto moveFixed=resolveWith(nowMoved.r,prevStill.r,sn.Get(),sn.Get(),jx,jy,pjx,pjy,"rows-jitter: translated camera, phased rows, phase declared");
            auto moveWrong=resolveWith(nowMoved.r,prevStill.r,sn.Get(),sn.Get(),0,0,0,0,"rows-jitter: translated camera, phased rows, phase undeclared");
            check(std::abs(moveTruth.px-1)<.001f && std::abs(moveTruth.py)<.001f,
                  "rows-jitter: a 0.3125-unit translation is one render pixel of motion at the sampled pixel");
            check(maxDiff(moveTruth,moveFixed)<=kSame && moveTruth.mask==moveFixed.mask,
                  "rows-jitter: the same translation with phased rows and the phase declared gives one pixel again, texture-wide");
            check(maxDiff(moveTruth,moveWrong)>=kWrong,
                  "rows-jitter negative control: the translation with the phase undeclared misses by about the phase difference");
            std::printf("flat mono resolve: rows-jitter fixture camera static error %.5f px (control %.3f px), translated error %.5f px (control %.3f px)\n",
                        maxDiff(stillTruth,stillFixed),maxDiff(stillTruth,stillWrong),maxDiff(moveTruth,moveFixed),maxDiff(moveTruth,moveWrong));
        }
        // (1) the camera term alone: every texel takes cameraBefore.
        const auto truth=resolveWith(epicNow,epicPrev,sceneNowReal.Get(),scenePrevReal.Get(),0,0,0,0,"rows-jitter: unjittered rows resolve");
        const auto fixedRows=resolveWith(nowPhased.r,prevPhased.r,sceneNowPhased.Get(),scenePrevPhased.Get(),jx,jy,pjx,pjy,
            "rows-jitter: phased rows with the phase declared resolve");
        const auto wrongRows=resolveWith(nowPhased.r,prevPhased.r,sceneNowPhased.Get(),scenePrevPhased.Get(),0,0,0,0,
            "rows-jitter: phased rows with the phase undeclared resolve");
        bool truthMoves=false;for(float v:truth.motion)if(v!=0)truthMoves=true;
        check(truthMoves,"rows-jitter: the unjittered scenario has motion to compare");
        const float cameraError=maxDiff(truth,fixedRows),cameraControl=maxDiff(truth,wrongRows);
        check(cameraError<=kSame,"rows-jitter: declared phase makes phased rows give the unjittered motion, texture-wide");
        check(truth.mask==fixedRows.mask,"rows-jitter: declared phase gives the unjittered reject mask, texture-wide");
        check(cameraControl>=kWrong,"rows-jitter negative control: phased rows with the phase undeclared are wrong by about the phase");
        // (2) the joined engine record: pixel (8,8) takes the record's own rows, EN and EB, which carry the phase too.
        slots[(8*w+8)*2]=1;slots[(8*w+8)*2+1]=.01f;context->UpdateSubresource(slotTexture.Get(),0,nullptr,slots.data(),w*8,0);
        const auto joinedTruth=resolveWith(epicNow,epicPrev,sceneNowReal.Get(),scenePrevReal.Get(),0,0,0,0,"rows-jitter: joined unjittered resolves");
        const auto joinedFixed=resolveWith(nowPhased.r,prevPhased.r,sceneNowPhased.Get(),scenePrevPhased.Get(),jx,jy,pjx,pjy,
            "rows-jitter: joined phased rows with the phase declared resolve");
        const auto joinedWrong=resolveWith(nowPhased.r,prevPhased.r,sceneNowPhased.Get(),scenePrevPhased.Get(),0,0,0,0,
            "rows-jitter: joined phased rows with the phase undeclared resolve");
        check(joinedTruth.reject==0 && joinedFixed.reject==0 && joinedWrong.reject==0,
              "rows-jitter: the joined pixel is treated through the engine rows in all three, never rejected");
        const float engineError=std::max(std::abs(joinedTruth.px-joinedFixed.px),std::abs(joinedTruth.py-joinedFixed.py));
        const float engineControl=std::max(std::abs(joinedTruth.px-joinedWrong.px),std::abs(joinedTruth.py-joinedWrong.py));
        check(engineError<=kSame,"rows-jitter: declared phase makes the joined pixel's engine reprojection give the unjittered motion");
        check(maxDiff(joinedTruth,joinedFixed)<=kSame && joinedTruth.mask==joinedFixed.mask,
              "rows-jitter: declared phase gives the unjittered texture with a joined pixel in it");
        check(engineControl>=kWrong,"rows-jitter negative control: the joined pixel is wrong by about the phase when the phase is undeclared");
        size_t controlFlips=0;for(size_t t=0;t<truth.mask.size()&&t<wrongRows.mask.size();++t)controlFlips+=truth.mask[t]!=wrongRows.mask[t];
        std::printf("flat mono resolve: rows-jitter camera term error %.5f px vs undeclared control %.3f px (%zu texels flip to rejected); "
                    "joined pixel (8,8) error %.5f px vs undeclared control %.3f px\n",cameraError,cameraControl,controlFlips,engineError,engineControl);
        // (3) a phase that cannot be a shift refuses the frame before any backend work, like every other invalid input.
        for(unsigned bad=0;bad<3;++bad){
            const int callsBeforeBad=backendCalls;
            g.rowsJitterX=bad==0?.75f:bad==1?std::nanf(""):0;g.previousRowsJitterY=bad==2?-.625f:0;g.rowsJitterY=g.previousRowsJitterX=0;
            bindOriginal();ComPtr<ID3D11ShaderResourceView> badOut;const char* badWhy=nullptr;++g.frame;
            const bool badOk=edvr::flatMonoResolve(device.Get(),context.Get(),g,badOut.GetAddressOf(),&badWhy);
            check(!badOk && !badOut && badWhy && !std::strcmp(badWhy,"flat-resolve-invalid-rows-jitter") &&
                  backendCalls==callsBeforeBad && restored(),
                  "rows-jitter: a rows phase outside half a pixel or not finite refuses the frame before any backend work");
        }
        g.rowsJitterX=g.rowsJitterY=g.previousRowsJitterX=g.previousRowsJitterY=0;

        // ---- The menu's stale-slot policy (flags.w = FlatMonoResolveFrame::staticScene, 2026-09-29). ----
        // A slot written by a keyed draw and then overdrawn by one that never wrote it (the Krait's unkeyed hull):
        // the slot's depth is not the pixel's, so its record says nothing about it. Outside the 3D main menu that
        // pixel's history is refused (rejection mask up, motion zero). With staticScene the pixel takes the camera
        // term -- the very motion a pixel with no slot at all takes -- so with the policy on the whole texture must
        // equal the no-slot texture. The real camera above is moved and turned, so the camera term is not zero and
        // "equals the camera term" cannot be met by a refused pixel's zero. Only the stale-depth refusal is relaxed: a
        // corrupt code, a sky pixel and the out-of-range sentinel stay refused, and a fresh joined slot keeps its record.
        {
            const UINT block0=4,blockSize=4;                      // the stale block: texels (4..7, 4..7)
            auto texel=[&](UINT x,UINT y){return size_t(y)*w+x;};
            auto put=[&](UINT x,UINT y,float code,float slotDepth){slots[texel(x,y)*2]=code;slots[texel(x,y)*2+1]=slotDepth;};
            auto upload=[&]{context->UpdateSubresource(slotTexture.Get(),0,nullptr,slots.data(),w*8,0);};
            // Beside the block: the pixels the policy must NOT touch. Three stay refused with it on -- a corrupt (even)
            // code, the out-of-range sentinel on a stale depth, and a sky pixel (depth 0) with a stale slot -- and one
            // fresh joined slot keeps its record's exact motion.
            auto others=[&]{put(2,2,2,.01f);put(13,3,4294967296.0f,.02f);put(12,12,1,.02f);put(8,8,1,.01f);};
            z[texel(12,12)]=0.0f;context->UpdateSubresource(depth.Get(),0,nullptr,z.data(),w*4,0);
            auto staleBlock=[&]{for(UINT y=block0;y<block0+blockSize;++y)for(UINT x=block0;x<block0+blockSize;++x)put(x,y,1,.02f);};
            auto noSlots=[&]{for(size_t i=0;i<slots.size();i+=2){slots[i]=-1;slots[i+1]=.01f;}};
            // The refused frames just above skipped frame numbers, so the next resolve is a counted reset
            // (motion zero, everything refused). One warm-up frame absorbs it; the three below are continuous.
            noSlots();others();upload();
            resolveWith(epicNow,epicPrev,sceneNowReal.Get(),scenePrevReal.Get(),0,0,0,0,"static scene: warm-up frame");
            noSlots();staleBlock();others();upload();
            g.staticScene=false;
            const auto off=resolveWith(epicNow,epicPrev,sceneNowReal.Get(),scenePrevReal.Get(),0,0,0,0,"static scene: stale block, policy off");
            const bool offReset=backendReset;
            g.staticScene=true;
            const auto on=resolveWith(epicNow,epicPrev,sceneNowReal.Get(),scenePrevReal.Get(),0,0,0,0,"static scene: stale block, policy on");
            const bool onReset=backendReset;
            g.staticScene=false;
            noSlots();others();upload();                          // the same frame with no stale block: the camera term
            const auto bare=resolveWith(epicNow,epicPrev,sceneNowReal.Get(),scenePrevReal.Get(),0,0,0,0,"static scene: no stale block, the camera term");
            check(!offReset && !onReset && !backendReset,
                  "static scene: the three frames are continuous, so no reset frame (everything refused) can hide the policy");
            auto inBlock=[&](size_t t){const UINT x=UINT(t%w),y=UINT(t/w);return x>=block0 && x<block0+blockSize && y>=block0 && y<block0+blockSize;};
            unsigned rejectedOff=0,rejectedOn=0,differOff=0,differOffOutside=0;
            for(size_t t=0;t<off.mask.size()&&t<on.mask.size()&&t<bare.mask.size();++t){
                if(inBlock(t)){rejectedOff+=off.mask[t]!=0;rejectedOn+=on.mask[t]!=0;}
                if(off.mask[t]!=bare.mask[t]){++differOff;if(!inBlock(t))++differOffOutside;}}
            const unsigned blockTexels=blockSize*blockSize;
            check(rejectedOff==blockTexels,"static scene: with the policy off every texel of the stale block is refused");
            check(rejectedOn==0,"static scene: with the policy on no texel of the stale block is refused");
            check(differOff==blockTexels && differOffOutside==0,
                  "static scene negative control: the policy off differs from the no-slot frame at the stale block's texels and nowhere else");
            float blockMotion=0;for(UINT y=block0;y<block0+blockSize;++y)for(UINT x=block0;x<block0+blockSize;++x)
                for(unsigned c=0;c<2;++c)blockMotion=std::max(blockMotion,std::abs(bare.motion[texel(x,y)*2+c]));
            check(blockMotion>.05f,"static scene: the camera term at the stale block is not zero, so a refused pixel's zero cannot pass for it");
            check(on.mask==bare.mask && maxDiff(on,bare)<=kSame,
                  "static scene: with the policy on a stale slot is exactly a pixel with no slot, texture-wide, block motion the camera term");
            check(off.reject==0 && on.reject==0 && on.px==off.px && on.py==off.py,
                  "static scene: a fresh joined slot keeps its record and its exact motion with the policy on or off");
            const size_t corrupt=texel(2,2),sentinel=texel(13,3),sky=texel(12,12);
            check(off.mask[corrupt]!=0 && on.mask[corrupt]!=0 && off.mask[sentinel]!=0 && on.mask[sentinel]!=0 &&
                  off.mask[sky]!=0 && on.mask[sky]!=0,
                  "static scene: a corrupt code, the out-of-range sentinel and a sky pixel with a stale slot stay refused with the policy on");
            std::printf("flat mono resolve: static scene: stale block %u/%u texels refused with the policy off, %u/%u with it on; "
                        "on vs no-slot texture difference %.5f px, block camera motion %.3f px\n",
                        rejectedOff,blockTexels,rejectedOn,blockTexels,maxDiff(on,bare),blockMotion);
            // Leave the fixture as the pixel-capture tests below expect it.
            noSlots();upload();z[texel(12,12)]=.01f;context->UpdateSubresource(depth.Get(),0,nullptr,z.data(),w*4,0);
            g.staticScene=false;
        }
    }
    context->ClearState();
    failures+=flatPixelCaptureGpuTests(device.Get(),context.Get());
    failures+=flatDrawCaptureGpuTests(device.Get(),context.Get());
    failures+=flatWeaponFootprintGpuTests(device.Get(),context.Get());
    failures+=flatOverlayLayerGpuTests(device.Get(),context.Get());
    failures+=flatForegroundOwnershipGpuTests(device.Get(),context.Get());
    // The HDR route's resolver half (design section 81): before the D3D message check below, so its draws are held to it.
    hdrRouteGpuTests(device.Get(),context.Get());
    // The VR world route's seams (section 82): the third upscaler slot, the first-person map and stencil in the prep, and the phase term
    // the map's vector gets when the world and the first-person camera are jittered (stage 2).
    upscalerSlotGpuTests(device.Get(),context.Get());
    firstPersonGpuTests(device.Get(),context.Get());
    firstPersonPhaseGpuTests(device.Get(),context.Get());
    // The stage 2 experiment build's refusal census and view: the prep's class byte, the counting pass and its read-back, the steady-detail
    // rule's effect on the counts, and the HDR finish's paint.
    refusalGpuTests(device.Get(),context.Get());
    // The depth-validated steady detail (the same section, the key's second form): the prep's depth check, its tolerance and its previous depth,
    // through the DLSS and FSR stubs and EDVR's own TAA, and the same scenario against the prep with one rule flipped at a time.
    steadyDepthGpuTests(device.Get(),context.Get());
    // The resolver's context isolation (the swap, and the explicit capture DXMT gets): also before the message check, so its calls are held to it.
    contextIsolationGpuTests(device.Get(),context.Get());
    if(messages)for(UINT64 i=0;i<messages->GetNumStoredMessages();++i){SIZE_T n=0;messages->GetMessage(i,nullptr,&n);std::vector<unsigned char> bytes(n);
        auto* msg=reinterpret_cast<D3D11_MESSAGE*>(bytes.data());messages->GetMessage(i,msg,&n);
        if(msg->Severity<=D3D11_MESSAGE_SEVERITY_WARNING){std::printf("D3D: %s\n",msg->pDescription);check(false,"no D3D resource hazards/errors/warnings");}}
    stats=edvr::flatMonoResolveStats();edvr::flatMonoResolveReset();
    auto afterReset=edvr::flatMonoResolveStats();
    check(afterReset.fullResets==stats.fullResets+1 && afterReset.acceptedResets==stats.acceptedResets &&
          afterReset.acceptedContinues==stats.acceptedContinues && afterReset.currentContinueRun==0,
          "full renderer reset preserves session diagnostics");
    context->ClearState();
    if(!failures)std::puts("flat mono resolve: PASS");return failures?1:0;
}
