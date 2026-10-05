// Exercise the shipped SDK prep and resolver contract through backend stubs.
// The adapter's marker/history production has separate tests; these cases pin
// the actual inputs handed to each backend and refusals before any H mutation.
#pragma once

inline void sdkForegroundGpuTests(ID3D11Device* device, ID3D11DeviceContext* context) {
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
        // Conservative footprints can include world overdraw. Final ownership
        // controls SDK prep: identical-depth world marker defeats old motion.
        markers[2*center]=0;context->UpdateSubresource(slots.Get(),0,nullptr,markers.data(),w*8,0);
        ++f.frame;f.foregroundFrame=f.frame;
        resolve(true,"SDK final world overdraw remains admitted");
        check(observedDepth==.01f && observedMotion==0 && observedMotionY==0 && observedReject==0,
              "equal-depth world overdraw uses world inputs despite conservative foreign footprint");
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
