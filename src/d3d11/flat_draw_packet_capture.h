#pragma once
// Manually armed, original-state evidence. Never issues a draw/query or changes
// a game binding. Unlike the pixel chronology, packet admission does not depend
// on the eventual temporal frame qualification.
#include <d3d11_3.h>
#include <wrl/client.h>
#include <algorithm>
#include <cstdio>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>
#include "device_hook.h"
#include "flat_trace.h"

namespace edvr {
class FlatDrawPacketCapture {
    template<class T> using Ptr=Microsoft::WRL::ComPtr<T>;
public:
    using Hash=uint64_t(*)(void*);
    using Lookup=bool(*)(char,uint64_t,const uint8_t**,size_t*);
    struct Args {char kind='?';UINT count=0,start=0,instances=0,startInstance=0;INT base=0;};
private:
    struct Field {std::string name,type,status="complete",value;};
    struct Part {UINT sub=0,rows=1,depth=1;bool done=false;UINT rowPitch=0,depthPitch=0;uint64_t bytes=0;std::vector<std::string> files;UINT logicalRowBytes=0;std::string status="queued";};
    struct Resource {Ptr<ID3D11Resource> source,stage;std::string name,status="queued",type,desc;
        uint64_t estimated=0;std::vector<Part> parts;};
    struct Packet {uint64_t frame=0;UINT q=0;uint64_t vs=0,ps=0,startMs=0;Args args;
        bool executed=false,complete=true,priority=true;std::string reason="candidate",file;
        std::string traceFile,traceStatus="pending-frame-close";
        std::vector<Field> original,actual;std::vector<Resource> resources;
        std::vector<std::pair<std::string,Ptr<ID3D11Resource>>> outputs;
        std::map<std::pair<ID3D11Resource*,unsigned>,size_t> resourceIds;};
    std::deque<std::unique_ptr<Packet>> pending_;
    std::map<std::pair<uint64_t,uint64_t>,std::pair<unsigned,uint64_t>> pairs_;
    std::wstring directory_;uint64_t armFrame_=0,armMs_=0,allocated_=0,written_=0;
    uint64_t budget_=1ull<<30;uint64_t metadataWritten_=0;std::string writeStatus_;
    uint64_t binaryBudget() const {return budget_>(64ull<<20)?budget_-(64ull<<20):budget_;}unsigned serial_=0,selected_=0,finished_=0,incomplete_=0,skipped_=0;
    bool armed_=false;Hash hash_=nullptr;Lookup lookup_=nullptr;
    uint64_t representativeReserved_=0;unsigned representativeMask_=0,priorityPairs_=0,budgetOmissions_=0;
    std::vector<std::string> packetFiles_;
    std::string lastManifest_;
    static constexpr uint64_t chunkCap_=64ull<<20;
    static std::string quote(const std::string& s) {
        std::string out="\"";for(unsigned char c:s){if(c=='\\'||c=='\"'){out+='\\';out+=char(c);}else if(c>=32)out+=char(c);else {char b[7];sprintf_s(b,"\\u%04x",c);out+=b;}}return out+'\"';
    }
    static std::string hex(const void* data,size_t n) {
        static const char* digits="0123456789ABCDEF";const auto* b=static_cast<const unsigned char*>(data);std::string out;out.reserve(n*2);
        for(size_t i=0;i<n;++i){out+=digits[b[i]>>4];out+=digits[b[i]&15];}return out;
    }
    static std::string identity(const void* p) {std::ostringstream o;o<<std::hex<<reinterpret_cast<uintptr_t>(p);return o.str();}
    template<class T> static void pod(std::vector<Field>& f,const std::string& name,const char* type,const T& d) {
        f.push_back({name,type,"complete",hex(&d,sizeof(d))});
    }
    static void text(std::vector<Field>& f,const std::string& name,const std::string& v,const char* status="complete") {f.push_back({name,"text",status,v});}
    void manifest(const char* status) {
        if(directory_.empty())return;std::ostringstream o;
        o<<"{\"schema\":\"edvr-flat-draw-capture\",\"version\":1,\"arm_frame\":"<<armFrame_
         <<",\"status\":"<<quote(status)<<",\"complete\":"<<(!armed_&&pending_.empty()&&selected_&&incomplete_==0?"true":"false")
         <<",\"budget\":"<<budget_<<",\"bytes_written\":"<<(written_+metadataWritten_+lastManifest_.size())<<",\"selected\":"<<selected_<<",\"finished\":"<<finished_
         <<",\"incomplete\":"<<incomplete_<<",\"pending\":"<<pending_.size()<<",\"priority_pair_cap\":16,\"priority_pairs\":"<<priorityPairs_<<",\"representative_category_cap\":4,\"representative_category_mask\":"<<representativeMask_<<",\"representative_budget\":268435456,\"representative_reserved\":"<<representativeReserved_<<",\"budget_omissions\":"<<budgetOmissions_<<",\"skipped\":"<<skipped_<<",\"packets\":[";
        for(size_t i=0;i<packetFiles_.size();++i){if(i)o<<',';o<<quote(packetFiles_[i]);}o<<"]}";
        FILE* f=nullptr;const auto path=directory_+L"\\manifest.json";const auto body=o.str();if(body==lastManifest_)return;if(body.size()>(1ull<<20)){++incomplete_;return;}lastManifest_=body;
        if(!_wfopen_s(&f,path.c_str(),L"wb")&&f){if(std::fwrite(body.data(),1,body.size(),f)!=body.size())++incomplete_;if(std::fclose(f)!=0)++incomplete_;}else ++incomplete_;
    }
    bool writeMapped(const std::string& stem,const D3D11_MAPPED_SUBRESOURCE& mapped,Part& part) {
        part.rowPitch=part.logicalRowBytes;part.depthPitch=part.logicalRowBytes*part.rows;
        part.bytes=uint64_t(part.depthPitch)*part.depth;
        writeStatus_="write-failed";
        if(part.bytes>binaryBudget() || written_>binaryBudget()-part.bytes){writeStatus_="budget-cap";++budgetOmissions_;return false;}
        if(!mapped.pData ||
           (part.rows>1&&mapped.RowPitch<part.logicalRowBytes) ||
           (part.depth>1&&mapped.DepthPitch<uint64_t(mapped.RowPitch)*part.rows))return false;
        FILE* out=nullptr;uint64_t fileBytes=0,index=0;bool ok=true;
        for(UINT z=0;z<part.depth&&ok;++z)for(UINT y=0;y<part.rows&&ok;++y) {
            const auto* row=static_cast<const BYTE*>(mapped.pData)+uint64_t(z)*mapped.DepthPitch+uint64_t(y)*mapped.RowPitch;
            uint64_t left=part.logicalRowBytes;
            while(left&&ok){if(!out){const auto leaf=stem+"_"+std::to_string(index++)+".bin";const auto path=directory_+L"\\"+std::wstring(leaf.begin(),leaf.end());
                    if(_wfopen_s(&out,path.c_str(),L"wb")||!out){ok=false;break;}part.files.push_back(leaf);fileBytes=0;}
                const uint64_t n=std::min(left,chunkCap_-fileBytes);if(std::fwrite(row,1,size_t(n),out)!=n){ok=false;break;}row+=n;left-=n;fileBytes+=n;written_+=n;
                if(fileBytes==chunkCap_){if(std::fclose(out)!=0)ok=false;out=nullptr;}
            }
        }if(out&&std::fclose(out)!=0)ok=false;return ok;
    }
    bool reserve(uint64_t n,bool priority) {
        if(n>budget_ || allocated_>budget_-n || (!priority&&(n>(256ull<<20)||representativeReserved_>(256ull<<20)-n))){++budgetOmissions_;return false;}
        allocated_+=n;if(!priority)representativeReserved_+=n;return true;
    }
    bool writeChunks(const std::string& stem,const void* data,uint64_t size,std::vector<std::string>& files) {
        writeStatus_="write-failed";if(size>binaryBudget() || written_>binaryBudget()-size){writeStatus_="budget-cap";++budgetOmissions_;return false;}
        const auto* p=static_cast<const unsigned char*>(data);
        for(uint64_t at=0,index=0;at<size;at+=chunkCap_,++index) {
            const uint64_t n=std::min(chunkCap_,size-at);const auto leaf=stem+"_"+std::to_string(index)+".bin";
            FILE* out=nullptr;const auto path=directory_+L"\\"+std::wstring(leaf.begin(),leaf.end());
            if(_wfopen_s(&out,path.c_str(),L"wb") || !out)return false;
            const bool ok=std::fwrite(p+at,1,size_t(n),out)==n;const bool closed=std::fclose(out)==0;
            if(!ok||!closed)return false;files.push_back(leaf);written_+=n;
        }return true;
    }
    static UINT formatBits(DXGI_FORMAT format) {
        const UINT f=UINT(format);
        if(f>=1&&f<=4)return 128;if(f>=5&&f<=8)return 96;if(f>=9&&f<=22)return 64;
        if(f>=23&&f<=47)return 32;if(f>=48&&f<=59)return 16;if(f>=60&&f<=65)return 8;
        if(f==66)return 1;if(f>=67&&f<=69)return 32;
        if(f>=70&&f<=84)return (f<=72||f>=79&&f<=81)?64:128;
        if(f>=85&&f<=86)return 16;if(f>=87&&f<=93)return 32;if(f>=94&&f<=99)return 128;
        if(f==115)return 16;return 0; // planar/video formats require explicit planes
    }
    static UINT rowBytes(DXGI_FORMAT format,UINT width) {
        const UINT f=UINT(format),bits=formatBits(format);if(!bits)return 0;
        if(f==68||f==69)return ((width+1)/2)*4;
        if((f>=70&&f<=84)||(f>=94&&f<=99))return ((width+3)/4)*(bits/8);
        return UINT((uint64_t(width)*bits+7)/8);
    }
    static uint64_t textureEstimate(DXGI_FORMAT format,UINT w,UINT h,UINT d,UINT mips,UINT arrays) {
        uint64_t total=0;const UINT f=UINT(format);const bool bc=(f>=70&&f<=84)||(f>=94&&f<=99);
        for(UINT i=0;i<mips;++i){const UINT rows=bc?(h+3)/4:h;total+=uint64_t((uint64_t(rowBytes(format,w))+255)&~255ull)*rows*d;w=std::max(1u,w/2);h=std::max(1u,h/2);d=std::max(1u,d/2);}return total*arrays;
    }
    size_t resource(ID3D11DeviceContext* ctx,Packet& p,const std::string& name,ID3D11Resource* source,unsigned phase=0) {
        const auto key=std::make_pair(source,phase);const auto prior=p.resourceIds.find(key);
        if(prior!=p.resourceIds.end())return prior->second;
        const size_t id=p.resources.size();p.resourceIds.emplace(key,id);p.resources.emplace_back();auto& r=p.resources.back();r.name=name;r.source=source;
        if(!source){r.status="unbound";return id;}
        Ptr<ID3D11Device> dev;ctx->GetDevice(&dev);D3D11_RESOURCE_DIMENSION dim{};source->GetType(&dim);HRESULT hr=E_FAIL;
        if(dim==D3D11_RESOURCE_DIMENSION_BUFFER){Ptr<ID3D11Buffer> b;source->QueryInterface(IID_PPV_ARGS(&b));if(!b){r.status="resource-QI-failed";p.complete=false;return id;}D3D11_BUFFER_DESC d{};b->GetDesc(&d);r.type="D3D11_BUFFER_DESC";r.desc=hex(&d,sizeof(d));r.estimated=d.ByteWidth;
            if(!reserve(r.estimated,p.priority)){r.status="cap";p.complete=false;return id;}d.Usage=D3D11_USAGE_STAGING;d.BindFlags=d.MiscFlags=d.StructureByteStride=0;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;Ptr<ID3D11Buffer> stage;hr=dev->CreateBuffer(&d,nullptr,&stage);r.stage=stage;r.parts.push_back({0,1,1});r.parts.back().logicalRowBytes=d.ByteWidth;
        }else if(dim==D3D11_RESOURCE_DIMENSION_TEXTURE2D){Ptr<ID3D11Texture2D> t;source->QueryInterface(IID_PPV_ARGS(&t));if(!t){r.status="resource-QI-failed";p.complete=false;return id;}D3D11_TEXTURE2D_DESC d{};t->GetDesc(&d);r.type="D3D11_TEXTURE2D_DESC";r.desc=hex(&d,sizeof(d));
            if(d.SampleDesc.Count!=1){r.status="unsupported-multisample";p.complete=false;return id;}
            if(!formatBits(d.Format)){r.status="unsupported-format-planes";p.complete=false;return id;}
            r.estimated=textureEstimate(d.Format,d.Width,d.Height,1,d.MipLevels,d.ArraySize);if(!reserve(r.estimated,p.priority)){r.status="cap";p.complete=false;return id;}
            const bool bc=(d.Format>=70&&d.Format<=84)||(d.Format>=94&&d.Format<=99);
            for(UINT a=0;a<d.ArraySize;++a)for(UINT m=0;m<d.MipLevels;++m){UINT rows=std::max(1u,d.Height>>m);if(bc)rows=(rows+3)/4;r.parts.push_back({D3D11CalcSubresource(m,a,d.MipLevels),rows,1});r.parts.back().logicalRowBytes=rowBytes(d.Format,std::max(1u,d.Width>>m));}
            d.Usage=D3D11_USAGE_STAGING;d.BindFlags=d.MiscFlags=0;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;Ptr<ID3D11Texture2D> stage;hr=dev->CreateTexture2D(&d,nullptr,&stage);r.stage=stage;
        }else if(dim==D3D11_RESOURCE_DIMENSION_TEXTURE1D){Ptr<ID3D11Texture1D> t;source->QueryInterface(IID_PPV_ARGS(&t));if(!t){r.status="resource-QI-failed";p.complete=false;return id;}D3D11_TEXTURE1D_DESC d{};t->GetDesc(&d);r.type="D3D11_TEXTURE1D_DESC";r.desc=hex(&d,sizeof(d));if(!formatBits(d.Format)){r.status="unsupported-format-planes";p.complete=false;return id;}r.estimated=textureEstimate(d.Format,d.Width,1,1,d.MipLevels,d.ArraySize);
            if(!reserve(r.estimated,p.priority)){r.status="cap";p.complete=false;return id;}for(UINT a=0;a<d.ArraySize;++a)for(UINT m=0;m<d.MipLevels;++m){r.parts.push_back({D3D11CalcSubresource(m,a,d.MipLevels),1,1});r.parts.back().logicalRowBytes=rowBytes(d.Format,std::max(1u,d.Width>>m));}
            d.Usage=D3D11_USAGE_STAGING;d.BindFlags=d.MiscFlags=0;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;Ptr<ID3D11Texture1D> stage;hr=dev->CreateTexture1D(&d,nullptr,&stage);r.stage=stage;
        }else if(dim==D3D11_RESOURCE_DIMENSION_TEXTURE3D){Ptr<ID3D11Texture3D> t;source->QueryInterface(IID_PPV_ARGS(&t));if(!t){r.status="resource-QI-failed";p.complete=false;return id;}D3D11_TEXTURE3D_DESC d{};t->GetDesc(&d);r.type="D3D11_TEXTURE3D_DESC";r.desc=hex(&d,sizeof(d));if(!formatBits(d.Format)){r.status="unsupported-format-planes";p.complete=false;return id;}r.estimated=textureEstimate(d.Format,d.Width,d.Height,d.Depth,d.MipLevels,1);
            if(!reserve(r.estimated,p.priority)){r.status="cap";p.complete=false;return id;}for(UINT m=0;m<d.MipLevels;++m){r.parts.push_back({m,std::max(1u,d.Height>>m),std::max(1u,d.Depth>>m)});r.parts.back().logicalRowBytes=rowBytes(d.Format,std::max(1u,d.Width>>m));}
            d.Usage=D3D11_USAGE_STAGING;d.BindFlags=d.MiscFlags=0;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;Ptr<ID3D11Texture3D> stage;hr=dev->CreateTexture3D(&d,nullptr,&stage);r.stage=stage;
        }else {r.status="unsupported-resource-dimension";p.complete=false;return id;}
        if(FAILED(hr)||!r.stage){allocated_-=r.estimated;if(!p.priority)representativeReserved_-=r.estimated;r.status="staging-create-failed";p.complete=false;return id;}
        if(phase==1)r.status="awaiting-original-draw";else ctx->CopyResource(r.stage.Get(),source);return id;
    }
    void bindResource(ID3D11DeviceContext* ctx,Packet& p,std::vector<Field>& f,const std::string& name,ID3D11Resource* r,bool snapshot,unsigned phase=0) {
        if(!r){text(f,name,"null");return;}
        f.push_back({name,snapshot?"resource-id":"object-id","complete",snapshot?std::to_string(resource(ctx,p,name,r,phase)):identity(r)});
    }
    template<class V,class D> void view(ID3D11DeviceContext* ctx,Packet& p,std::vector<Field>& f,const std::string& name,V* v,const char* type,bool snapshot,bool output=false,unsigned phase=0) {
        if(!v){text(f,name,"null");return;}D d{};v->GetDesc(&d);pod(f,name+".view",type,d);Ptr<ID3D11Resource> r;v->GetResource(&r);bindResource(ctx,p,f,name+".resource",r.Get(),snapshot,phase);if(output&&snapshot)p.outputs.emplace_back(name,r);
    }
    void shader(Packet& p,std::vector<Field>& f,char stage,const char* name,IUnknown* s,ID3D11ClassInstance** classes,UINT n,bool snapshot,unsigned phase=0) {
        pod(f,std::string(name)+".classCount","UINT",n);
        if(!s){text(f,name,"null");return;}const uint64_t h=hash_?hash_(s):0;std::ostringstream out;out<<std::hex<<h;text(f,std::string(name)+".hash",out.str());text(f,std::string(name)+".object",identity(s));
        if(snapshot&&phase==0&&((stage=='v'&&h!=p.vs)||(stage=='p'&&h!=p.ps))){text(f,std::string(name)+".identity","mismatch","binding-shadow-vs-live-mismatch");p.complete=false;}
        for(UINT i=0;i<n;++i){D3D11_CLASS_INSTANCE_DESC d{};classes[i]->GetDesc(&d);const auto prefix=std::string(name)+".class"+std::to_string(i);pod(f,prefix,"D3D11_CLASS_INSTANCE_DESC",d);
            char type[256]{},instance[256]{};SIZE_T nt=sizeof(type),ni=sizeof(instance);classes[i]->GetTypeName(type,&nt);classes[i]->GetInstanceName(instance,&ni);type[255]=instance[255]=0;
            text(f,prefix+".type",type,nt<=sizeof(type)?"complete":"truncated");text(f,prefix+".instance",instance,ni<=sizeof(instance)?"complete":"truncated");if(nt>sizeof(type)||ni>sizeof(instance))p.complete=false;classes[i]->Release();}
        if(stage=='g'){std::vector<uint8_t> creation;
            if(flatPacketGeometryShaderData(static_cast<ID3D11GeometryShader*>(s),creation))
                f.push_back({"GS.stream_output_creation","FlatPacketGeometryCreate+FlatPacketSOElement[]+UINT[]","complete",hex(creation.data(),creation.size())});
            else {text(f,"GS.stream_output_creation","unknown","missing-creation-SO-declarations");p.complete=false;}}
        if(!snapshot)return;const uint8_t* data=nullptr;size_t bytes=0;std::vector<std::string> files;
        const bool found=lookup_ && lookup_(stage,h,&data,&bytes);
        const std::string stem=p.file+(phase==2?"_executed_shader_":"_shader_")+name;
        if(!found){text(f,std::string(name)+".bytecode","missing","missing-creation-bytes");p.complete=false;}
        else if(!writeChunks(stem,data,bytes,files)){text(f,std::string(name)+".bytecode","",writeStatus_.c_str());p.complete=false;}
        else {std::string names;for(auto& file:files){if(!names.empty())names+=';';names+=file;}text(f,std::string(name)+".bytecode",names);}
    }
    void pipeline(ID3D11DeviceContext* ctx,Packet& p,std::vector<Field>& f,bool snapshot,unsigned phase=0) {
        Ptr<ID3D11DeviceContext1> c1;ctx->QueryInterface(IID_PPV_ARGS(&c1));
#define EDVR_PACKET_STAGE(S,C,N) { Ptr<ID3D11##S##Shader> s;ID3D11ClassInstance* classes[253]{};UINT nc=253;ctx->C##GetShader(&s,classes,&nc);shader(p,f,N,#C,s.Get(),classes,nc,snapshot,phase); \
        ID3D11Buffer* cb[14]{};UINT first[14]{},count[14]{};if(c1)c1->C##GetConstantBuffers1(0,14,cb,first,count);else ctx->C##GetConstantBuffers(0,14,cb); \
        for(UINT i=0;i<14;++i){Ptr<ID3D11Buffer> b;b.Attach(cb[i]);std::string key=std::string(#C)+".CB"+std::to_string(i);if(b&&!c1){D3D11_BUFFER_DESC d{};b->GetDesc(&d);count[i]=(d.ByteWidth+15)/16;}UINT range[]={first[i],count[i]};pod(f,key+".range","UINT[2]",range);bindResource(ctx,p,f,key,b.Get(),snapshot,phase);} \
        ID3D11ShaderResourceView* srv[128]{};ctx->C##GetShaderResources(0,128,srv);for(UINT i=0;i<128;++i){Ptr<ID3D11ShaderResourceView> v;v.Attach(srv[i]);view<ID3D11ShaderResourceView,D3D11_SHADER_RESOURCE_VIEW_DESC>(ctx,p,f,std::string(#C)+".SRV"+std::to_string(i),v.Get(),"D3D11_SHADER_RESOURCE_VIEW_DESC",snapshot,false,phase);} \
        ID3D11SamplerState* samplers[16]{};ctx->C##GetSamplers(0,16,samplers);for(UINT i=0;i<16;++i){Ptr<ID3D11SamplerState> samplerState;samplerState.Attach(samplers[i]);std::string key=std::string(#C)+".sampler"+std::to_string(i);if(samplerState){D3D11_SAMPLER_DESC d{};samplerState->GetDesc(&d);pod(f,key,"D3D11_SAMPLER_DESC",d);}else text(f,key,"null");} }
        EDVR_PACKET_STAGE(Vertex,VS,'v');EDVR_PACKET_STAGE(Hull,HS,'h');EDVR_PACKET_STAGE(Domain,DS,'d');EDVR_PACKET_STAGE(Geometry,GS,'g');EDVR_PACKET_STAGE(Pixel,PS,'p');
#undef EDVR_PACKET_STAGE
        ID3D11Buffer* vb[32]{};UINT strides[32]{},offsets[32]{};ctx->IAGetVertexBuffers(0,32,vb,strides,offsets);
        for(UINT i=0;i<32;++i){Ptr<ID3D11Buffer> b;b.Attach(vb[i]);const auto key="IA.VB"+std::to_string(i);UINT values[]={strides[i],offsets[i]};pod(f,key+".strideOffset","UINT[2]",values);bindResource(ctx,p,f,key,b.Get(),snapshot,phase);}
        Ptr<ID3D11Buffer> ib;DXGI_FORMAT fmt{};UINT offset=0;ctx->IAGetIndexBuffer(&ib,&fmt,&offset);UINT index[]={UINT(fmt),offset};pod(f,"IA.indexFormatOffset","UINT[2]",index);bindResource(ctx,p,f,"IA.IB",ib.Get(),snapshot,phase);
        D3D11_PRIMITIVE_TOPOLOGY topo{};ctx->IAGetPrimitiveTopology(&topo);pod(f,"IA.topology","UINT",topo);
        Ptr<ID3D11InputLayout> layout;ctx->IAGetInputLayout(&layout);if(layout){std::vector<FlatPacketInputElement> items;if(flatPacketInputLayout(layout.Get(),items))for(size_t i=0;i<items.size();++i)pod(f,"IA.layout"+std::to_string(i),"FlatPacketInputElement",items[i]);else {text(f,"IA.layout",identity(layout.Get()),"missing-creation-layout");p.complete=false;}}else text(f,"IA.layout","null");
        Ptr<ID3D11BlendState> blend;FLOAT factors[4]{};UINT mask=0;ctx->OMGetBlendState(&blend,factors,&mask);pod(f,"OM.blendFactors","FLOAT[4]",factors);pod(f,"OM.sampleMask","UINT",mask);
        if(blend){D3D11_BLEND_DESC d{};blend->GetDesc(&d);pod(f,"OM.blend","D3D11_BLEND_DESC",d);Ptr<ID3D11BlendState1> b1;if(SUCCEEDED(blend.As(&b1))){D3D11_BLEND_DESC1 d1{};b1->GetDesc1(&d1);pod(f,"OM.blend1","D3D11_BLEND_DESC1",d1);}}else text(f,"OM.blend","default");
        Ptr<ID3D11DepthStencilState> depth;UINT ref=0;ctx->OMGetDepthStencilState(&depth,&ref);pod(f,"OM.stencilRef","UINT",ref);if(depth){D3D11_DEPTH_STENCIL_DESC d{};depth->GetDesc(&d);pod(f,"OM.depthStencil","D3D11_DEPTH_STENCIL_DESC",d);}else text(f,"OM.depthStencil","default");
        Ptr<ID3D11RasterizerState> raster;ctx->RSGetState(&raster);if(raster){D3D11_RASTERIZER_DESC d{};raster->GetDesc(&d);pod(f,"RS.state","D3D11_RASTERIZER_DESC",d);Ptr<ID3D11RasterizerState1> r1;if(SUCCEEDED(raster.As(&r1))){D3D11_RASTERIZER_DESC1 d1{};r1->GetDesc1(&d1);pod(f,"RS.state1","D3D11_RASTERIZER_DESC1",d1);}Ptr<ID3D11RasterizerState2> r2;if(SUCCEEDED(raster.As(&r2))){D3D11_RASTERIZER_DESC2 d2{};r2->GetDesc2(&d2);pod(f,"RS.state2","D3D11_RASTERIZER_DESC2",d2);}}else text(f,"RS.state","default");
        D3D11_VIEWPORT vp[16]{};UINT nv=16;ctx->RSGetViewports(&nv,vp);pod(f,"RS.viewportCount","UINT",nv);for(UINT i=0;i<std::min(nv,16u);++i)pod(f,"RS.viewport"+std::to_string(i),"D3D11_VIEWPORT",vp[i]);D3D11_RECT sc[16]{};UINT ns=16;ctx->RSGetScissorRects(&ns,sc);pod(f,"RS.scissorCount","UINT",ns);for(UINT i=0;i<std::min(ns,16u);++i)pod(f,"RS.scissor"+std::to_string(i),"D3D11_RECT",sc[i]);
        ID3D11RenderTargetView* rt[8]{};Ptr<ID3D11DepthStencilView> dsv;ctx->OMGetRenderTargets(8,rt,&dsv);
        // Outputs first in the snapshot budget: complete before/after is more
        // useful than a large unused SRV. Original capture calls output pass
        // separately before this metadata pass.
        for(UINT i=0;i<8;++i){Ptr<ID3D11RenderTargetView> v;v.Attach(rt[i]);view<ID3D11RenderTargetView,D3D11_RENDER_TARGET_VIEW_DESC>(ctx,p,f,"OM.RTV"+std::to_string(i),v.Get(),"D3D11_RENDER_TARGET_VIEW_DESC",snapshot,true,phase);}
        view<ID3D11DepthStencilView,D3D11_DEPTH_STENCIL_VIEW_DESC>(ctx,p,f,"OM.DSV",dsv.Get(),"D3D11_DEPTH_STENCIL_VIEW_DESC",snapshot,true,phase);
        Ptr<ID3D11Device> dev;ctx->GetDevice(&dev);const UINT nu=dev->GetFeatureLevel()>=D3D_FEATURE_LEVEL_11_1?64:8;ID3D11UnorderedAccessView* uav[64]{};ctx->OMGetRenderTargetsAndUnorderedAccessViews(0,nullptr,nullptr,0,nu,uav);
        for(UINT i=0;i<nu;++i){Ptr<ID3D11UnorderedAccessView> v;v.Attach(uav[i]);view<ID3D11UnorderedAccessView,D3D11_UNORDERED_ACCESS_VIEW_DESC>(ctx,p,f,"OM.UAV"+std::to_string(i),v.Get(),"D3D11_UNORDERED_ACCESS_VIEW_DESC",snapshot,true,phase);if(v){text(f,"OM.UAV"+std::to_string(i)+".counter","unknown","unsupported-hidden-counter");p.complete=false;}}
        ID3D11Buffer* so[4]{};ctx->SOGetTargets(4,so);for(UINT i=0;i<4;++i){Ptr<ID3D11Buffer> b;b.Attach(so[i]);bindResource(ctx,p,f,"SO.target"+std::to_string(i),b.Get(),snapshot,phase);if(b&&snapshot)p.outputs.emplace_back("SO.target"+std::to_string(i),b);if(b){text(f,"SO.offset"+std::to_string(i),"unknown","unsupported-offset-query");p.complete=false;}}
        Ptr<ID3D11Predicate> predicate;BOOL value=FALSE;ctx->GetPredication(&predicate,&value);pod(f,"predicate.value","BOOL",value);text(f,"predicate.object",identity(predicate.Get()));if(predicate){text(f,"predicate.result","unknown","unsupported-predicate-value");p.complete=false;}
    }
    void outputsBefore(ID3D11DeviceContext* ctx,Packet& p,unsigned phase=0) {
        ID3D11RenderTargetView* rt[8]{};Ptr<ID3D11DepthStencilView> dsv;ctx->OMGetRenderTargets(8,rt,&dsv);
        for(UINT i=0;i<8;++i){Ptr<ID3D11RenderTargetView> v;v.Attach(rt[i]);if(v){Ptr<ID3D11Resource> r;v->GetResource(&r);resource(ctx,p,"OM.RTV"+std::to_string(i),r.Get(),phase);resource(ctx,p,"OM.RTV"+std::to_string(i)+".after",r.Get(),true);}}
        if(dsv){Ptr<ID3D11Resource> r;dsv->GetResource(&r);resource(ctx,p,"OM.DSV",r.Get(),phase);resource(ctx,p,"OM.DSV.after",r.Get(),true);}
        Ptr<ID3D11Device> dev;ctx->GetDevice(&dev);const UINT count=dev->GetFeatureLevel()>=D3D_FEATURE_LEVEL_11_1?64:8;ID3D11UnorderedAccessView* uav[64]{};
        ctx->OMGetRenderTargetsAndUnorderedAccessViews(0,nullptr,nullptr,0,count,uav);
        for(UINT i=0;i<count;++i){Ptr<ID3D11UnorderedAccessView> v;v.Attach(uav[i]);if(v){Ptr<ID3D11Resource> r;v->GetResource(&r);const auto name="OM.UAV"+std::to_string(i);resource(ctx,p,name,r.Get(),phase);resource(ctx,p,name+".after",r.Get(),1);}}
        ID3D11Buffer* so[4]{};ctx->SOGetTargets(4,so);for(UINT i=0;i<4;++i){Ptr<ID3D11Buffer> b;b.Attach(so[i]);if(b){const auto name="SO.target"+std::to_string(i);resource(ctx,p,name,b.Get(),phase);resource(ctx,p,name+".after",b.Get(),1);}}
    }
    void finish(Packet& p) {
        if(p.traceStatus!="complete")p.complete=false;
        std::ostringstream o;o<<"{\"schema\":\"edvr-flat-draw-packet\",\"version\":1,\"frame\":"<<p.frame<<",\"q\":"<<p.q<<",\"vs\":\""<<std::hex<<p.vs<<"\",\"ps\":\""<<p.ps<<std::dec<<"\",\"complete\":"<<(p.complete?"true":"false")<<",\"executed\":"<<(p.executed?"true":"false")<<",\"reason\":"<<quote(p.reason)<<",\"draw\":{\"kind\":"<<quote(std::string(1,p.args.kind))<<",\"count\":"<<p.args.count<<",\"start\":"<<p.args.start<<",\"base\":"<<p.args.base<<",\"instances\":"<<p.args.instances<<",\"start_instance\":"<<p.args.startInstance<<"}";
        o<<",\"trace_file\":"<<quote(p.traceFile)<<",\"trace_status\":"<<quote(p.traceStatus);
        auto fields=[&](const char* name,const std::vector<Field>& f){o<<",\""<<name<<"\":[";for(size_t i=0;i<f.size();++i){if(i)o<<',';o<<"{\"name\":"<<quote(f[i].name)<<",\"type\":"<<quote(f[i].type)<<",\"status\":"<<quote(f[i].status)<<",\"value\":"<<quote(f[i].value)<<"}";}o<<']';};fields("original",p.original);fields("executed_pipeline",p.actual);
        o<<",\"resources\":[";for(size_t i=0;i<p.resources.size();++i){auto& r=p.resources[i];if(i)o<<',';o<<"{\"id\":"<<i<<",\"name\":"<<quote(r.name)<<",\"source\":"<<quote(identity(r.source.Get()))<<",\"type\":"<<quote(r.type)<<",\"desc_hex\":"<<quote(r.desc)<<",\"status\":"<<quote(r.status)<<",\"subresources\":[";for(size_t j=0;j<r.parts.size();++j){auto& a=r.parts[j];if(j)o<<',';o<<"{\"subresource\":"<<a.sub<<",\"status\":"<<quote(a.status)<<",\"row_pitch\":"<<a.rowPitch<<",\"depth_pitch\":"<<a.depthPitch<<",\"rows\":"<<a.rows<<",\"depth\":"<<a.depth<<",\"bytes\":"<<a.bytes<<",\"files\":[";for(size_t k=0;k<a.files.size();++k){if(k)o<<',';o<<quote(a.files[k]);}o<<"]}";}o<<"]}";}o<<"]}";
        const auto body=o.str();FILE* out=nullptr;const auto path=directory_+L"\\"+std::wstring(p.file.begin(),p.file.end())+L".json";
        bool ok=false;if(body.size()>(63ull<<20)||metadataWritten_>(63ull<<20)-body.size()){p.complete=false;++budgetOmissions_;}
        else if(!_wfopen_s(&out,path.c_str(),L"wb")&&out){ok=std::fwrite(body.data(),1,body.size(),out)==body.size();if(std::fclose(out)!=0)ok=false;}
        if(ok)metadataWritten_+=body.size();if(!ok)p.complete=false;++finished_;if(!p.complete)++incomplete_;
    }
public:
    void arm(const std::wstring& directory,uint64_t frame,Hash hash,Lookup lookup,uint64_t budget=1ull<<30) {
        cancel();directory_=directory;lastManifest_.clear();hash_=hash;lookup_=lookup;budget_=budget;armFrame_=frame;armMs_=GetTickCount64();allocated_=written_=metadataWritten_=0;selected_=finished_=incomplete_=skipped_=0;pairs_.clear();packetFiles_.clear();representativeReserved_=0;representativeMask_=priorityPairs_=budgetOmissions_=0;armed_=true;manifest("armed-no-priority-draw-observed");
    }
    void cancel(){for(auto& p:pending_){p->complete=false;p->reason="rearmed-or-cancelled";for(auto& r:p->resources)if(r.status=="queued"||r.status=="awaiting-original-draw")r.status="cancelled";finish(*p);}pending_.clear();armed_=false;allocated_=0;manifest("cancelled");}
    bool armed()const{return armed_;}unsigned selected()const{return selected_;}unsigned finished()const{return finished_;}unsigned incomplete()const{return incomplete_;}unsigned skipped()const{return skipped_;}
    bool active()const{return armed_||!pending_.empty();}
    void missing(void* token,const char* field,const char* reason){if(!token)return;auto& p=*static_cast<Packet*>(token);text(p.original,field,"unavailable",reason);p.complete=false;}
    void* before(ID3D11DeviceContext* ctx,uint64_t frame,UINT q,uint64_t vs,uint64_t ps,const Args& args,bool priority,bool queriesSafe,unsigned representative=0) {
        if(!armed_||(!priority&&!representative)||!ctx)return nullptr;if(frame-armFrame_>900||GetTickCount64()-armMs_>30000){armed_=false;manifest("expired");return nullptr;}
        if(priority){const auto key=std::make_pair(vs,ps);auto found=pairs_.find(key);if(found==pairs_.end()){if(priorityPairs_>=16){++skipped_;return nullptr;}found=pairs_.emplace(key,std::make_pair(0u,uint64_t(-1))).first;++priorityPairs_;}
            if(found->second.first>=2||found->second.second==frame){++skipped_;return nullptr;}++found->second.first;found->second.second=frame;
        }else {if(representative>4 || (representativeMask_&(1u<<representative))){++skipped_;return nullptr;}representativeMask_|=1u<<representative;}
        auto p=std::make_unique<Packet>();p->frame=frame;p->q=q;p->vs=vs;p->ps=ps;p->args=args;p->priority=priority;p->startMs=GetTickCount64();p->file="packet_"+std::to_string(frame)+"_"+std::to_string(q)+"_"+std::to_string(++serial_);
        text(p->original,"query.observer",queriesSafe?"no-active-count-query":"active-or-unknown",queriesSafe?"complete":"unsupported-query-state");if(!queriesSafe)p->complete=false;
        outputsBefore(ctx,*p);pipeline(ctx,*p,p->original,true);void* result=p.get();packetFiles_.push_back(p->file+".json");pending_.push_back(std::move(p));++selected_;manifest("readback-pending");return result;
    }
    void execution(ID3D11DeviceContext* ctx,void* token,ID3D11Buffer* indirect,UINT offset,const char* reason) {
        if(!token)return;auto& p=*static_cast<Packet*>(token);p.reason=reason?reason:"candidate";outputsBefore(ctx,p,2);pipeline(ctx,p,p.actual,true,2);
        if(indirect){bindResource(ctx,p,p.original,"draw.indirect_arguments",indirect,true,2);pod(p.original,"draw.indirect_offset","UINT",offset);
            D3D11_BUFFER_DESC desc{};indirect->GetDesc(&desc);const UINT size=p.args.kind=='Z'?20u:16u;
            if((p.args.kind!='Y'&&p.args.kind!='Z')||offset%4 || offset>desc.ByteWidth || size>desc.ByteWidth-offset){text(p.original,"draw.arguments","invalid","argument-buffer-bounds");p.complete=false;}
        }else if(p.args.kind=='?'||p.args.kind=='A'||p.args.kind=='Y'||p.args.kind=='Z'){text(p.original,"draw.arguments","unknown",p.args.kind=='A'?"unsupported-DrawAuto-count":"missing-indirect-arguments");p.complete=false;}
    }
    void after(ID3D11DeviceContext* ctx,void* token) {if(!token)return;auto& p=*static_cast<Packet*>(token);p.executed=true;for(auto& output:p.outputs){const size_t id=resource(ctx,p,output.first+".after",output.second.Get(),true);auto& r=p.resources[id];if(r.stage&&r.status=="awaiting-original-draw"){ctx->CopyResource(r.stage.Get(),r.source.Get());r.status="queued";}}}
    void abandoned(void* token){if(!token)return;auto& p=*static_cast<Packet*>(token);p.complete=false;p.reason="original-draw-not-observed";}
    void trace(uint64_t frame,const FlatTraceRing& ring,bool sealed) {
        bool needed=false;for(auto& p:pending_)if(p->frame==frame&&p->traceStatus=="pending-frame-close")needed=true;if(!needed)return;
        const FlatTraceFrameHeader* header=nullptr;const FlatTraceEvent* events=nullptr;std::string status="missing-frame";
        for(UINT i=0;i<kFlatTraceFrames;++i)if(ring.slotUsed[i]&&ring.headers[i].frame==frame){header=&ring.headers[i];events=ring.events[i];status=!sealed&&i==ring.slot?"in-flight":header->truncated?"overflow":!header->eventCount?"empty":"complete";break;}
        const std::string leaf="trace_"+std::to_string(frame)+".bin";
        if(status=="complete") {
            const uint64_t size=sizeof(FlatTraceHeader)+sizeof(*header)+uint64_t(header->eventCount)*sizeof(FlatTraceEvent);
            if(size>chunkCap_ || size>binaryBudget() || written_>binaryBudget()-size)status="trace-budget-cap";
            else {FILE* out=nullptr;const auto path=directory_+L"\\"+std::wstring(leaf.begin(),leaf.end());bool ok=false;
                if(!_wfopen_s(&out,path.c_str(),L"wb")&&out){FlatTraceHeader fileHeader;fileHeader.frameCount=1;
                    ok=std::fwrite(&fileHeader,1,sizeof(fileHeader),out)==sizeof(fileHeader)&&std::fwrite(header,1,sizeof(*header),out)==sizeof(*header)&&std::fwrite(events,sizeof(*events),header->eventCount,out)==header->eventCount;
                    if(std::fclose(out)!=0)ok=false;}
                if(ok)written_+=size;else status="trace-write-failed";
            }
        }
        for(auto& p:pending_)if(p->frame==frame){p->traceStatus=status;p->traceFile=status=="complete"?leaf:"";if(status!="complete")p->complete=false;}
        manifest("readback-pending");
    }
    void present(ID3D11DeviceContext* ctx,uint64_t frame) {
        if(!armed_&&pending_.empty())return;
        for(auto it=pending_.begin();it!=pending_.end();) {auto& p=**it;if(p.frame>=frame){++it;continue;}bool waiting=false;
            const bool timeout=GetTickCount64()-p.startMs>10000||frame-p.frame>300;
            for(size_t i=0;i<p.resources.size();++i){auto& r=p.resources[i];if(r.status=="awaiting-original-draw"){r.status="original-draw-not-observed";p.complete=false;continue;}if(r.status!="queued")continue;if(timeout){r.status="timeout";for(auto& part:r.parts)if(!part.done)part.status="timeout";p.complete=false;continue;}
                for(auto& part:r.parts){if(part.done)continue;D3D11_MAPPED_SUBRESOURCE mapped{};const HRESULT hr=ctx->Map(r.stage.Get(),part.sub,D3D11_MAP_READ,D3D11_MAP_FLAG_DO_NOT_WAIT,&mapped);
                    if(hr==DXGI_ERROR_WAS_STILL_DRAWING){waiting=true;continue;}if(FAILED(hr)){part.status=r.status="map-failed";p.complete=false;break;}
                    const bool ok=writeMapped(p.file+"_resource_"+std::to_string(i)+"_sub_"+std::to_string(part.sub),mapped,part);ctx->Unmap(r.stage.Get(),part.sub);part.done=true;
                    part.status=ok?"complete":writeStatus_;
                    if(!ok){r.status=writeStatus_;p.complete=false;break;}
                }if(r.status=="queued"&&std::all_of(r.parts.begin(),r.parts.end(),[](const Part& a){return a.done;}))r.status="complete";
            }
            if(!waiting){if(!p.executed)p.complete=false;finish(p);for(auto& r:p.resources)allocated_-=r.stage?r.estimated:0;it=pending_.erase(it);}else ++it;
        }if(armed_&&(frame-armFrame_>900||GetTickCount64()-armMs_>30000))armed_=false;
        manifest(!pending_.empty()?"readback-pending":armed_?selected_?"capturing":"armed-no-priority-draw-observed":selected_?"finished":"expired-no-priority-draw-observed");
    }
};
} // namespace edvr
