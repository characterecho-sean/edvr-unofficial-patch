#pragma once
#include "../../src/d3d11/flat_origin_optical.h"
#include "../../src/d3d11/flat_world_witness.h"
#include "../../src/common/runtime_profile.h"

// A narrow WARP fixture for the *passive* sidecar. The current rows move
// 80 -> 20 m in both cases. Only the prior depth changes: a true coordinate
// reanchor leaves a fixed world pattern in place, whereas inward travel moves
// that pattern by the raw camera projection. The renderer still receives the
// same source buffers it would without the sidecar.
inline int originOpticalGpuTests(ID3D11Device* device,ID3D11DeviceContext* context) {
    using namespace edvr;
    check(!flatOriginCaptureEnabled(), "origin optical: capture defaults off in production resolver rig");
    const size_t beforeOff=opticalLines.size();
    originAmbiguityGpuTests(device,context,RuntimeProfile::Flat);
    flatMonoResolvePollPixels(context,1);
    check(opticalLines.size()==beforeOff, "origin optical: default-off reset/poll paths produce no probe output");
    check(flatOriginOpticalCategory(60.f,20,80,false)==0,
        "origin optical: 80 to 20 m inward travel has reserved short-inward quota");
    unsigned spent[5]{};
    for(unsigned n=0;n<120;++n){const unsigned c=flatOriginOpticalCategory(5000.f,20,5020,false);if(spent[c]<6)++spent[c];}
    check(spent[0]==0 && spent[1]==6 && flatOriginOpticalCategory(60.f,20,80,false)==0,
        "origin optical: long periodic reanchors cannot spend inward-travel quota");
    check(!flatOriginOpticalPriorDomainSupported(true,false) && !flatOriginOpticalPriorDomainSupported(false,true) &&
          !flatOriginOpticalPriorDomainSupported(false,false,true) &&
          flatOriginOpticalPriorDomainSupported(false,false),
        "origin optical: foreground and mixed-camera previous depth refuse a score without prior ownership");
    {uint32_t score[40]{};score[7]=100;score[12]=90;
     check(std::strcmp(flatOriginOpticalSignal(score),"inconclusive")==0,
        "origin optical: a flat-depth tie is explicitly inconclusive");
     score[13]=20;score[14]=0;check(std::strcmp(flatOriginOpticalSignal(score),"raw-travel-depth-favored")==0,
        "origin optical: only shared-support exclusive depth fits can favor raw translation");
     score[13]=0;score[14]=20;check(std::strcmp(flatOriginOpticalSignal(score),"zero-translation-depth-favored")==0,
        "origin optical: only shared-support exclusive depth fits can favor zero translation");}

    const RuntimeProfile previousProfile=runtimeProfileValue();
    struct Outcome {std::vector<unsigned char> mask,output;std::vector<float> motion;std::string input,signal;bool score=false,reset=false;};
    auto run=[&](bool reanchor,bool probe,bool gap=false,bool steady=true,bool foreground=false,bool priorForeground=false)->Outcome {
        flatMonoResolveReset();
        runtimeProfileSet(probe?RuntimeProfile::Flat:RuntimeProfile::LegacyVr);
        steadygpu::Rig r(device,context);
        r.start(FlatMonoResolveMode::Dlss);
        r.f.steadyDetail=steady;
        r.f.camera[0][0]=r.f.previousCamera[0][0]=1.f/64.f;
        r.f.camera[5][0]=r.f.previousCamera[5][0]=80.f;
        ComPtr<ID3D11Texture2D> foregroundMap;
        ComPtr<ID3D11ShaderResourceView> foregroundView;
        if(foreground || priorForeground) {
            std::vector<float> emptyMap(steadygpu::W*steadygpu::H*4,0.f);
            foregroundMap=texture(device,steadygpu::W,steadygpu::H,DXGI_FORMAT_R32G32B32A32_FLOAT,
                D3D11_BIND_SHADER_RESOURCE,emptyMap.data(),steadygpu::W*16);
            foregroundView=view(device,foregroundMap.Get());
        }
        if(priorForeground) {
            r.f.foregroundMotion=foregroundView.Get();r.f.foregroundQualified=true;r.f.foregroundFrame=r.f.frame;
        }
        r.fillZ(.01f);
        r.setZ(reanchor?6:0,0,10,steadygpu::H,.02f);
        r.run("origin optical: adjacent baseline");
        r.f.reset=false;
        if(priorForeground){r.f.foregroundMotion=nullptr;r.f.foregroundQualified=false;}
        if(gap)++r.f.frame;
        r.f.previousCamera[5][0]=80.f;r.f.camera[5][0]=20.f;
        r.fillZ(.01f);r.setZ(6,0,10,steadygpu::H,.02f);
        const uint64_t eventFrame=r.f.frame;
        // Header-only CB readback must retain every component, not just the
        // first/last stores. Keep the scene rows deliberately unlike the
        // resolver rows to prove which bound buffers the sidecar reads.
        float sceneNow[277][4]{},scenePrev[277][4]{};
        std::memcpy(sceneNow+270,r.f.camera,sizeof(r.f.camera));
        std::memcpy(scenePrev+270,r.f.previousCamera,sizeof(r.f.previousCamera));
        sceneNow[275][0]=123.25f;sceneNow[275][1]=-45.5f;sceneNow[275][2]=678.125f;
        scenePrev[275][0]=-912.5f;scenePrev[275][1]=34.25f;scenePrev[275][2]=-56.75f;
        constexpr uint32_t nowStamp=0x13579BDFu,prevStamp=0x2468ACE0u;
        std::memcpy(&sceneNow[276][0],&nowStamp,4);std::memcpy(&scenePrev[276][0],&prevStamp,4);
        D3D11_BUFFER_DESC sceneDesc{};sceneDesc.ByteWidth=sizeof(sceneNow);
        sceneDesc.Usage=D3D11_USAGE_DEFAULT;sceneDesc.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
        D3D11_SUBRESOURCE_DATA sceneInit{};ComPtr<ID3D11Buffer> boundNow,boundPrev;
        sceneInit.pSysMem=sceneNow;
        check(SUCCEEDED(device->CreateBuffer(&sceneDesc,&sceneInit,boundNow.GetAddressOf())),
            "origin optical: distinct scene-now constant buffer created");
        sceneInit.pSysMem=scenePrev;
        check(SUCCEEDED(device->CreateBuffer(&sceneDesc,&sceneInit,boundPrev.GetAddressOf())),
            "origin optical: distinct scene-prev constant buffer created");
        r.f.engine.sceneNow=boundNow.Get();r.f.engine.scenePrev=boundPrev.Get();
        if(foreground) {
            r.f.foregroundMotion=foregroundView.Get();r.f.foregroundQualified=true;r.f.foregroundFrame=eventFrame;
        }
        const size_t firstLine=opticalLines.size();
        ComPtr<ID3D11ShaderResourceView> output;
        steadygpu::Seen event=r.run("origin optical: 60 m inward candidate",true,&output);
        Outcome o{};o.reset=event.reset;o.mask=std::move(event.mask);o.motion=std::move(event.motion);
        if(output){ComPtr<ID3D11Resource> resource;ComPtr<ID3D11Texture2D> image;
            output->GetResource(resource.GetAddressOf());if(resource&&SUCCEEDED(resource.As(&image)))
                check(readWhole(context,image.Get(),o.output,4),"origin optical: final output readable");}
        r.f.previousCamera[5][0]=r.f.camera[5][0];
        for(unsigned attempt=0;attempt<12;++attempt) {
            for(size_t i=firstLine;i<opticalLines.size();++i) {
                const std::string marker="frame="+std::to_string(eventFrame)+" ";
                if(opticalLines[i].find("flat origin optical input:")==0 && opticalLines[i].find(marker)!=std::string::npos)
                    o.input=opticalLines[i];
                if(opticalLines[i].find("flat origin optical score:")==0 && opticalLines[i].find(marker)!=std::string::npos) {
                    o.score=true;o.signal=opticalLines[i];break;
                }
            }
            if(o.score || !flatOriginCaptureEnabled() || !probe || gap || !steady || foreground || priorForeground)break;
            context->Flush();Sleep(2);
            r.run("origin optical: readback drain frame");
        }
        if(probe && (gap || !steady || foreground || priorForeground)) {
            bool unavailable=false;
            for(size_t i=firstLine;i<opticalLines.size();++i)
                unavailable |= opticalLines[i].find(priorForeground?"unavailable=prior-domain-unverified-previous-foreground-or-untrusted":
                    foreground?"unavailable=prior-domain-unverified-foreground-or-untrusted":
                    "unavailable=no-adjacent-previous-depth")!=std::string::npos;
            check(unavailable,priorForeground?"origin optical: prior SDK foreground excludes current world-only score":
                foreground?"origin optical: qualified SDK foreground input refuses unverified prior domain":
                "origin optical: gap or missing depth names no adjacent previous depth");
        }
        if(probe && o.score) {
            check(o.input.find("scene-now=(123.25,-45.5,678.125) scene-prev=(-912.5,34.25,-56.75)")!=std::string::npos &&
                  o.input.find("stamps="+std::to_string(nowStamp)+"/"+std::to_string(prevStamp))!=std::string::npos,
                "origin optical: all six raw scene-origin components and both stamps survive GPU readback");
        }
        return o;
    };

    const size_t beforeDisabled=opticalLines.size();
    const Outcome captureOff=run(true,true); // Flat profile, actual startup default remains off
    check(captureOff.reset && !captureOff.score && opticalLines.size()==beforeDisabled,
        "origin optical: 80 to 20 still resets with all F1 captures off");
    flatOriginCaptureInitialize(true); // explicit startup opt-in for the remaining probe fixtures
    check(flatOriginCaptureEnabled(), "origin optical: explicit opt-in reaches enabled probe");

    // Present-frame polling is the production drain/heartbeat path. Run its
    // bounded idle start far above the fixture's resolver frame numbers, then
    // prove a later scored event gets a fresh aggregate despite that cap.
    auto heartbeatCount=[&](){size_t n=0;for(const auto& line:opticalLines)
        if(line.find("flat origin optical heartbeat:")==0)++n;return n;};
    uint64_t heartbeatPresent=uint64_t(1)<<40;
    const size_t heartbeatBefore=heartbeatCount();
    for(unsigned n=0;n<16;++n,heartbeatPresent+=2700)flatMonoResolvePollPixels(context,heartbeatPresent);
    const size_t heartbeatIdle=heartbeatCount();
    flatMonoResolvePollPixels(context,heartbeatPresent);heartbeatPresent+=2700;
    check(heartbeatCount()==heartbeatIdle && heartbeatIdle<=heartbeatBefore+12,
        "origin optical: unchanged present polling stops after bounded startup heartbeats");

    const Outcome inward=run(false,true);
    flatMonoResolvePollPixels(context,heartbeatPresent);heartbeatPresent+=2700;
    const size_t heartbeatAfterEvent=heartbeatCount();
    check(inward.score && heartbeatAfterEvent==heartbeatIdle+1 &&
          !opticalLines.empty() && opticalLines.back().find("flat origin optical heartbeat:")==0 &&
          opticalLines.back().find("no-event=0")!=std::string::npos,
        "origin optical: scored event reopens aggregate after idle heartbeat cap");
    flatMonoResolvePollPixels(context,heartbeatPresent);heartbeatPresent+=2700;
    check(heartbeatCount()==heartbeatAfterEvent,
        "origin optical: unchanged aggregate stays quiet after the post-event report");
    const Outcome inwardNoProbe=run(false,false);
    if(!inward.score || inward.signal.find("signal=raw-travel-depth-favored")==std::string::npos)
        std::printf("origin optical inward diagnostic: score=%u %s\n",inward.score?1u:0u,inward.signal.c_str());
    check(inward.score && inward.signal.find("signal=raw-travel-depth-favored")!=std::string::npos,
        "origin optical: 80 to 20 m inward travel favors unmodified raw translation on shared static depth samples");
    check(inward.input.find("planned-reset=1 origin-ambiguous=1")!=std::string::npos,
        "origin optical: the passive input comparison stays alive on a planned ambiguity reset");
    check(inward.mask==inwardNoProbe.mask && inward.motion==inwardNoProbe.motion && inward.output==inwardNoProbe.output,
        "origin optical: passive sidecar changes neither SDK inputs nor final color");
    const Outcome reanchor=run(true,true),reanchorNoProbe=run(true,false);
    check(reanchor.reset && captureOff.mask==reanchor.mask && captureOff.motion==reanchor.motion &&
          captureOff.output==reanchor.output,
        "origin optical: startup off/on produce identical reset, SDK inputs and displayed color");
    check(reanchor.score && reanchor.signal.find("signal=zero-translation-depth-favored")!=std::string::npos,
        "origin optical: same raw camera jump with reexpressed world favors zero translation");
    check(reanchor.input.find("planned-reset=1 origin-ambiguous=1")!=std::string::npos,
        "origin optical: identical camera inputs also report reset when depth favors a coordinate change");
    check(reanchor.mask==reanchorNoProbe.mask && reanchor.motion==reanchorNoProbe.motion && reanchor.output==reanchorNoProbe.output,
        "origin optical: reanchor probe leaves motion, mask, and final color unchanged");
    run(false,true,true);  // frame gap, previous depth no longer adjacent
    run(false,true,false,false); // steady depth disabled, no prior image
    run(false,true,false,true,true); // bound SDK foreground: prior image has no ownership mask
    run(false,true,false,true,false,true); // same depth scale: previous foreground, current world-only
    // Exercise the production resolver's world-event hook and its early-return
    // RAII path. No game store callbacks are fabricated: absent cohorts must
    // remain explicitly inconclusive even when the clocks match.
    auto worldWire=[&](bool invalidSlot) {
        flatMonoResolveReset();
        runtimeProfileSet(RuntimeProfile::Flat);
        steadygpu::Rig r(device,context);
        r.start(FlatMonoResolveMode::Dlss);
        r.f.steadyDetail=true;
        r.f.camera[5][0]=r.f.previousCamera[5][0]=80.f;
        flatWorldWitnessFrameBoundary(static_cast<uint32_t>(r.f.frame),true,false,false);
        r.run("origin world wiring: baseline");
        r.f.reset=false;r.f.previousCamera[5][0]=80.f;r.f.camera[5][0]=20.f;
        if(invalidSlot)r.f.slot=999;
        const uint64_t candidate=r.f.frame;
        flatWorldWitnessFrameBoundary(static_cast<uint32_t>(candidate),true,false,false);
        const size_t first=worldLines.size();
        r.run(invalidSlot?"origin world wiring: invalid slot candidate":
            "origin world wiring: admitted candidate",!invalidSlot);
        for(uint32_t frame=static_cast<uint32_t>(candidate)+1;frame<=candidate+3;++frame)
            flatWorldWitnessFrameBoundary(frame,true,false,false);
        std::string event,coverage;
        const std::string marker="resolver-frame="+std::to_string(candidate)+" ";
        for(size_t i=first;i<worldLines.size();++i) {
            if(worldLines[i].find("flat world witness: resolver-frame=")==0&&
               worldLines[i].find(marker)!=std::string::npos)event=worldLines[i];
            if(worldLines[i].find("flat world witness coverage:")==0&&
               worldLines[i].find(marker)!=std::string::npos)coverage=worldLines[i];
        }
        check(!event.empty()&&event.find("engine-at-event="+std::to_string(candidate))!=std::string::npos&&
              event.find("clocks-match=1 complete=1")!=std::string::npos,
              "origin world wiring: resolver candidate reaches completed same-clock witness");
        check(event.find(invalidSlot?"unavailable=1 reason=flat-resolve-invalid-slot":
              "unavailable=0 reason=")!=std::string::npos,
              invalidSlot?"origin world wiring: RAII early exit names invalid slot":
                  "origin world wiring: admitted candidate reaches event API");
        check(event.find("world-edge-valid=0")!=std::string::npos&&
              coverage.find("observed=0,0,0,0,0,0")!=std::string::npos,
              "origin world wiring: no store observations remain an explicit no-cohort result");
        flatWorldWitnessShutdown();
    };
    worldWire(false);
    worldWire(true);
    runtimeProfileSet(previousProfile);
    flatMonoResolveReset();
    return 0;
}
