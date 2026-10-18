#pragma once
#include "../../src/common/runtime_profile.h"

// Exercise the production resolver, prep shader, backend reset argument and
// displayed pixels. Identical camera inputs cannot tell travel from a rebase.
inline void originAmbiguityGpuTests(ID3D11Device* device,ID3D11DeviceContext* context,
    edvr::RuntimeProfile fixtureProfile=edvr::RuntimeProfile::LegacyVr) {
    using namespace edvr;
    const bool previousXessCapture=xessCaptureInputs;
    xessCaptureInputs=true;
    const RuntimeProfile previousProfile=runtimeProfileValue();
    runtimeProfileSet(fixtureProfile); // Flat with capture off proves the production reset is independent
    const FlatMonoResolveMode modes[]={FlatMonoResolveMode::Dlaa,FlatMonoResolveMode::Dlss,
        FlatMonoResolveMode::Fsr,FlatMonoResolveMode::Xess};
    struct Case {float before,now;bool reset;};
    const Case cases[]={
        {80,20,true}, // real inward travel, or a coordinate re-anchor: same observable input
        {80,20,true}, // a genuine re-anchor must receive the same conservative decision
        {5000,15,true}, {5020,20,true}, // unknown coordinate change and unknown physical residual
        {80,30,false}, {80,29.999f,true}, // exactly50 versus strictly over50 on one axis
        {400,480,false}, {-400,-480,false}, {400,320,false}};
    auto pixel=[&](ID3D11ShaderResourceView* out) {
        uint32_t value=0;ComPtr<ID3D11Resource> resource;ComPtr<ID3D11Texture2D> image;
        if(out)out->GetResource(resource.GetAddressOf());if(resource)resource.As(&image);
        check(image && readPixel(context,image.Get(),&value,4),"origin ambiguity: displayed pixel readable");
        return value;
    };
    auto currentImage=[&](ID3D11ShaderResourceView* out,const std::vector<uint32_t>& expected) {
        ComPtr<ID3D11Resource> resource;ComPtr<ID3D11Texture2D> image;
        if(out)out->GetResource(resource.GetAddressOf());if(resource)resource.As(&image);
        std::vector<unsigned char> actual;
        return image && readWhole(context,image.Get(),actual,4) &&
            actual.size()==expected.size()*sizeof(uint32_t) &&
            std::memcmp(actual.data(),expected.data(),actual.size())==0;
    };
    for(const auto mode:modes)for(const auto& c:cases) {
        flatMonoResolveReset();steadygpu::Rig r(device,context);r.start(mode);
        r.f.jitterCycle=mode==FlatMonoResolveMode::Xess?16u:8u;
        r.f.staticScene=true;r.fillZ(2.5e-5f); // near .025 / z1000
        r.f.camera[5][0]=r.f.previousCamera[5][0]=c.before;
        if(c.reset){r.setRecord(1);r.put(8,8,1,2.5e-5f);} // joined engine pixel must reset too
        r.run("origin ambiguity: SDK baseline");r.f.reset=false;
        r.f.previousCamera[5][0]=c.before;r.f.camera[5][0]=c.now;
        for(UINT y=0;y<steadygpu::H;++y)for(UINT x=0;x<steadygpu::W;++x)
            r.rgba[size_t(y)*steadygpu::W+x]=((x+y)&1)?0xffc0c0c0u:0xff404040u;
        const auto before=flatMonoResolveStats();ComPtr<ID3D11ShaderResourceView> out;
        const auto seen=r.run("origin ambiguity: SDK decision",true,&out);
        const auto after=flatMonoResolveStats();
        check(seen.reset==c.reset && after.lastReset==c.reset,
            "origin ambiguity: every SDK receives the production reset decision");
        if(c.reset) {
            check((mode==FlatMonoResolveMode::Xess?seen.mask.empty():seen.refusedAll()==steadygpu::W*steadygpu::H) && seen.noMotion(0,0,steadygpu::W,steadygpu::H) &&
                  currentImage(out.Get(),r.rgba) && after.originRebases==before.originRebases+1 &&
                  after.acceptedResets==before.acceptedResets+1 && after.currentContinueRun==0,
                "origin ambiguity: inward/re-anchor input rejects all history and displays current color");
            std::memcpy(r.f.previousCamera,r.f.camera,sizeof(r.f.camera));
            const auto recovered=r.run("origin ambiguity: frame after reset");
            check(!recovered.reset && (mode==FlatMonoResolveMode::Xess?recovered.mask.empty():
                  recovered.mask.size()==steadygpu::W*steadygpu::H && recovered.mask[4*steadygpu::W+4]==0) &&
                  recovered.noMotion(4,4,1,1) && flatMonoResolveStats().currentContinueRun==1,
                "origin ambiguity: immediately following camera-only frame resumes history");
        } else {
            const float expected=(c.now-c.before)/1000.f*(steadygpu::W*.5f);
            const size_t center=(8*steadygpu::W+8)*2;
            check(seen.motion.size()>center+1 && std::abs(seen.motion[center]-expected)<.005f &&
                  std::abs(seen.motion[center+1])<.001f && after.originRebases==before.originRebases,
                "origin ambiguity: continued positive/negative travel retains signed physical translation");
        }
    }
    // Nonzero jitter and foreground coverage must not bypass the same reset.
    // HDR finishes in the game's color target, so inspect that target directly.
    for(const auto mode:modes) {
        flatMonoResolveReset();steadygpu::Rig r(device,context);r.start(mode);
        r.f.jitterCycle=mode==FlatMonoResolveMode::Xess?16u:8u;
        r.f.hdr=true;r.f.staticScene=true;r.f.jitterX=.25f;r.f.previousJitterX=-.25f;
        std::vector<uint32_t> rgb(size_t(steadygpu::W)*steadygpu::H,hdrgpu::pack(.5,.5,.5));
        auto hdr=texture(device,steadygpu::W,steadygpu::H,DXGI_FORMAT_R11G11B10_FLOAT,
            D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET,rgb.data(),steadygpu::W*4);
        auto hdrView=view(device,hdr.Get());r.f.color=hdrView.Get();
        std::vector<float> fg(size_t(steadygpu::W)*steadygpu::H*4,0.f);
        for(size_t i=0;i<fg.size();i+=4){fg[i]=.5f;fg[i+2]=.01f;fg[i+3]=1;}
        auto foreground=texture(device,steadygpu::W,steadygpu::H,DXGI_FORMAT_R32G32B32A32_FLOAT,
            D3D11_BIND_SHADER_RESOURCE,fg.data(),steadygpu::W*16);
        auto foregroundView=view(device,foreground.Get());r.f.foregroundMotion=foregroundView.Get();
        r.f.foregroundQualified=true;r.f.foregroundFrame=r.f.frame;
        r.put(8,8,-2,.01f); // final foreign owner at the same scene depth
        r.f.camera[5][0]=r.f.previousCamera[5][0]=80;
        r.run("origin ambiguity: jittered HDR foreground baseline");r.f.reset=false;
        r.f.foregroundFrame=r.f.frame;
        const auto control=r.run("origin ambiguity: accepted HDR foreground control");
        const size_t center=(8*steadygpu::W+8)*2;
        check(!control.reset && (mode==FlatMonoResolveMode::Xess?control.mask.empty():
              control.mask.size()==steadygpu::W*steadygpu::H && control.mask[8*steadygpu::W+8]==0) && control.motion.size()>center+1 &&
              std::abs(control.motion[center]-.5f)<.001f && std::abs(control.motion[center+1])<.001f,
            "origin ambiguity: positive control admits the marked foreground's real motion");
        r.f.camera[5][0]=20;r.f.foregroundFrame=r.f.frame;
        std::fill(rgb.begin(),rgb.end(),hdrgpu::pack(.25,.25,.25));
        context->UpdateSubresource(hdr.Get(),0,nullptr,rgb.data(),steadygpu::W*4,0);
        const auto before=flatMonoResolveStats();const auto seen=r.run("origin ambiguity: HDR foreground reset");
        // The game target is packed R11G11B10F, distinct from the SDK's fp16
        // output. Verify every current pixel, including the marked foreground.
        check(currentImage(hdrView.Get(),rgb) &&
              seen.reset && (mode==FlatMonoResolveMode::Xess?seen.mask.empty():seen.refusedAll()==steadygpu::W*steadygpu::H) && seen.noMotion(0,0,steadygpu::W,steadygpu::H) &&
              flatMonoResolveStats().originRebases==before.originRebases+1,
            "origin ambiguity: HDR current-color finish and jittered foreground reject pre-reset history");
    }
    // TAA's actual output exposes the old bug: the old code blends128 with64
    // rather than showing64. The next frame must then reuse the newly seeded64.
    flatMonoResolveReset();steadygpu::Rig r(device,context);r.start(FlatMonoResolveMode::Taa);
    r.f.staticScene=true;r.f.camera[5][0]=r.f.previousCamera[5][0]=80;
    r.run("origin ambiguity: TAA baseline");r.f.reset=false;r.f.camera[5][0]=20;
    for(UINT y=0;y<steadygpu::H;++y)for(UINT x=0;x<steadygpu::W;++x)
        r.rgba[size_t(y)*steadygpu::W+x]=((x+y)&1)?0xffc0c0c0u:0xff404040u;
    ComPtr<ID3D11ShaderResourceView> out;
    r.run("origin ambiguity: TAA reset",true,&out);
    check(flatMonoResolveStats().lastReset && pixel(out.Get())==0xff404040u,
        "origin ambiguity: TAA displays current checker rather than absorbed history");
    std::memcpy(r.f.previousCamera,r.f.camera,sizeof(r.f.camera));
    for(UINT y=0;y<steadygpu::H;++y)for(UINT x=0;x<steadygpu::W;++x)
        r.rgba[size_t(y)*steadygpu::W+x]=((x+y)&1)?0xff404040u:0xffc0c0c0u;
    r.run("origin ambiguity: TAA recovery",true,&out);
    const unsigned recovered=pixel(out.Get())&255u;
    check(!flatMonoResolveStats().lastReset && recovered>64 && recovered<100,
        "origin ambiguity: TAA resumes using the reset frame's current history");
    flatMonoResolveReset();runtimeProfileSet(previousProfile);
    xessCaptureInputs=previousXessCapture;
}
