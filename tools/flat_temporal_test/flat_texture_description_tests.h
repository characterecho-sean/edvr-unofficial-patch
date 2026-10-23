#pragma once
#include "../../src/d3d11/flat_forward_light.h"
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>

namespace texture_description_test {
using namespace edvr::forward_light;
struct Reader {
    TextureDescription value{2496,1404,1,1,1,19};bool texture=true,partialFailure=false;unsigned reads=0;
    bool operator()(TextureDescription& out){++reads;if(texture||partialFailure)out=value;return texture;}
};
inline bool same(const TextureObservation& a,const TextureObservation& b) {
    const auto& x=a.surface;const auto& y=b.surface;
    return x.resource==y.resource&&x.width==y.width&&x.height==y.height&&x.mip==y.mip&&x.slice==y.slice&&
        x.slices==y.slices&&x.samples==y.samples&&x.known==y.known&&a.format==b.format;
}
// Independent pre-consolidation structural oracle. The old wrapper retained
// the identity/view coordinates even when QI failed, while its zero-initialized
// D3D texture descriptor supplied zero extent/sample/format in that case.
inline TextureObservation legacy(uint64_t id,unsigned mip,unsigned slice,unsigned slices,const Reader& r) {
    TextureObservation out;out.surface.resource=id;out.surface.mip=mip;out.surface.slice=slice;out.surface.slices=slices;
    if(!id||!r.texture)return out;
    out.surface.width=r.value.width;out.surface.height=r.value.height;out.surface.samples=r.value.samples;out.format=r.value.format;
    out.surface.known=r.value.arraySize==1&&r.value.mipLevels==1&&r.value.samples==1&&mip==0&&slice==0&&slices==1;
    return out;
}
inline std::string region(const char* start,const char* end) {
    std::ifstream file("src/d3d11/flat_forward_light_runtime.inl");std::string line,text;bool found=false;
    while(std::getline(file,line)) {
        if(!found&&line.find(start)!=std::string::npos)found=true;
        if(!found)continue;if(!text.empty()&&line.find(end)!=std::string::npos)break;
        text+=line+'\n';if(text.size()>8192)return {};
    }
    return text;
}
inline unsigned count(const std::string& text,const char* word) {
    unsigned n=0;size_t pos=0;while((pos=text.find(word,pos))!=std::string::npos){++n;pos+=std::strlen(word);}return n;
}
inline unsigned run() {
    unsigned checks=0,failures=0;
    const auto expect=[&](bool ok,const char* why){++checks;if(!ok){++failures;std::printf("FAIL: texture description %s\n",why);}};
    Reader reader;const auto good=describeTexture(17,0,0,1,reader);
    expect(reader.reads==1&&good.surface.known&&good.surface.resource==17&&good.surface.width==2496&&good.surface.height==1404&&good.format==19,
        "one immutable-read callback supplies complete valid surface and format");
    reader.reads=0;const auto missing=describeTexture(0,3,4,5,reader);
    expect(!reader.reads&&!missing.surface.known&&!missing.surface.width&&!missing.format&&missing.surface.mip==3&&missing.surface.slice==4&&missing.surface.slices==5,
        "null resource never calls reader and retains requested view coordinates");
    reader.texture=false;reader.partialFailure=true;reader.reads=0;
    const auto unsupported=describeTexture(17,0,0,1,reader);
    expect(reader.reads==1&&!unsupported.surface.known&&!unsupported.surface.width&&!unsupported.surface.height&&!unsupported.surface.samples&&!unsupported.format,
        "failed type query discards even a partially populated callback descriptor");
    // Exhaust actual helper outputs over shape and DXGI-format combinations.
    // Format is deliberately not a structural whitelist: DSV/SRV validators
    // still apply their separate exact resource/view format requirements.
    unsigned cases=0;
    for(uint64_t id:{0ull,17ull})for(bool isTexture:{false,true})
    for(unsigned mips:{0u,1u,2u})for(unsigned arrays:{0u,1u,2u})for(unsigned samples:{0u,1u,2u,8u})
    for(unsigned mip:{0u,1u})for(unsigned slice:{0u,1u})for(unsigned slices:{0u,1u,2u})
    for(unsigned format:{0u,19u,20u,21u,27u,28u,39u,40u,41u,87u,103u,191u}) {
        Reader r;r.texture=isTexture;r.value={2496,1404,mips,arrays,samples,format};
        const auto actual=describeTexture(id,mip,slice,slices,r),expected=legacy(id,mip,slice,slices,r);
        ++cases;if(!same(actual,expected)||r.reads!=(id?1u:0u)){++failures;std::printf("FAIL: texture description oracle case %u\n",cases);return failures;}
    }
    ++checks;
    Reader current;auto snapshot=describeTexture(17,0,0,1,current);
    current.value={3840,2160,1,1,1,39};
    auto next=describeTexture(17,0,0,1,current);
    expect(current.reads==2&&snapshot.surface.width==2496&&snapshot.format==19&&next.surface.width==3840&&next.format==39,
        "same pointer is described afresh and prior value snapshot does not alias mutable callback storage");
    current.texture=false;const auto gone=describeTexture(17,0,0,1,current);
    expect(current.reads==3&&!gone.surface.known&&!gone.format&&next.surface.known,
        "no persistent success or format cache survives a later failed read of the same identity");
    Reader independent;const auto other=describeTexture(18,0,0,1,independent);
    expect(!plane(snapshot.surface,other.surface)&&independent.reads==1,"equal descriptors do not merge distinct resource identities");
    // Feed both observations through the actual Ledger; no domain/publication
    // rule is bypassed by the new descriptor helper, including known rejection.
    for(unsigned variant=0;variant<5;++variant) {
        Reader r;if(variant==1)r.texture=false;if(variant==2)r.value.samples=4;if(variant==3)r.value.arraySize=2;if(variant==4)r.value.mipLevels=2;
        auto actual=describeTexture(17,0,0,1,r),expected=legacy(17,0,0,1,r);
        Ledger a,b;a.reset(19);b.reset(19);unsigned char camera[96]{};Source source{90,1,2,3,19,1,0,4096};
        a.observe(actual.surface,Role::Origin,true,true,source,camera,2);b.observe(expected.surface,Role::Origin,true,true,source,camera,2);
        expect(a.used==b.used&&a.overflow==b.overflow&&(a.used==0||
            (a.domains[0].failure==b.domains[0].failure&&a.domains[0].hasProjection==b.domains[0].hasProjection&&a.domains[0].origins==b.domains[0].origins)),
            "actual Ledger receives identical structural observations for valid and unsupported textures");
    }
    // Limited bridge wiring, not a claim that fake callbacks measure driver
    // calls. These checks connect the behavioral callback budget to the actual
    // QI/GetDesc adapter and verify the two callers no longer repeat that work.
    const auto adapter=region("forward_light::Surface forwardTexture(","bool forwardSrv(");
    const auto srv=region("bool forwardSrv(","bool forwardDsvActual(");
    const auto dsv=region("bool forwardDsvActual(","bool forwardDsv(");
    expect(count(adapter,"forward_light::describeTexture(")==1&&count(adapter,"->QueryInterface(")==1&&count(adapter,"->GetDesc(")==1&&
        adapter.find("if(!texture)return false;")!=std::string::npos&&adapter.find("if(format)*format=observed.format;")!=std::string::npos,
        "actual adapter links one callback request to one QI and at most one texture GetDesc");
    expect(count(srv,"forwardTexture(")==1&&srv.find("out=forwardTexture(resource.Get(),")!=std::string::npos&&
        srv.find(",&format)")!=std::string::npos&&srv.find("QueryInterface")==std::string::npos&&
        srv.find("texture->GetDesc")==std::string::npos,
        "SRV path reuses format from its one same-resource texture observation");
    expect(count(dsv,"forwardTexture(")==1&&dsv.find("value.surface=forwardTexture(value.resource.Get(),")!=std::string::npos&&
        dsv.find(",&value.format)")!=std::string::npos&&dsv.find("QueryInterface")==std::string::npos&&
        dsv.find("texture->GetDesc")==std::string::npos,
        "DSV cache miss reuses format from its one same-resource texture observation");
    expect(srv.find("PSGetShaderResources(slot,1,&view)")!=std::string::npos&&srv.find("view->GetResource(&resource)")!=std::string::npos&&
        srv.find("view->GetDesc(&v)")!=std::string::npos&&srv.find("if(!resource)return false;")!=std::string::npos,
        "SRV still reads actual nullable binding, resource and immutable view descriptor");
    expect(dsv.find("OMGetRenderTargets(0,nullptr,&current)")!=std::string::npos&&dsv.find("dsv->GetResource(&value.resource)")!=std::string::npos&&
        dsv.find("dsv->GetDesc(&value.view)")!=std::string::npos&&dsv.find("OMGetDepthStencilState(&state,&ref)")!=std::string::npos&&
        dsv.find("state->GetDesc(&value)")!=std::string::npos&&dsv.find("D3D11_DSV_READ_ONLY_DEPTH")!=std::string::npos,
        "DSV still queries actual view and depth state, describing on cache miss with read-only handling");
    std::printf("texture description: %u checks, %u oracle cases, %u failure(s)\n",checks,cases,failures);return failures;
}
} // namespace texture_description_test
