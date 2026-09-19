#include "../../src/d3d11/object_classification_probe.h"
#include <d3d11.h>
#include <wrl/client.h>
#include <windows.h>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;
namespace {
unsigned checks=0;
void check(bool ok,const char* what){++checks;if(!ok){std::printf("FAIL: %s\n",what);std::exit(1);}}
void hr(HRESULT value,const char* what){if(FAILED(value)){std::printf("FAIL: %s (0x%08lX)\n",what,(unsigned long)value);std::exit(1);}}
struct Device {
    ComPtr<ID3D11Device> d;ComPtr<ID3D11DeviceContext> c;
    Device(){D3D_FEATURE_LEVEL level{};hr(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&d,&level,&c),"WARP device");}
    ComPtr<ID3D11Buffer> buffer(UINT bytes,UINT bind,D3D11_USAGE usage,const void* data=nullptr,UINT stride=0){
        D3D11_BUFFER_DESC x{};x.ByteWidth=bytes;x.BindFlags=bind;x.Usage=usage;x.StructureByteStride=stride;
        if(stride)x.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        if(usage==D3D11_USAGE_DYNAMIC)x.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;
        if(usage==D3D11_USAGE_STAGING){x.BindFlags=0;x.CPUAccessFlags=D3D11_CPU_ACCESS_READ;}
        D3D11_SUBRESOURCE_DATA s{};s.pSysMem=data;ComPtr<ID3D11Buffer> out;
        hr(d->CreateBuffer(&x,data&&usage!=D3D11_USAGE_STAGING?&s:nullptr,&out),"buffer");return out;
    }
    ComPtr<ID3D11Texture2D> texture(UINT w,UINT h,DXGI_FORMAT format,const void* data,UINT pitch,UINT bind=D3D11_BIND_SHADER_RESOURCE){
        D3D11_TEXTURE2D_DESC x{};x.Width=w;x.Height=h;x.MipLevels=x.ArraySize=x.SampleDesc.Count=1;x.Format=format;x.BindFlags=bind;
        D3D11_SUBRESOURCE_DATA s{};s.pSysMem=data;s.SysMemPitch=pitch;ComPtr<ID3D11Texture2D> out;
        hr(d->CreateTexture2D(&x,data?&s:nullptr,&out),"texture");return out;
    }
};
template<class T>std::vector<uint8_t> bytes(const std::vector<T>& v){const auto* p=reinterpret_cast<const uint8_t*>(v.data());return {p,p+v.size()*sizeof(T)};}
void put32(std::vector<uint8_t>& v,size_t at,uint32_t value){check(at+4<=v.size(),"fixture put32 range");memcpy(v.data()+at,&value,4);}
std::array<uint32_t,16> key(uint32_t base){std::array<uint32_t,16> x{};for(uint32_t i=0;i<16;++i)x[i]=base+i;return x;}
void makeRecord(uint8_t* record,const std::array<uint32_t,16>& k,const std::vector<uint8_t>& pool,size_t poolAt,uint32_t firstRecord,uint32_t instances,uint32_t poolIndex){
    memset(record,0,240);memcpy(record,k.data(),64);memcpy(record+64,pool.data()+poolAt+28,4);
    memcpy(record+68,pool.data()+poolAt+320,4);memcpy(record+80,pool.data()+poolAt+4,24);
    memcpy(record+116,&firstRecord,4);memcpy(record+120,&instances,4);memcpy(record+124,&poolIndex,4);
}
std::vector<uint8_t> readFile(const std::wstring& p){FILE* f=nullptr;check(_wfopen_s(&f,p.c_str(),L"rb")==0&&f,"open generated file");fseek(f,0,SEEK_END);long n=ftell(f);check(n>=0,"generated file size");rewind(f);std::vector<uint8_t> out(static_cast<size_t>(n));check(fread(out.data(),1,out.size(),f)==out.size(),"read generated file");check(fclose(f)==0,"close generated file");return out;}
std::wstring joined(const wchar_t* dir,const wchar_t* file){std::wstring p=dir;if(!p.empty()&&p.back()!=L'\\'&&p.back()!=L'/')p+=L'\\';p+=file;return p;}

void stateTests(Device& x){
    std::vector<uint8_t> poolData(336,0x11),idData(16,0x22);auto pool=x.buffer(UINT(poolData.size()),D3D11_BIND_SHADER_RESOURCE,D3D11_USAGE_DYNAMIC,poolData.data(),336);auto ids=x.buffer(UINT(idData.size()),D3D11_BIND_VERTEX_BUFFER,D3D11_USAGE_DYNAMIC,idData.data());
    edvr::ObjectClassificationProbe p;p.arm(0);p.noteDraw(x.c.Get(),0,0,0,1,pool.Get(),ids.Get(),0,key(1).data());
    check(p.discoveryFrame()==0&&p.active(),"frame zero is valid discovery");
    D3D11_MAPPED_SUBRESOURCE mapped{};hr(x.c->Map(pool.Get(),0,D3D11_MAP_WRITE_DISCARD,0,&mapped),"write map");p.setFrame(1);p.noteMap(x.c.Get(),pool.Get(),0,D3D11_MAP_WRITE_DISCARD,S_OK,mapped.pData!=nullptr);memset(mapped.pData,0x33,poolData.size());x.c->Unmap(pool.Get(),0);p.noteUnmap(x.c.Get(),pool.Get(),0);
    check(p.observedWriteCount()==1&&p.writeCount()==1,"writable Map and Unmap form one complete epoch");
    p.noteWrite(x.c.Get(),pool.Get(),edvr::ObjectClassificationProbe::Update);p.noteWrite(x.c.Get(),pool.Get(),edvr::ObjectClassificationProbe::CopyResource,ids.Get());
    check(p.observedWriteCount()==3,"update and copy each advance generation");
    const auto nonempty=p.observedWriteCount();p.noteWrite(x.c.Get(),pool.Get(),edvr::ObjectClassificationProbe::Update,nullptr,0,12,12);check(p.observedWriteCount()==nonempty,"known empty writes do not advance generation");
    p.setFrame(3);p.noteDraw(x.c.Get(),3,1,0,1,pool.Get(),ids.Get(),0,key(2).data());check(p.drawCount()==0,"eye one cannot select");
    p.noteDraw(x.c.Get(),3,0,0,1,pool.Get(),ids.Get(),0,key(2).data());check(p.selectedFrame()==3&&p.drawCount()==1,"three discovery frames precede selection");
    const auto before=p.summary();p.unknownWrites();p.noteDraw(x.c.Get(),3,0,1,1,pool.Get(),ids.Get(),0,key(3).data());const auto after=p.summary();
    check(after.snapshots>=before.snapshots+2,"unknown writes force distinct resource snapshots");
    check(after.writes==before.writes+2,"unknown writes recorded for both nominated resources");
    p.foreignWrite();p.noteDraw(x.c.Get(),3,0,2,1,pool.Get(),ids.Get(),0,key(4).data());check(p.summary().foreignWrites==1&&p.summary().snapshots>=after.snapshots+2,"foreign epoch prevents snapshot reuse");
    p.stage(x.c.Get(),2,0,7,nullptr,nullptr,nullptr,0);p.stage(x.c.Get(),3,1,7,nullptr,nullptr,nullptr,0);check(p.summary().stageRejects==2,"wrong frame and eye stage rejected");
    const ULONG heldBefore=pool->AddRef();pool->Release();p.reset();const ULONG heldAfter=pool->AddRef();pool->Release();check(heldAfter<heldBefore,"reset releases retained resources");

    auto staging=x.buffer(64,0,D3D11_USAGE_STAGING);edvr::ObjectClassificationProbe read;read.arm(0);read.noteDraw(x.c.Get(),0,0,0,1,staging.Get(),ids.Get(),0,nullptr);
    hr(x.c->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped),"read map");read.noteMap(x.c.Get(),staging.Get(),0,D3D11_MAP_READ,S_OK,mapped.pData!=nullptr);x.c->Unmap(staging.Get(),0);read.noteUnmap(x.c.Get(),staging.Get(),0);
    check(read.observedWriteCount()==0,"read-only Map and Unmap do not advance generation");

    edvr::ObjectClassificationProbe open;open.arm(0);open.noteDraw(x.c.Get(),0,0,0,1,pool.Get(),ids.Get(),0,nullptr);
    hr(x.c->Map(pool.Get(),0,D3D11_MAP_WRITE_DISCARD,0,&mapped),"open write map");open.noteMap(x.c.Get(),pool.Get(),0,D3D11_MAP_WRITE_DISCARD,S_OK,mapped.pData!=nullptr);open.noteDraw(x.c.Get(),3,0,0,1,pool.Get(),ids.Get(),0,nullptr);check(open.summary().openMapSnapshots==1,"open writable Map refuses source snapshot");x.c->Unmap(pool.Get(),0);open.noteUnmap(x.c.Get(),pool.Get(),0);check(open.summary().stackSamples==2,"Map and matching Unmap retain both caller stacks");
    const auto unavailableSnapshots=open.summary().snapshots;open.noteDraw(x.c.Get(),3,0,1,1,pool.Get(),ids.Get(),0,nullptr);
    check(open.summary().snapshots==unavailableSnapshots+1&&open.summary().openMapSnapshots==1,"completed Map retries its unavailable snapshot without duplicating unchanged IDs");
    open.noteUnmap(x.c.Get(),pool.Get(),0);check(open.summary().unmatchedUnmaps==1&&open.observedWriteCount()==2,"unmatched Unmap conservatively invalidates generation");

    edvr::ObjectClassificationProbe duplicate;duplicate.arm(0);duplicate.noteDraw(x.c.Get(),0,0,0,1,pool.Get(),ids.Get(),0,nullptr);duplicate.noteMap(x.c.Get(),pool.Get(),0,D3D11_MAP_WRITE_DISCARD,S_OK,true);duplicate.noteMap(x.c.Get(),pool.Get(),0,D3D11_MAP_WRITE_DISCARD,S_OK,true);check(duplicate.summary().duplicateMaps==1&&duplicate.observedWriteCount()==2,"duplicate outstanding Map is explicit and unmatched");

    D3D11_BUFFER_DESC unsafeDesc{};unsafeDesc.ByteWidth=336;unsafeDesc.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS;unsafeDesc.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;unsafeDesc.StructureByteStride=336;ComPtr<ID3D11Buffer> unsafe;hr(x.d->CreateBuffer(&unsafeDesc,nullptr,&unsafe),"unsafe buffer");
    edvr::ObjectClassificationProbe unsafeProbe;unsafeProbe.arm(0);unsafeProbe.noteDraw(x.c.Get(),0,0,0,1,unsafe.Get(),ids.Get(),0,nullptr);unsafeProbe.noteDraw(x.c.Get(),3,0,0,1,unsafe.Get(),ids.Get(),0,nullptr);check(unsafeProbe.summary().unsafeAssociations==1,"UAV nominated source is unavailable");

    edvr::ObjectClassificationProbe cap;cap.arm(0);cap.noteDraw(x.c.Get(),0,0,0,1,pool.Get(),ids.Get(),0,nullptr);for(unsigned i=0;i<520;++i)cap.noteWrite(x.c.Get(),pool.Get(),edvr::ObjectClassificationProbe::Update);auto capped=cap.summary();check(capped.writes==512&&capped.observedWrites==520&&capped.writeOverflow==8&&capped.unmatchedWrites>0,"event cap never freezes generation");
    cap.setFrame(3);for(unsigned i=0;i<520;++i)cap.noteDraw(x.c.Get(),3,0,i,1,pool.Get(),ids.Get(),0,nullptr);check(cap.summary().draws==512&&cap.summary().drawOverflow==8,"draw cap explicit");

    edvr::ObjectClassificationProbe resources;resources.arm(0);std::vector<ComPtr<ID3D11Buffer>> many;for(unsigned i=0;i<66;++i){many.push_back(x.buffer(16,D3D11_BIND_VERTEX_BUFFER,D3D11_USAGE_DEFAULT));resources.noteDraw(x.c.Get(),0,0,0,1,many.back().Get(),ids.Get(),0,nullptr);}check(resources.summary().resources==64&&resources.summary().resourceOverflow>0,"resource cap explicit");

    auto copySource=x.buffer(UINT(poolData.size()),D3D11_BIND_SHADER_RESOURCE,D3D11_USAGE_DYNAMIC,poolData.data(),336);edvr::ObjectClassificationProbe chain;chain.arm(0);chain.noteDraw(x.c.Get(),0,0,0,1,pool.Get(),ids.Get(),0,nullptr);chain.noteWrite(x.c.Get(),pool.Get(),edvr::ObjectClassificationProbe::CopyResource,copySource.Get());
    hr(x.c->Map(copySource.Get(),0,D3D11_MAP_WRITE_DISCARD,0,&mapped),"copy source map");chain.noteMap(x.c.Get(),copySource.Get(),0,D3D11_MAP_WRITE_DISCARD,S_OK,mapped.pData!=nullptr);x.c->Unmap(copySource.Get(),0);chain.noteUnmap(x.c.Get(),copySource.Get(),0);check(chain.observedWriteCount()==2&&chain.summary().stackSamples==3,"buffer copy source acquires subsequent Map and Unmap route");const auto chainWrites=chain.observedWriteCount();chain.unknownWrites();check(chain.observedWriteCount()==chainWrites+3,"unknown writes invalidate pool, IDs, and watched copy source");
}

void fixture(Device& x,const wchar_t* directory){
    CreateDirectoryW(directory,nullptr);check(GetLastError()==ERROR_ALREADY_EXISTS||GetFileAttributesW(directory)!=INVALID_FILE_ATTRIBUTES,"fixture output directory");
    std::vector<uint8_t> poolA(672),poolB(336),idsA(24),idsB(8);
    for(size_t i=0;i<poolA.size();++i)poolA[i]=uint8_t((i*7+3)&255);for(size_t i=0;i<poolB.size();++i)poolB[i]=uint8_t((i*11+5)&255);
    put32(poolA,28,0xA0A00028);put32(poolA,320,0xA0A00140);put32(poolA,336+28,0xB0B00028);put32(poolA,336+320,0xB0B00140);put32(poolB,28,0xC0C00028);put32(poolB,320,0xC0C00140);
    uint32_t idWordsA[]={0,0x11111111,1,0x22222222,0,0x33333333};memcpy(idsA.data(),idWordsA,sizeof(idWordsA));uint32_t idWordsB[]={0,0x44444444};memcpy(idsB.data(),idWordsB,sizeof(idWordsB));
    auto pool=x.buffer(UINT(poolA.size()),D3D11_BIND_SHADER_RESOURCE,D3D11_USAGE_DEFAULT,poolA.data(),336);auto ids=x.buffer(UINT(idsA.size()),D3D11_BIND_VERTEX_BUFFER,D3D11_USAGE_DYNAMIC,idsA.data());
    auto missingPool=x.buffer(UINT(poolB.size()),D3D11_BIND_SHADER_RESOURCE,D3D11_USAGE_DEFAULT,poolB.data(),336);auto missingIds=x.buffer(UINT(idsB.size()),D3D11_BIND_VERTEX_BUFFER,D3D11_USAGE_DEFAULT,idsB.data());
    auto source=x.buffer(UINT(poolA.size()),D3D11_BIND_SHADER_RESOURCE,D3D11_USAGE_DEFAULT,poolA.data(),336);
    auto k0=key(0x1000),k1=key(0x2000),k2=key(0x3000);auto& p=edvr::objectClassificationProbe;p.arm(20);
    p.noteDraw(x.c.Get(),20,0,0,1,pool.Get(),ids.Get(),0,k0.data(),0xEB5234DB6ADB491Dull);
    p.setFrame(21);D3D11_MAPPED_SUBRESOURCE m{};hr(x.c->Map(ids.Get(),0,D3D11_MAP_WRITE_DISCARD,0,&m),"fixture ids map");p.noteMap(x.c.Get(),ids.Get(),0,D3D11_MAP_WRITE_DISCARD,S_OK,m.pData!=nullptr);memcpy(m.pData,idsA.data(),idsA.size());x.c->Unmap(ids.Get(),0);p.noteUnmap(x.c.Get(),ids.Get(),0);
    p.noteWrite(x.c.Get(),pool.Get(),edvr::ObjectClassificationProbe::CopyResource,source.Get());x.c->CopyResource(pool.Get(),source.Get());
    p.noteDraw(x.c.Get(),21,0,0,1,pool.Get(),ids.Get(),0,k0.data(),0xEB5234DB6ADB491Dull);p.noteDraw(x.c.Get(),22,0,0,1,pool.Get(),ids.Get(),0,k0.data(),0xEB5234DB6ADB491Dull);
    p.setFrame(23);p.noteDraw(x.c.Get(),23,0,0,1,pool.Get(),ids.Get(),0,k0.data(),0xEB5234DB6ADB491Dull);
    std::vector<uint8_t> poolSecond=poolA;for(size_t i=0;i<336;++i)poolSecond[i]=uint8_t(0xD0+(i&15));put32(poolSecond,336+28,0xB0B00028);put32(poolSecond,336+320,0xB0B00140);
    x.c->UpdateSubresource(pool.Get(),0,nullptr,poolSecond.data(),0,0);p.noteWrite(x.c.Get(),pool.Get(),edvr::ObjectClassificationProbe::Update);
    p.noteDraw(x.c.Get(),23,0,1,1,pool.Get(),ids.Get(),8,k1.data(),0xDE545DC8EE4FBB87ull);
    p.noteDraw(x.c.Get(),23,0,2,1,missingPool.Get(),missingIds.Get(),0,k2.data(),0x61AE8EB05FDC18DDull);
    check(p.summary().snapshots==5&&p.summary().unobservedSources==2,"fixture includes rewritten and missing provenance");
    std::vector<float> scene(16,0.5f);scene[5]=0.75f;auto sceneTex=x.texture(4,4,DXGI_FORMAT_R32_FLOAT,scene.data(),16);
    struct Float2{float x,y;};std::vector<Float2> coverage(16,{0,0});coverage[5]={1,0.25f};coverage[6]={2,0.5f};auto coverageTex=x.texture(4,4,DXGI_FORMAT_R32G32_FLOAT,coverage.data(),32,D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE);
    std::vector<uint8_t> mesh(3*240);makeRecord(mesh.data(),k0,poolA,0,0,1,0);makeRecord(mesh.data()+240,k1,poolSecond,336,1,1,1);makeRecord(mesh.data()+480,k2,poolB,0,2,1,0);
    auto meshBuffer=x.buffer(UINT(mesh.size()),D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS,D3D11_USAGE_DEFAULT,mesh.data(),240);
    std::vector<uint32_t> colour(16,0xFF203040);colour[5]=0xFF00A0E0;auto colourTex=x.texture(4,4,DXGI_FORMAT_R8G8B8A8_UNORM,colour.data(),16);
    const auto writesBefore=p.writeCount();p.stage(x.c.Get(),22,0,900,sceneTex.Get(),coverageTex.Get(),meshBuffer.Get(),3,colourTex.Get());check(!p.sealed(),"wrong-frame fixture stage rejected");
    D3D11_QUERY_DESC queryDesc{D3D11_QUERY_OCCLUSION_PREDICATE,0};ComPtr<ID3D11Predicate> predicate;hr(x.d->CreatePredicate(&queryDesc,&predicate),"fixture predicate");x.c->Begin(predicate.Get());x.c->End(predicate.Get());x.c->SetPredication(predicate.Get(),FALSE);
    p.stage(x.c.Get(),23,0,900,sceneTex.Get(),coverageTex.Get(),meshBuffer.Get(),3,colourTex.Get());check(p.sealed()&&!p.active()&&p.writeCount()==writesBefore,"own GPU copies do not enter write ledger");
    ComPtr<ID3D11Predicate> restored;BOOL predicateValue=TRUE;x.c->GetPredication(&restored,&predicateValue);check(restored==predicate&&!predicateValue,"stage copies restore caller predication");x.c->SetPredication(nullptr,FALSE);
    check(p.write(x.c.Get(),directory,L"fixture"),"write fixture JSON and BIN");
    const auto json=readFile(joined(directory,L"classification_fixture.json"));const std::string text(json.begin(),json.end());check(text.find("edvr_object_classification_v1")!=std::string::npos&&text.find("scene_colour")!=std::string::npos,"fixture JSON schema and colour blob");
    const auto bin=readFile(joined(directory,L"classification_fixture.bin"));check(bin.size()==2688,"fixture binary exact size");check(memcmp(bin.data(),poolA.data(),poolA.size())==0,"first draw retains original pool bytes");check(memcmp(bin.data()+696,poolSecond.data(),poolSecond.size())==0,"rewrite produces a distinct pool snapshot");
}
}

int wmain(int argc,wchar_t** argv){if(argc!=2){std::puts("usage: object_classification_test.exe <output-directory>");return 2;}Device device;stateTests(device);fixture(device,argv[1]);std::printf("object classification: %u checks passed; fixture written\n",checks);return 0;}
