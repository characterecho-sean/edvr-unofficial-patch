// Exercise the shipped SDK prep and resolver contract through backend stubs.
// The adapter's marker/history production has separate tests; these cases pin
// the actual inputs handed to each backend and refusals before any H mutation.
#pragma once

// The scenario, on whichever prep the resolver holds (the shipped one, or a mutated one a mutation run installs).
inline void sdkForegroundScenario(ID3D11Device* device, ID3D11DeviceContext* context) {
    using namespace edvr;
    ResolveFixture fixture(device,context);
    constexpr UINT w=ResolveFixture::w,h=ResolveFixture::h;
    std::vector<uint32_t> rgb(w*h,hdrgpu::pack(20,30,40));
    std::vector<float> depths(w*h,.01f),markers(w*h*2),map(w*h*4);
    std::vector<unsigned char> coverage(w*h,255);
    for(UINT i=0;i<w*h;++i) {markers[2*i]=0;markers[2*i+1]=.01f;}
    const UINT center=8*w+8;
    markers[2*center]=-3; // actual pool slot zero, foreign camera
    map[4*center]=1.75f;map[4*center+1]=-.5f;
    map[4*center+2]=.0025f;map[4*center+3]=1;
    auto color=texture(device,w,h,DXGI_FORMAT_R11G11B10_FLOAT,D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET,rgb.data(),w*4);
    auto depth=texture(device,w,h,DXGI_FORMAT_R32_FLOAT,D3D11_BIND_SHADER_RESOURCE,depths.data(),w*4);
    auto slots=texture(device,w,h,DXGI_FORMAT_R32G32_FLOAT,D3D11_BIND_SHADER_RESOURCE,markers.data(),w*8);
    auto motion=texture(device,w,h,DXGI_FORMAT_R32G32B32A32_FLOAT,D3D11_BIND_SHADER_RESOURCE,map.data(),w*16);
    auto mask=texture(device,w,h,DXGI_FORMAT_R8_UNORM,D3D11_BIND_SHADER_RESOURCE,coverage.data(),w);
    auto colorView=view(device,color.Get()),depthView=view(device,depth.Get()),slotsView=view(device,slots.Get());
    auto motionView=view(device,motion.Get()),maskView=view(device,mask.Get());
    FlatMonoResolveFrame f{};f.color=colorView.Get();f.depth=depthView.Get();
    f.renderWidth=f.outputWidth=w;f.renderHeight=f.outputHeight=h;
    camera(f.camera);camera(f.previousCamera);fixture.engine(f);f.engine.slots=slotsView.Get();
    f.hdr=true;f.untrustedCameraCoverage=maskView.Get();
    f.foregroundMotion=motionView.Get();f.foregroundQualified=true;
    const auto resolve=[&](bool expected,const char* what) {
        fixture.bindOriginal();ComPtr<ID3D11ShaderResourceView> output;const char* why=nullptr;
        const bool ok=flatMonoResolve(device,context,f,&output,&why);
        check(ok==expected,what);check(fixture.restored(),"SDK foreground restores game pipeline");
        return ok;
    };
    expectedJx=expectedJy=0;
    for(auto mode:{FlatMonoResolveMode::Dlaa,FlatMonoResolveMode::Dlss,FlatMonoResolveMode::Fsr}) {
        // A holstered frame still requires the planner's complete ownership
        // certificate, even though its actual H foreground map is empty.
        const auto mixedMap=map;
        markers[2*center]=0;std::fill(map.begin(),map.end(),0);
        context->UpdateSubresource(slots.Get(),0,nullptr,markers.data(),w*8,0);
        context->UpdateSubresource(motion.Get(),0,nullptr,map.data(),w*16,0);
        flatMonoResolveReset();f.mode=mode;f.frame=69000+UINT(mode)*10;
        f.foregroundFrame=f.frame;f.foregroundRequired=true;f.foregroundQualified=true;
        f.untrustedCameraCoverage=nullptr;f.reset=true;
        resolve(true,"configured SDK accepts required certified world-only frame");
        ++f.frame;f.foregroundFrame=f.frame;f.reset=false;
        const int worldOnlyCalls=backendCalls;
        resolve(true,"configured SDK continues required empty foreground map");
        check(backendCalls==worldOnlyCalls+1 && !backendReset && observedDepth==.01f &&
              observedMotion==0 && observedMotionY==0 && observedReject==0,
              "holstered world-only frame reaches configured SDK with sane world inputs and no TAA");
        map=mixedMap;markers[2*center]=-3;
        context->UpdateSubresource(slots.Get(),0,nullptr,markers.data(),w*8,0);
        context->UpdateSubresource(motion.Get(),0,nullptr,map.data(),w*16,0);
        f.foregroundRequired=false;f.untrustedCameraCoverage=maskView.Get();
        flatMonoResolveReset();f.mode=mode;f.frame=70000+UINT(mode)*10;f.foregroundFrame=f.frame;f.reset=true;
        resolve(true,"SDK foreground reset contract accepted");
        ++f.frame;f.foregroundFrame=f.frame;f.reset=false;
        const int before=backendCalls;
        resolve(true,"configured SDK receives qualified mixed-camera frame");
        check(backendCalls==before+1 && !backendReset && observedMotion==1.75f && observedMotionY==-.5f &&
              observedDepth==.0025f && observedReject==0,
              "all SDK modes receive real foreground motion and canonical depth without TAA");
        map[4*center]=map[4*center+1]=0;map[4*center+3]=2;
        context->UpdateSubresource(motion.Get(),0,nullptr,map.data(),w*16,0);
        ++f.frame;f.foregroundFrame=f.frame;const int ambiguousCalls=backendCalls;
        resolve(true,"ambiguous foreground history keeps configured SDK engaged");
        check(backendCalls==ambiguousCalls+1 && !backendReset && observedDepth==.0025f &&
              observedMotion==0 && observedMotionY==0 && observedReject==255,
              "GPU ambiguity rejects only history and retains canonical depth without whole-frame reset or TAA");
        map[4*center]=1.75f;map[4*center+1]=-.5f;map[4*center+3]=1;
        context->UpdateSubresource(motion.Get(),0,nullptr,map.data(),w*16,0);
        // Conservative footprints can include world overdraw. Final ownership
        // controls SDK prep: identical-depth world marker defeats old motion.
        markers[2*center]=0;context->UpdateSubresource(slots.Get(),0,nullptr,markers.data(),w*8,0);
        ++f.frame;f.foregroundFrame=f.frame;
        resolve(true,"SDK final world overdraw remains admitted");
        check(observedDepth==.01f && observedMotion==0 && observedMotionY==0 && observedReject==0,
              "equal-depth world overdraw uses world inputs despite conservative foreign footprint");
        // Section 104: world draws leave no mark, so a first-person mark names its pixel only while its depth is still the pixel's raw
        // depth. A world surface drawn over it leaves the negative code behind with the old depth: the pixel takes the world path (the
        // engine lookup answers the camera term for a negative code), never the foreground map. The camera is moved so that term is
        // not zero and cannot be mistaken for a refusal's zero.
        f.previousCamera[5][0]=.05f;
        markers[2*center]=0;context->UpdateSubresource(slots.Get(),0,nullptr,markers.data(),w*8,0);
        ++f.frame;f.foregroundFrame=f.frame;
        resolve(true,"SDK moving camera with no mark resolves");
        const float cameraX=observedMotion,cameraY=observedMotionY,cameraDepth=observedDepth;
        check(std::isfinite(cameraX) && std::isfinite(cameraY) && (cameraX!=0 || cameraY!=0) && cameraDepth==.01f && observedReject==0,
              "reference: an unmarked pixel takes the camera term, which is not zero here");
        markers[2*center]=-3;markers[2*center+1]=.02f;   // a first-person mark whose depth is no longer the pixel's (.01)
        context->UpdateSubresource(slots.Get(),0,nullptr,markers.data(),w*8,0);
        ++f.frame;f.foregroundFrame=f.frame;
        resolve(true,"SDK stale first-person mark remains admitted");
        check(observedMotion==cameraX && observedMotionY==cameraY && observedDepth==cameraDepth && observedReject==0 &&
              !(observedMotion==1.75f && observedMotionY==-.5f) && observedDepth!=.0025f,
              "a first-person mark under another depth is stale: the pixel takes the camera term and raw depth, not the foreground map");
        // The same stale pixel under a map the foreground path would have refused (ambiguous history) is still the world's, not a refused
        // first-person pixel; with the depth equal, the identical map and mark are refused as first-person (kClassWeaponRefused).
        map[4*center+3]=2;context->UpdateSubresource(motion.Get(),0,nullptr,map.data(),w*16,0);
        f.refusalCensus=true;
        auto takeCensus=[&]() {   // the sample lands a few frames later; the bound is generous because the rig runs beside others
            refusalgpu::Taken taken;
            for(int i=0;i<3000 && taken.frames<1;++i) {
                context->Flush();taken.add(flatMonoResolveTakeRefusalCensus());
                if(taken.frames<1)Sleep(2);
            }
            return taken;
        };
        (void)flatMonoResolveTakeRefusalCensus();   // nothing has asked yet; start the sums from zero
        for(unsigned i=0;i<kFlatMonoRefusalEvery;++i) {   // four asking frames take exactly one sample
            ++f.frame;f.foregroundFrame=f.frame;
            resolve(true,"SDK stale first-person mark under an ambiguous map stays admitted");
            check(observedMotion==cameraX && observedMotionY==cameraY && observedDepth==cameraDepth && observedReject==0,
                  "a stale mark is not first-person even where the foreground map would refuse: world inputs, history kept");
        }
        const auto staleCensus=takeCensus();
        check(staleCensus.frames>=1 && staleCensus.counts[kFlatMonoClassWeaponRefused]==0 && staleCensus.refused()==0,
              "census: the stale mark's pixel is not counted as a refused first-person pixel");
        markers[2*center+1]=.01f;context->UpdateSubresource(slots.Get(),0,nullptr,markers.data(),w*8,0);
        for(unsigned i=0;i<kFlatMonoRefusalEvery;++i) {
            ++f.frame;f.foregroundFrame=f.frame;
            resolve(true,"SDK matching-depth first-person mark under an ambiguous map stays admitted");
            check(observedReject==255 && observedDepth==.0025f,
                  "control: the same mark at the pixel's own depth is first-person and its ambiguous history is refused");
        }
        const auto freshCensus=takeCensus();
        check(freshCensus.frames>=1 && freshCensus.counts[kFlatMonoClassWeaponRefused]==freshCensus.frames &&
              freshCensus.refused()==freshCensus.frames,
              "census: the matching-depth mark's pixel is counted once per sampled frame as a refused first-person pixel");
        f.refusalCensus=false;map[4*center+3]=1;
        context->UpdateSubresource(motion.Get(),0,nullptr,map.data(),w*16,0);
        camera(f.previousCamera);
        markers[2*center]=-3;context->UpdateSubresource(slots.Get(),0,nullptr,markers.data(),w*8,0);
        // A common near avoids depth >1 when the foreground camera can see
        // nearer geometry. World reprojection still consumes the raw depth.
        f.foregroundDepthNear=expectedNear=.0125f;
        map[4*center+2]=.00125f;context->UpdateSubresource(motion.Get(),0,nullptr,map.data(),w*16,0);
        ++f.frame;f.foregroundFrame=f.frame;
        resolve(true,"SDK common depth convention transition resolves");
        check(backendReset && observedDepth==.00125f,"SDK depth convention change resets once");
        ++f.frame;f.foregroundFrame=f.frame;
        resolve(true,"SDK common depth convention continues");
        check(!backendReset && observedDepth==.00125f && observedMotion==1.75f,
              "stable common near keeps real motion and temporal continuity");
        markers[2*center]=0;context->UpdateSubresource(slots.Get(),0,nullptr,markers.data(),w*8,0);
        ++f.frame;f.foregroundFrame=f.frame;
        resolve(true,"world under common SDK depth convention resolves");
        check(observedDepth==.005f && observedMotion==0,"world depth is scaled after raw world reprojection");
        markers[2*center]=-3;context->UpdateSubresource(slots.Get(),0,nullptr,markers.data(),w*8,0);
        map[4*center]=24.f;context->UpdateSubresource(motion.Get(),0,nullptr,map.data(),w*16,0);
        ++f.frame;f.foregroundFrame=f.frame;
        resolve(true,"actual offscreen foreground motion reaches SDK");
        check(observedMotion==24.f && observedReject==255 && !backendReset,
              "real disocclusion retains actual motion while finish takes current color");
        f.foregroundDepthNear=0;expectedNear=.025f;map[4*center]=1.75f;map[4*center+2]=.0025f;
        context->UpdateSubresource(motion.Get(),0,nullptr,map.data(),w*16,0);
        auto refuse=[&](const char* what) {
            const int calls=backendCalls;std::vector<unsigned char> original,after;
            check(readWhole(context,color.Get(),original,4),"SDK refusal original H readable");
            resolve(false,what);
            check(readWhole(context,color.Get(),after,4) && original==after && backendCalls==calls,
                  "unqualified foreground refuses before backend or H mutation");
        };
        ++f.frame;f.foregroundFrame=f.frame-1;refuse("stale foreground frame refuses");
        f.foregroundFrame=f.frame;f.foregroundQualified=false;refuse("missing final ownership qualification refuses");
        f.foregroundQualified=true;
        f.foregroundRequired=true;f.foregroundMotion=nullptr;
        f.untrustedCameraCoverage=nullptr;refuse("required foreground contract refuses even without conservative coverage");
        f.foregroundMotion=motionView.Get();f.untrustedCameraCoverage=maskView.Get();f.foregroundRequired=false;
        auto wrong=texture(device,w,h,DXGI_FORMAT_R16G16B16A16_FLOAT,D3D11_BIND_SHADER_RESOURCE);
        auto wrongView=view(device,wrong.Get());f.foregroundMotion=wrongView.Get();refuse("rounded foreground depth format refuses");
        f.foregroundMotion=motionView.Get();
    }
    // TAA never consumes this SDK-only map or its ownership certificate.
    flatMonoResolveReset();f.mode=FlatMonoResolveMode::Taa;f.frame=71000;f.reset=true;
    f.foregroundQualified=false;f.foregroundFrame=0;
    const int calls=backendCalls;resolve(true,"native TAA retains existing conservative coverage route");
    check(backendCalls==calls,"TAA route does not call an SDK");
    flatMonoResolveReset();context->ClearState();
}

// The scenario on the shipped prep, then on the prep with one section 104 rule flipped at a time: the stale-mark checks must catch
// each flip (a scenario that cannot fail proves nothing), and the unmutated source compiled here must pass through the same seam.
inline void sdkForegroundGpuTests(ID3D11Device* device, ID3D11DeviceContext* context) {
    using namespace edvr;
    sdkForegroundScenario(device,context);
    struct Mutant {const char* name;const char* from;const char* to;};
    static const Mutant mutants[]={
        {"a negative mark is first person at any depth (the rule before section 104)",
         "foreground=owner.x < -1 && asuint(owner.y)==asuint(depth);","foreground=owner.x < -1;"},
        {"a mark names its pixel only where its depth differs",
         "foreground=owner.x < -1 && asuint(owner.y)==asuint(depth);","foreground=owner.x < -1 && asuint(owner.y)!=asuint(depth);"},
    };
    const std::string shipped=kFlatMonoShaderSource;
    auto runMutated=[&](const std::string& hlsl,int* failed,std::string* first) {
        std::vector<unsigned char> bytes;
        if(!fpgpu::compilePrep(hlsl,bytes))return false;
        ComPtr<ID3D11ComputeShader> probe;   // the bytes must make a compute shader, or "the scenario fails" could mean the resolver never started
        if(FAILED(device->CreateComputeShader(bytes.data(),bytes.size(),nullptr,probe.GetAddressOf())))return false;
        flatMonoResolveTestPrepBytecode(bytes.data(),bytes.size());flatMonoResolveReset();
        *failed=0;mutationFailures=failed;mutationFirst.clear();
        sdkForegroundScenario(device,context);
        mutationFailures=nullptr;if(first)*first=mutationFirst;
        return true;
    };
    {   // The control: the same source, compiled here and run through the same seam, is the shipped prep and passes.
        int failed=0;std::string first;
        const bool ran=runMutated(shipped,&failed,&first);
        check(ran && failed==0,"SDK foreground mutations: control: the unmutated prep, compiled here and run through the test seam, passes the whole scenario");
    }
    for(const Mutant& m:mutants) {
        bool once=false;
        const std::string hlsl=fpgpu::replaceOnce(shipped,m.from,m.to,&once);
        char what[320];
        std::snprintf(what,sizeof(what),"SDK foreground mutations: \"%s\": its anchor is in the shader source exactly once",m.name);
        check(once,what);
        int failed=0;std::string first;
        const bool ran=once && runMutated(hlsl,&failed,&first);
        std::snprintf(what,sizeof(what),"SDK foreground mutations: \"%s\" compiles and makes a compute shader",m.name);
        check(ran,what);
        if(!ran)continue;
        std::snprintf(what,sizeof(what),"SDK foreground mutations: \"%s\" is caught by the scenario",m.name);
        check(failed>0,what);
        std::printf("flat mono resolve: SDK foreground mutation \"%s\": %d checks fail; first: %s\n",m.name,failed,first.c_str());
    }
    flatMonoResolveTestPrepBytecode(nullptr,0);flatMonoResolveReset();
}
