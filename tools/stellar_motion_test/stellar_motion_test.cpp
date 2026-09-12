#include "../../src/d3d11/holo_motion.h"
#include "../../src/d3d11/stellar_coverage.h"
#include "../../src/d3d11/gpu_interval.h"
#include <d3dcompiler.h>
#include <d3d11sdklayers.h>
#include <vector>
#include <fstream>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
using Microsoft::WRL::ComPtr;
unsigned checks=0;
void check(bool ok,const char* why){++checks;if(!ok){std::printf("FAIL: %s\n",why);std::exit(1);}}
void hr(HRESULT h){check(SUCCEEDED(h),"D3D operation");}
ComPtr<ID3DBlob> compile(const char* source,const char* profile){
    ComPtr<ID3DBlob> code,error;HRESULT h=D3DCompile(source,std::strlen(source),nullptr,nullptr,nullptr,"main",profile,D3DCOMPILE_ENABLE_STRICTNESS,0,&code,&error);
    if(FAILED(h)&&error)std::puts(static_cast<const char*>(error->GetBufferPointer()));hr(h);return code;
}
namespace edvr {
ID3D11ComputeShader* shaderSwapCompileCs(ID3D11DeviceContext* ctx,const char* s,size_t,const char*,const char*,const SwapMacro*,const char*){
    auto code=compile(s,"cs_5_0");ComPtr<ID3D11Device> dev;ctx->GetDevice(&dev);ID3D11ComputeShader* shader=nullptr;
    hr(dev->CreateComputeShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&shader));return shader;
}
}
using namespace edvr;
#include "orbital_replay.h"
#include "opacity_replay.h"
int main(int argc,char** argv){
    ComPtr<ID3D11Device> dev;ComPtr<ID3D11DeviceContext> ctx;D3D_FEATURE_LEVEL level;
    HRESULT made=D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,D3D11_CREATE_DEVICE_DEBUG,nullptr,0,D3D11_SDK_VERSION,&dev,&level,&ctx);
    if(made==DXGI_ERROR_SDK_COMPONENT_MISSING)made=D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&dev,&level,&ctx);hr(made);
    ComPtr<ID3D11InfoQueue> messages;dev.As(&messages);
    {
        GpuIntervals<2> timer;
        for(int i=0;i<2;++i){check(timer.begin(ctx.Get()),"GPU coverage interval begins");timer.end(ctx.Get());}
        check(!timer.begin(ctx.Get())&&timer.totals.skipped==1,"full GPU interval ring skips without waiting or overwriting");
        // Only the test flushes/waits. Production polls once per frame.
        ctx->Flush();
        for(unsigned i=0;i<2000 && timer.totals.samples+timer.totals.invalid<2;++i){timer.poll(ctx.Get());Sleep(1);}
        check(timer.totals.samples==2&&timer.totals.invalid==0&&timer.totals.ms>=0,"GPU coverage timestamps complete and are counted exactly once");
        timer.poll(ctx.Get());check(timer.totals.samples==2,"completed GPU intervals are not counted twice");
        check(timer.begin(ctx.Get()),"completed GPU interval can be reused");timer.end(ctx.Get());
        timer.reset(ctx.Get());
        check(gpuTimingShutdown(ctx.Get()),"shared GPU coverage clock closes on its owner");
    }
    auto buffer=[&](UINT size,UINT bind){D3D11_BUFFER_DESC d{};d.ByteWidth=size;d.BindFlags=bind;ComPtr<ID3D11Buffer> b;hr(dev->CreateBuffer(&d,nullptr,&b));return b;};
    auto read=[&](ID3D11ShaderResourceView* view){
        ComPtr<ID3D11Resource> res;view->GetResource(&res);ComPtr<ID3D11Buffer> src;hr(res.As(&src));D3D11_BUFFER_DESC d{};src->GetDesc(&d);
        d.BindFlags=d.MiscFlags=d.StructureByteStride=0;d.Usage=D3D11_USAGE_STAGING;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Buffer> stage;hr(dev->CreateBuffer(&d,nullptr,&stage));ctx->CopyResource(stage.Get(),src.Get());D3D11_MAPPED_SUBRESOURCE m{};hr(ctx->Map(stage.Get(),0,D3D11_MAP_READ,0,&m));
        std::vector<float> v(d.ByteWidth/4);std::memcpy(v.data(),m.pData,d.ByteWidth);ctx->Unmap(stage.Get(),0);return v;
    };
    compile(kRingCoverage,"ps_5_0");compile(kOrbitalCoverageVs,"vs_5_0");compile(kOrbitalCoveragePs,"ps_5_0");
    float model[32]{},scene[1104]{},material[16]{};unsigned char stream[4096]{};
    auto cb0=buffer(sizeof(model),D3D11_BIND_CONSTANT_BUFFER),cb1=buffer(sizeof(scene),D3D11_BIND_CONSTANT_BUFFER),cb2=buffer(sizeof(material),D3D11_BIND_CONSTANT_BUFFER);
    auto vb0=buffer(1024,D3D11_BIND_VERTEX_BUFFER),vb1=buffer(sizeof(stream),D3D11_BIND_VERTEX_BUFFER),ib=buffer(128,D3D11_BIND_INDEX_BUFFER);
    D3D11_TEXTURE2D_DESC td{};td.Width=td.Height=8;td.MipLevels=td.ArraySize=td.SampleDesc.Count=1;td.Format=DXGI_FORMAT_R32_FLOAT;td.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> source;ComPtr<ID3D11ShaderResourceView> surface;hr(dev->CreateTexture2D(&td,nullptr,&source));hr(dev->CreateShaderResourceView(source.Get(),nullptr,&surface));
    HoloMotion motion;
    auto run=[&](unsigned mode,unsigned count,unsigned vertices){
        ctx->ClearState();ctx->UpdateSubresource(cb0.Get(),0,nullptr,model,0,0);ctx->UpdateSubresource(cb1.Get(),0,nullptr,scene,0,0);ctx->UpdateSubresource(cb2.Get(),0,nullptr,material,0,0);
        ctx->UpdateSubresource(vb1.Get(),0,nullptr,stream,0,0);ID3D11Buffer* cb[3]={mode==2?nullptr:cb0.Get(),cb1.Get(),cb2.Get()};ctx->VSSetConstantBuffers(0,3,cb);
        ID3D11Buffer* vb[2]={vb0.Get(),vb1.Get()};UINT strides[2]={mode==2?16u:40u,60},offsets[2]{};ctx->IASetVertexBuffers(0,2,vb,strides,offsets);ctx->IASetIndexBuffer(ib.Get(),DXGI_FORMAT_R16_UINT,0);
        ctx->PSSetShaderResources(3,1,surface.GetAddressOf());D3D11_VIEWPORT vp{0,0,8,8,0,1};ctx->RSSetViewports(1,&vp);
        check(motion.prepare(ctx.Get(),source.Get(),{mode==2?'N':'X',vertices,count,0,0,0},10,mode),"stellar transform preparation");
        ID3D11ShaderResourceView* views[2]{};motion.views(source.Get(),views);check(views[1]!=nullptr,"stellar records exposed");return read(views[1]);
    };
    model[16]=model[21]=model[30]=1;model[27]=.025f;model[31]=1e9f;
    auto v=run(1,1,6);check(v[56]==1&&v[59]==0,"far ring accepts valid current pose without inventing history");
    motion.frameBoundary();model[31]-=1e6f;v=run(1,1,6);check(v[59]==1&&std::fabs(v[55]-1e6f)<1,"ring approach carries physical translation");
    motion.frameBoundary();run(1,1,6);run(1,1,6);motion.frameBoundary();v=run(1,1,6);check(v[59]==1,"identical repeated ring passes share one physical predecessor");
    motion.frameBoundary();material[0]=1;v=run(1,1,6);check(v[59]==0,"different ring material cannot borrow old identity");
    motion=HoloMotion{};scene[270*4]=scene[271*4+1]=scene[272*4+3]=1;scene[273*4+2]=.025f;
    float* instances=reinterpret_cast<float*>(stream);
    for(int i=0;i<2;++i){float* p=instances+i*15;p[2]=1e9f;p[3]=2;p[7]=1;p[8]=p[9]=float(i+1)*1e8f;p[10]=1;}
    run(2,2,64);motion.frameBoundary();for(int i=0;i<2;++i)instances[i*15+2]-=1e6f;
    v=run(2,2,64);check(v[59]==1&&v[119]==1,"orbital instances have independent histories");
    check(std::fabs(v[55]-1e6f)<128&&std::fabs(v[115]-1e6f)<128,"large orbital scale has a conditioned inverse");
    motion.frameBoundary();float swapped[15];std::memcpy(swapped,stream,60);std::memcpy(stream,stream+60,60);std::memcpy(stream+60,swapped,60);
    v=run(2,2,64);check(v[59]==1&&v[119]==1,"orbital reorder follows shape rather than stream slot");
    unsigned pairCount=0,accepted=0;float maximum=0;
    if(argc>1){
        std::ifstream input(argv[1],std::ios::binary);input.read(reinterpret_cast<char*>(&pairCount),4);check(pairCount>0&&pairCount<4096,"stellar fixture header");
        for(unsigned pair=0;pair<pairCount;++pair){
            unsigned mode,count,vertices;input.read(reinterpret_cast<char*>(&mode),4);input.read(reinterpret_cast<char*>(&count),4);input.read(reinterpret_cast<char*>(&vertices),4);
            check((mode==1||mode==2)&&count>0&&count<=64,"stellar fixture draw");motion.frameBoundary();motion.frameBoundary();
            for(int side=0;side<2;++side){input.read(reinterpret_cast<char*>(model),sizeof(model));input.read(reinterpret_cast<char*>(scene),sizeof(scene));input.read(reinterpret_cast<char*>(material),sizeof(material));input.read(reinterpret_cast<char*>(stream),sizeof(stream));check(bool(input),"complete captured state");v=run(mode,count,vertices);if(!side)motion.frameBoundary();}
            for(unsigned n=0;n<count;++n){
                unsigned expectedValid;float point[3],expected[3];input.read(reinterpret_cast<char*>(&expectedValid),4);input.read(reinterpret_cast<char*>(point),12);input.read(reinterpret_cast<char*>(expected),12);check(bool(input),"complete captured expectation");
                const float* r=v.data()+60*n;check((r[59]==1)==(expectedValid==1),"captured eligibility and identity agree");
                if(!expectedValid)continue;++accepted;
                double before[3]{};for(int row=0;row<3;++row){before[row]=r[47+row*4];for(int k=0;k<3;++k)before[row]+=double(r[44+row*4+k])*point[k];}
                for(int k=0;k<2;++k){float error=float(std::fabs(before[k]/before[2]-double(expected[k])/expected[2])*1134);if(error>maximum)maximum=error;if(error>=.03f)std::printf("pair %u mode %u record %u error %.6f\n",pair,mode,n,error);check(error<.03f,"captured stellar projection within 0.03 input pixels");}
            }
        }
    }
    if(messages)for(UINT64 i=0;i<messages->GetNumStoredMessagesAllowedByRetrievalFilter();++i){SIZE_T size=0;messages->GetMessage(i,nullptr,&size);std::vector<char> data(size);auto* m=reinterpret_cast<D3D11_MESSAGE*>(data.data());hr(messages->GetMessage(i,m,&size));if(m->Severity<=D3D11_MESSAGE_SEVERITY_ERROR){std::puts(m->pDescription);check(false,"D3D debug layer");}}
    if(argc>2)verifyOrbitalVertices(dev.Get(),ctx.Get(),argv[2]);
    if(argc>3)verifyRingOpacity(dev.Get(),ctx.Get(),argv[3]);
    std::printf("stellar motion: %u checks; %u captured pairs, %u matched instances, max error %.6f input pixels\n",checks,pairCount,accepted,maximum);
}
