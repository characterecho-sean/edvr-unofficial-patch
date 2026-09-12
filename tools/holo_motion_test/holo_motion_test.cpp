#include "../../src/d3d11/holo_motion.h"
#include <d3dcompiler.h>
#include <d3d11sdklayers.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <vector>
#include <fstream>
#include <iterator>
using Microsoft::WRL::ComPtr;
unsigned checks=0;
void check(bool ok,const char* text) { ++checks; if(!ok) { std::printf("FAIL: %s\n",text); std::exit(1); } }
void hr(HRESULT h) { check(SUCCEEDED(h),"D3D operation"); }
ComPtr<ID3DBlob> compile(const char* source,const char* profile) {
    ComPtr<ID3DBlob> code,error;
    HRESULT h=D3DCompile(source,std::strlen(source),nullptr,nullptr,nullptr,"main",profile,D3DCOMPILE_ENABLE_STRICTNESS,0,&code,&error);
    if(FAILED(h) && error) std::puts(static_cast<const char*>(error->GetBufferPointer())); hr(h); return code;
}
namespace edvr {
ID3D11ComputeShader* shaderSwapCompileCs(ID3D11DeviceContext* ctx,const char* source,size_t,const char*,const char*,const SwapMacro*,const char*) {
    auto code=compile(source,"cs_5_0"); ComPtr<ID3D11Device> dev; ctx->GetDevice(&dev);
    ID3D11ComputeShader* shader=nullptr; hr(dev->CreateComputeShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&shader)); return shader;
}
}
using namespace edvr;
std::vector<float> read(ID3D11Device* dev,ID3D11DeviceContext* ctx,ID3D11ShaderResourceView* srv) {
    ComPtr<ID3D11Resource> resource; srv->GetResource(&resource); ComPtr<ID3D11Buffer> buffer; hr(resource.As(&buffer));
    D3D11_BUFFER_DESC bd{}; buffer->GetDesc(&bd); bd.BindFlags=bd.MiscFlags=bd.StructureByteStride=0; bd.Usage=D3D11_USAGE_STAGING; bd.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Buffer> stage; hr(dev->CreateBuffer(&bd,nullptr,&stage)); ctx->CopyResource(stage.Get(),buffer.Get());
    D3D11_MAPPED_SUBRESOURCE map{}; hr(ctx->Map(stage.Get(),0,D3D11_MAP_READ,0,&map));
    std::vector<float> result(bd.ByteWidth/4); std::memcpy(result.data(),map.pData,bd.ByteWidth); ctx->Unmap(stage.Get(),0); return result;
}
int main(int argc,char** argv) {
    ComPtr<ID3D11Device> dev; ComPtr<ID3D11DeviceContext> ctx; D3D_FEATURE_LEVEL level;
    HRESULT made=D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,D3D11_CREATE_DEVICE_DEBUG,nullptr,0,D3D11_SDK_VERSION,&dev,&level,&ctx);
    if(made==DXGI_ERROR_SDK_COMPONENT_MISSING) made=D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&dev,&level,&ctx);
    hr(made); ComPtr<ID3D11InfoQueue> messages; dev.As(&messages);
    auto buffer=[&](UINT bytes,UINT bind,UINT stride=0) {
        D3D11_BUFFER_DESC bd{}; bd.ByteWidth=bytes; bd.BindFlags=bind; bd.StructureByteStride=stride; bd.MiscFlags=stride?D3D11_RESOURCE_MISC_BUFFER_STRUCTURED:0;
        ComPtr<ID3D11Buffer> b; hr(dev->CreateBuffer(&bd,nullptr,&b)); return b;
    };
    float model[8][4]{},sceneData[276][4]{},material[4][4]{};
    model[4][0]=model[5][1]=model[7][2]=1; model[6][3]=.025f;
    UINT records[2][84]{}; float scale=1,z=1;
    std::memcpy(&records[0][1],&scale,4); std::memcpy(&records[0][6],&z,4);
    records[0][2]=0x80008000; records[0][3]=0xffff8000;
    ComPtr<ID3D11Buffer> cb[3]={buffer(sizeof(model),D3D11_BIND_CONSTANT_BUFFER),buffer(sizeof(sceneData),D3D11_BIND_CONSTANT_BUFFER),buffer(sizeof(material),D3D11_BIND_CONSTANT_BUFFER)};
    auto pool=buffer(sizeof(records),D3D11_BIND_SHADER_RESOURCE,336);
    ComPtr<ID3D11ShaderResourceView> poolSrv; hr(dev->CreateShaderResourceView(pool.Get(),nullptr,&poolSrv));
    auto instances=buffer(32,D3D11_BIND_VERTEX_BUFFER),vertices=buffer(160,D3D11_BIND_VERTEX_BUFFER),indices=buffer(12,D3D11_BIND_INDEX_BUFFER);
    UINT inst[8]{};
    D3D11_TEXTURE2D_DESC td{}; td.Width=td.Height=8; td.MipLevels=td.ArraySize=td.SampleDesc.Count=1;
    td.Format=DXGI_FORMAT_R32_FLOAT; td.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> scene; ComPtr<ID3D11ShaderResourceView> surface; hr(dev->CreateTexture2D(&td,nullptr,&scene)); hr(dev->CreateShaderResourceView(scene.Get(),nullptr,&surface));
    HoloMotion motion; HoloDraw args{'X',6,1,0,0,0};
    auto bind=[&] {
        ctx->UpdateSubresource(cb[0].Get(),0,nullptr,model,0,0); ctx->UpdateSubresource(cb[1].Get(),0,nullptr,sceneData,0,0); ctx->UpdateSubresource(cb[2].Get(),0,nullptr,material,0,0);
        ctx->UpdateSubresource(pool.Get(),0,nullptr,records,0,0); ctx->UpdateSubresource(instances.Get(),0,nullptr,inst,0,0);
        ID3D11Buffer* vb[2]={instances.Get(),vertices.Get()}; UINT strides[2]={8,40},offsets[2]{}; ctx->IASetVertexBuffers(0,2,vb,strides,offsets); ctx->IASetIndexBuffer(indices.Get(),DXGI_FORMAT_R16_UINT,0);
        ID3D11Buffer* constants[3]={cb[0].Get(),cb[1].Get(),cb[2].Get()}; ctx->VSSetConstantBuffers(0,3,constants);
        ctx->VSSetShaderResources(33,1,poolSrv.GetAddressOf()); ctx->PSSetShaderResources(2,1,surface.GetAddressOf());
        D3D11_VIEWPORT vp{0,0,8,8,0,1}; ctx->RSSetViewports(1,&vp);
    };
    auto run=[&](unsigned mode=0,unsigned sourceSlot=2) {
        bind(); ctx->CSSetConstantBuffers(1,1,cb[1].GetAddressOf()); ctx->CSSetShaderResources(2,1,poolSrv.GetAddressOf());
        if(mode==3)ctx->PSSetShaderResources(0,1,surface.GetAddressOf());
        if(mode==0 && sourceSlot==1) {
            ID3D11ShaderResourceView* none=nullptr;ctx->PSSetShaderResources(2,1,&none);
            ctx->PSSetShaderResources(1,1,surface.GetAddressOf());
        }
        check(motion.prepare(ctx.Get(),scene.Get(),args,10,mode,sourceSlot),"production history prepared");
        ComPtr<ID3D11Buffer> after; ctx->CSGetConstantBuffers(1,1,&after); check(after.Get()==cb[1].Get(),"caller CS constants restored");
        ComPtr<ID3D11ShaderResourceView> afterSrv; ctx->CSGetShaderResources(2,1,&afterSrv); check(afterSrv.Get()==poolSrv.Get(),"caller CS resource restored");
        ID3D11ShaderResourceView* views[2]{}; motion.views(scene.Get(),views); check(views[0] && views[1],"current-eye inputs exposed"); return read(dev.Get(),ctx.Get(),views[1]);
    };
    auto values=run(); check(values[56]==1 && values[59]==0,"first frame valid geometry, no invented predecessor");
    motion.frameBoundary(); model[4][3]=.02f; values=run();
    check(values[59]==1 && std::fabs(values[47]+.02f)<1e-6,"draw-time clip translation captured");
    motion.frameBoundary(); values=run(0,1);
    check(values[59]==1 && std::fabs(values[47])<1e-6,"unlit t1 material matches the same surface history without a t2 binding");
    // Reorder the pool and its instance stream without changing the object.
    std::memcpy(records[1],records[0],sizeof(records[0])); std::memset(records[0],0,sizeof(records[0])); inst[0]=1;
    motion.frameBoundary(); model[4][3]=.03f; values=run(); check(values[59]==1 && std::fabs(values[47]+.01f)<1e-6,"pool-slot reordering preserves history");
    motion.frameBoundary(); run(); run(); motion.frameBoundary(); values=run(); check(values[59]==0,"duplicate candidate declines");
    motion.frameBoundary(); material[2][0]=1; values=run(); check(values[59]==1,"animated material retains geometric history");
    motion.frameBoundary(); motion.frameBoundary(); values=run(); check(values[59]==0,"missing frame declines");
    motion.frameBoundary(); records[1][0]=1; values=run(); check(values[56]==0 && values[59]==0,"skinned geometry declines"); records[1][0]=0;
    float farDepth=16000; std::memcpy(&records[1][6],&farDepth,4); motion.frameBoundary(); values=run(); check(values[56]==0,"distant target marker declines");
    std::memcpy(&records[1][6],&z,4); motion.frameBoundary(); run();
    // Actual captured constants and reordered pool records, against double-
    // precision projected motion at the panel centre. Optional local fixture.
    for(int fixture=1;fixture<argc && fixture<=2;++fixture) {
        const unsigned mode=fixture==2?3:0;
        std::ifstream input(argv[fixture],std::ios::binary); UINT pairs=0; input.read(reinterpret_cast<char*>(&pairs),4); check(pairs>0 && pairs<4096,"fixture pair count");
        for(UINT pair=0;pair<pairs;++pair) {
            motion.frameBoundary(); motion.frameBoundary(); inst[0]=0;
            for(int side=0;side<2;++side) {
                input.read(reinterpret_cast<char*>(model),sizeof(model)); input.read(reinterpret_cast<char*>(sceneData),sizeof(sceneData));
                input.read(reinterpret_cast<char*>(material),sizeof(material)); input.read(reinterpret_cast<char*>(records[0]),336);
                check(bool(input),"captured state complete"); values=run(mode); if(side==0) motion.frameBoundary();
            }
            float point[3],expected[3]; input.read(reinterpret_cast<char*>(point),12); input.read(reinterpret_cast<char*>(expected),12);
            if(values[59]!=1)std::printf("fixture %d pair %u eligible %.0f matched %.0f origin %.9g %.9g %.9g\n",fixture,pair,values[56],values[59],values[35],values[39],values[43]);
            check(bool(input) && values[59]==1,"captured pair matched");
            float before[3]{}; for(int r=0;r<3;++r) { before[r]=values[44+r*4+3]; for(int j=0;j<3;++j) before[r]+=values[44+r*4+j]*point[j]; }
            for(int j=0;j<2;++j) check(std::fabs((before[j]/before[2]-expected[j]/expected[2])*1134)<.015,"captured projection within 0.015 pixels");
        }
        std::printf("replayed %u captured %s pairs\n",pairs,mode==3?"sprite":"hologram");
    }
    // Compile the production temporal consumer, including both grid modes.
    std::ifstream file("src/d3d11/temporal_shader_source.h"); std::string source((std::istreambuf_iterator<char>(file)),{});
    auto begin=source.find("bool holoPixel("),end=source.find("\n}\n",begin); check(begin!=std::string::npos && end!=std::string::npos,"temporal consumer found");
    std::string hlsl="Texture2D<float2> HC:register(t12);struct HoloRecord{uint4 key[8];float4 clip[3];float4 map[3];float4 meta;};StructuredBuffer<HoloRecord> HR:register(t13);Texture2D<float> Z:register(t0);RWTexture2D<float4> Out:register(u0);cbuffer P:register(b0){float4 holoJitter;float4 probe;}static const int4 region=0;static const int2 size=int2(8,8);static const float4 knobs=float4(0,1,.025,0);bool uiCovered(int2 q){return q.x!=0;}float zSceneAt(int2 q){return Z.Load(int3(q,0));}\n";
    hlsl+=source.substr(begin,end+3-begin);
    hlsl+="[numthreads(8,8,1)]void main(uint3 id:SV_DispatchThreadID){float2 pp;float zp;bool ok=holoPixel(id.xy,probe.xy,pp,zp);Out[id.xy]=float4(pp-id.xy,zp,ok?1:0);}";
    auto consumerCode=compile(hlsl.c_str(),"cs_5_0"); ComPtr<ID3D11ComputeShader> consumer;
    hr(dev->CreateComputeShader(consumerCode->GetBufferPointer(),consumerCode->GetBufferSize(),nullptr,&consumer));
    motion=HoloMotion{}; std::memset(model,0,sizeof(model)); std::memset(sceneData,0,sizeof(sceneData)); std::memset(records,0,sizeof(records));
    model[4][0]=model[5][1]=model[7][2]=1; model[6][3]=.025f;
    std::memcpy(&records[0][1],&scale,4); std::memcpy(&records[0][6],&z,4); records[0][2]=0x80008000; records[0][3]=0xffff8000; inst[0]=0;
    model[4][3]=-.0625f; run(); motion.frameBoundary(); model[4][3]=.0825f; run();
    ID3D11ShaderResourceView* views[2]{}; motion.views(scene.Get(),views);
    const float coverage[4]={1,.025f,0,0}; ctx->ClearRenderTargetView(motion.target(),coverage);
    float depths[64]; for(auto& d:depths) d=.025f; depths[10]=.05f;
    ctx->UpdateSubresource(scene.Get(),0,nullptr,depths,8*sizeof(float),0);
    td.Format=DXGI_FORMAT_R32G32B32A32_FLOAT; td.BindFlags=D3D11_BIND_UNORDERED_ACCESS;
    ComPtr<ID3D11Texture2D> output,staging; ComPtr<ID3D11UnorderedAccessView> uav;
    hr(dev->CreateTexture2D(&td,nullptr,&output)); hr(dev->CreateUnorderedAccessView(output.Get(),nullptr,&uav));
    td.BindFlags=0; td.Usage=D3D11_USAGE_STAGING; td.CPUAccessFlags=D3D11_CPU_ACCESS_READ; hr(dev->CreateTexture2D(&td,nullptr,&staging));
    auto params=buffer(32,D3D11_BIND_CONSTANT_BUFFER); float parameters[8]={.5f,0,1,0,0,0,0,16};
    auto consume=[&] {
        ctx->UpdateSubresource(params.Get(),0,nullptr,parameters,0,0);
        ctx->CSSetShader(consumer.Get(),nullptr,0); ctx->CSSetConstantBuffers(0,1,params.GetAddressOf());
        ctx->CSSetShaderResources(0,1,surface.GetAddressOf()); ctx->CSSetShaderResources(12,2,views); ctx->CSSetUnorderedAccessViews(0,1,uav.GetAddressOf(),nullptr); ctx->Dispatch(1,1,1);
        ctx->CopyResource(staging.Get(),output.Get()); D3D11_MAPPED_SUBRESOURCE map{}; hr(ctx->Map(staging.Get(),0,D3D11_MAP_READ,0,&map));
        std::vector<float> pixels(256); for(int y=0;y<8;++y) std::memcpy(pixels.data()+y*32,static_cast<char*>(map.pData)+y*map.RowPitch,128);
        ctx->Unmap(staging.Get(),0); ctx->ClearState(); return pixels;
    };
    auto pixels=consume(); check(pixels[3]==0,"unmarked pixel cannot take hologram motion"); check(pixels[10*4+3]==0,"later foreground depth rejects stale holo coverage");
    check(pixels[27*4+3]==1 && std::fabs(pixels[27*4]+.08f)<1e-5 && std::fabs(pixels[27*4+2]-1)<1e-5,"DLSS consumer removes raster jitter and preserves prior physical depth");
    parameters[4]=.25f; pixels=consume(); check(pixels[27*4+3]==1 && std::fabs(pixels[27*4]+.08f)<1e-5,"native output grid gives the same physical motion");
    parameters[2]=0; pixels=consume(); check(pixels[27*4+3]==0,"skipped temporal frame declines unmatched jitter history");
    // The sprite is a plane at local Y=0, viewed at Z=15 (beyond the
    // cockpit split) and later at planetary distance. Nearer scenery is
    // retained in coverage depth, without changing the sprite's motion.
    for(float distance:{15.f,60000000.f}) {
        ctx->ClearState();motion=HoloMotion{};inst[0]=0;
        std::memset(model,0,sizeof(model));std::memset(records,0,sizeof(records));std::memset(material,0,sizeof(material));
        // Map local X,Z into view X,Y; local Y is the plane normal.
        model[4][0]=1;model[5][2]=1;model[7][1]=1;model[7][3]=distance;
        std::memcpy(&records[0][1],&distance,4);records[0][2]=0x80008000;records[0][3]=0xffff8000;
        sceneData[275][0]=sceneData[275][1]=sceneData[275][2]=0;
        values=run(3);check(values[56]==1 && values[59]==0,"sprite permits forced projection and far plane without inventing history");
        motion.frameBoundary();model[4][3]=distance*.02f;values=run(3);
        check(values[59]==1,"sprite transform survives arbitrary physical distance");
        motion.views(scene.Get(),views);const float spriteCoverage[4]={1,.05f,0,0};ctx->ClearRenderTargetView(motion.target(),spriteCoverage);
        for(auto& d:depths)d=.05f;depths[10]=.1f;ctx->UpdateSubresource(scene.Get(),0,nullptr,depths,8*sizeof(float),0);
        parameters[0]=parameters[1]=parameters[4]=parameters[5]=0;parameters[2]=1;
        pixels=consume();check(pixels[10*4+3]==0,"later foreground still rejects sprite motion");
        check(pixels[27*4+3]==1 && std::fabs(pixels[27*4]+.08f)<1e-4,"sprite motion uses its plane rather than the foreground depth");
        check(std::fabs(pixels[27*4+2]/distance-1)<.001,"sprite preserves physical predecessor depth");
        motion.frameBoundary();material[0][0]=.25f;values=run(3);check(values[59]==1,"sprite brightness changes retain geometry history");
        motion.frameBoundary();material[1][0]=.25f;values=run(3);check(values[59]==0,"different sprite UV tile declines predecessor");
    }
    if(messages) for(UINT64 i=0;i<messages->GetNumStoredMessagesAllowedByRetrievalFilter();++i) {
        SIZE_T bytes=0; messages->GetMessage(i,nullptr,&bytes); std::vector<char> memory(bytes); auto* message=reinterpret_cast<D3D11_MESSAGE*>(memory.data()); hr(messages->GetMessage(i,message,&bytes));
        if(message->Severity<=D3D11_MESSAGE_SEVERITY_ERROR) { std::puts(message->pDescription); check(false,"D3D debug layer"); }
    }
    std::printf("hologram motion: %u checks passed\n",checks);
}
