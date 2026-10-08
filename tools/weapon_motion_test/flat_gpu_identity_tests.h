#pragma once
#include <limits>

// Actual GPU identity snapshots are the oracle. Deliberately omit every CPU
// row and input certificate, including slots first seen after publication.
//
// The map matches a draw to the previous frame's draw of the same geometry BY CONTENT: the pool record's identity (word 0, and the word at byte
// 28 without byte 30), never its slot (flat_foreground_motion_shader.h). The pool has two rows here, slot 0 (skeleton 92) and slot 1 (skeleton
// 740), the same allocation word in both; a test that needs two slots with one identity gives row 1 row 0's words (or the other way round) and
// puts them back. Every rejected sample says why in x: 1 invalid current, 2 not authentic, 3 no prior, 4 prior positions invalid, 5 identity
// differs, 6 prior identity invalid, 7 ambiguous.
template<class Pose,class Raster,class ReadMap>
void flatGpuIdentityTests(ID3D11DeviceContext* ctx,ID3D11ShaderResourceView* owners,
                         ID3D11ShaderResourceView* depth,unsigned width,unsigned height,
                         edvr::FlatForegroundMotion::Inputs inputs,const float world[6][4],
                         Pose oldPose,Pose nowPose,Raster raster,ReadMap readMap,
                         ID3D11Buffer* pool,unsigned (&poolData)[168]) {
    using edvr::FlatForegroundMotion;
    // The base map's contract: reasons 3, 5 and 6 for a draw with no history (the sibling pass, flat_foreground_sibling.h, replaces them).
    FlatForegroundMotion::RigSiblingPass baseMap(false);
    inputs.gpuIdentity=true;inputs.identity={};
    inputs.identity.refusal="identity-pool-slot-unobserved";
    inputs.certificate={};inputs.phaseX=inputs.phaseY=0;
    auto capture=[&](FlatForegroundMotion& motion,Pose pose,unsigned slot,unsigned frame,unsigned writer=1,unsigned flags=0) {
        raster(pose,slot,writer,flags);inputs.writerToken=writer;
        check(motion.capture(ctx,issue,9,1,0,0,0,frame,inputs),"GPU identities capture without CPU row or certificate");
    };
    // The map's samples of the class `expectedClass` must be more than 2000 pixels, each with the actual canonical depth; with a reason (a
    // rejection), every one of them names it in x, and with no reason the samples are not asked to.
    auto output=[&](FlatForegroundMotion& motion,unsigned frame,float expectedClass,const char* what,bool localOnly=false,bool conventionReset=false,int reason=-1) {
        FlatForegroundMotion::Output result;
        ID3D11ShaderResourceView* beforeViews[15]{};ctx->VSGetShaderResources(0,15,beforeViews);
        Microsoft::WRL::ComPtr<ID3D11DeviceContext1> context1;ctx->QueryInterface(IID_PPV_ARGS(&context1));
        Microsoft::WRL::ComPtr<ID3D11Buffer> beforeVsCb,beforePsCb;
        UINT beforeVsFirst=0,beforeVsCount=0,beforePsFirst=0,beforePsCount=0;
        if(context1){context1->VSGetConstantBuffers1(0,1,&beforeVsCb,&beforeVsFirst,&beforeVsCount);
            context1->PSGetConstantBuffers1(0,1,&beforePsCb,&beforePsFirst,&beforePsCount);}
        check(motion.prepareH(ctx,owners,depth,world,frame,width,height,result) && result.qualified,
              "actual GPU identity H retains configured SDK contract");
        if(localOnly)check(!result.resetRequired,"new GPU geometry rejects its own history without resetting a continuous world");
        if(conventionReset)check(result.resetRequired,"actual canonical-depth convention transition still resets SDK history");
        ID3D11ShaderResourceView* afterViews[15]{};ctx->VSGetShaderResources(0,15,afterViews);
        for(unsigned i=0;i<15;++i){check(beforeViews[i]==afterViews[i],"GPU H restores all fifteen VS SRVs");
            if(beforeViews[i])beforeViews[i]->Release();if(afterViews[i])afterViews[i]->Release();}
        if(context1){Microsoft::WRL::ComPtr<ID3D11Buffer> vsCb,psCb;UINT vsFirst=0,vsCount=0,psFirst=0,psCount=0;
            context1->VSGetConstantBuffers1(0,1,&vsCb,&vsFirst,&vsCount);context1->PSGetConstantBuffers1(0,1,&psCb,&psFirst,&psCount);
            check(vsCb==beforeVsCb && vsFirst==beforeVsFirst && vsCount==beforeVsCount &&
                  psCb==beforePsCb && psFirst==beforePsFirst && psCount==beforePsCount,
                  "GPU H restores exact Context1 VS and PS constant-buffer ranges");}
        const auto pixels=readMap(result.motion.Get());unsigned matching=0,unnamed=0;
        for(unsigned p=0;p<width*height;++p)if(pixels[4*p+3]==expectedClass) {
            ++matching;check(pixels[4*p+2]>0 && pixels[4*p+2]<=1,
                             "matched or rejected GPU history retains actual canonical depth");
            if(reason>=0 && pixels[4*p]!=float(reason))++unnamed;
        }
        check(matching>2000,what);
        if(reason>=0) {
            const std::string named=std::string(what)+": every rejected sample names its reason, "+std::to_string(reason);
            check(unnamed==0,named.c_str());
        }
        return pixels;
    };
    // A class-1 sample's motion is the real previous original-VS output against this one: x in pixels is
    // (previous mouse - this mouse) * width / 2 * canonical depth / clip, y is zero (the pose moves in x only).
    auto motionIsReal=[&](const std::vector<float>& pixels,Pose before,Pose after,const char* what) {
        unsigned seen=0;bool exact=true;
        for(unsigned p=0;p<width*height;++p)if(pixels[4*p+3]==1) {
            ++seen;
            exact=exact && std::fabs(pixels[4*p]-(before.mouse-after.mouse)*width*.5f*pixels[4*p+2]/before.clip)<1.f/256 &&
                   std::fabs(pixels[4*p+1])<1.f/256;
        }
        check(seen>2000 && exact,what);
    };
    // The two pool rows hold one identity while the test needs two slots to share it: row 1 takes row 0's two identity words. Put back after.
    const unsigned rowOneWords[2]={poolData[84],poolData[91]};
    const auto shareIdentity=[&] {poolData[84]=poolData[0];poolData[91]=poolData[7];ctx->UpdateSubresource(pool,0,nullptr,poolData,0,0);};
    const auto restoreRows=[&] {poolData[84]=rowOneWords[0];poolData[91]=rowOneWords[1];ctx->UpdateSubresource(pool,0,nullptr,poolData,0,0);};
    {
        FlatForegroundMotion motion;capture(motion,oldPose,1,1000);output(motion,1000,2,"new GPU identity rejects old history locally",false,false,3);
        check(motion.stats().noCandidate==1 && motion.stats().priorsOne==0 && motion.stats().noPriorPool==0 &&
                  motion.stats().noPriorNear==0 && motion.stats().noPriorAbsent==0,
              "a draw whose geometry the frame before did not draw has no candidate");
        capture(motion,nowPose,1,1001);auto pixels=output(motion,1001,1,"steady GPU identity selects actual previous positions");
        motionIsReal(pixels,oldPose,nowPose,"GPU identity motion matches real previous original-VS output");
        check(motion.stats().noCandidate==1 && motion.stats().priorsOne==1 && motion.stats().priorsSeveral==0 && motion.stats().repeated==0,
              "the steady draw has one prior, supplied by the adapter, and is the first of its geometry in its frame");
    }
    {
        // THE SLOT IS NOT AN IDENTITY. Elite re-orders the pool every live frame, so the same record sits at a new slot most frames: a repacked
        // slot with the same geometry and identity keeps its history, and its motion is the real previous original-VS output.
        FlatForegroundMotion motion;capture(motion,oldPose,1,1100);
        // The record that was at slot 1 (skeleton 740) is at slot 0 now.
        const unsigned saved=poolData[0];poolData[0]=poolData[84];ctx->UpdateSubresource(pool,0,nullptr,poolData,0,0);
        capture(motion,nowPose,0,1101);
        auto pixels=output(motion,1101,1,"a repacked slot with the same geometry and identity keeps history");
        motionIsReal(pixels,oldPose,nowPose,"a repacked slot's motion equals the real previous original-VS output");
        poolData[0]=saved;ctx->UpdateSubresource(pool,0,nullptr,poolData,0,0);
    }
    {
        // The field symptom: the slot moves every frame (here between the two rows, which share one identity) and the weapon keeps its history
        // on every one of them. With the slot compared, every frame after the first was rejected.
        shareIdentity();
        FlatForegroundMotion motion;Pose before=oldPose,now=oldPose;constexpr unsigned frames=8;
        capture(motion,before,0,1150);
        for(unsigned i=1;i<frames;++i) {
            now=oldPose;now.mouse=oldPose.mouse+.01f*i;
            capture(motion,now,i&1,1150+i);
            const std::string label="a slot permuted every frame keeps its history: frame "+std::to_string(i)+" of "+std::to_string(frames);
            auto pixels=output(motion,1150+i,1,label.c_str());
            motionIsReal(pixels,before,now,(label+", with the real previous motion").c_str());
            before=now;
        }
        check(motion.stats().priorsOne==frames-1 && motion.stats().noCandidate==1,
              "every permuted frame found its prior among the candidates and none was refused by the adapter");
        restoreRows();
    }
    {
        // A changed byte of the record's signature (28, 29 or 31) is another record: rejected, naming identity-differs. Byte 30, a per-instance
        // parameter that changes on 10 to 40 percent of rewritten records, is not part of the identity: accepted, with the real motion.
        const struct {unsigned shift;bool keeps;const char* byte;} bytes[]={{0,false,"28"},{8,false,"29"},{16,true,"30"},{24,false,"31"}};
        unsigned frame=1200;
        for(const auto& b:bytes) {
            FlatForegroundMotion motion;capture(motion,oldPose,1,frame);
            const unsigned saved=poolData[91];poolData[91]^=1u<<b.shift;ctx->UpdateSubresource(pool,0,nullptr,poolData,0,0);
            capture(motion,nowPose,1,frame+1);
            const std::string label=std::string("a changed GPU allocation byte ")+b.byte+(b.keeps?" (a per-instance parameter) keeps history":
                " (the record's signature) rejects stale history");
            auto pixels=output(motion,frame+1,b.keeps?1:2,label.c_str(),false,false,b.keeps?-1:5);
            if(b.keeps)motionIsReal(pixels,oldPose,nowPose,(label+", with the real previous motion").c_str());
            poolData[91]=saved;ctx->UpdateSubresource(pool,0,nullptr,poolData,0,0);
            frame+=10;
        }
        // Every bit of the record's first word is the identity, the bone base: another base is another record.
        FlatForegroundMotion motion;capture(motion,oldPose,1,frame);
        const unsigned saved=poolData[84];++poolData[84];ctx->UpdateSubresource(pool,0,nullptr,poolData,0,0);
        capture(motion,nowPose,1,frame+1);output(motion,frame+1,2,"a different bone base rejects stale history",false,false,5);
        poolData[84]=saved;ctx->UpdateSubresource(pool,0,nullptr,poolData,0,0);
    }
    {
        FlatForegroundMotion motion;capture(motion,oldPose,1,1300);capture(motion,oldPose,1,1300,2);
        check(motion.stats().repeated==1,"the second draw of one geometry in a frame counts as repeated");
        capture(motion,nowPose,1,1301);output(motion,1301,1,"identical duplicate GPU triangles coalesce without CPU certificates");
        check(motion.stats().priorsSeveral==1 && motion.stats().priorsOne==0,"a draw with two priors is counted as having several");
    }
    {
        FlatForegroundMotion motion;Pose different=oldPose;different.mouse+=.07f;
        capture(motion,oldPose,1,1400);capture(motion,different,1,1400,2);
        capture(motion,nowPose,1,1401);output(motion,1401,2,"different duplicate GPU poses reject history rather than choosing a pose",false,false,7);
    }
    {
        // Content cannot tell two draws of one geometry and one identity apart, and the slot no longer can: swapped slots with different poses
        // are ambiguous, not paired by their old slot.
        shareIdentity();
        FlatForegroundMotion motion;Pose different=oldPose;different.mouse+=.07f;Pose differentNow=nowPose;differentNow.mouse+=.07f;
        capture(motion,oldPose,0,1450);capture(motion,different,1,1450,2);
        capture(motion,differentNow,0,1451);capture(motion,nowPose,1,1451,2);
        output(motion,1451,2,"two draws of one geometry with swapped slots and different poses reject: no pose is chosen, no slot breaks the tie",false,false,7);
        restoreRows();
    }
    {
        FlatForegroundMotion motion;capture(motion,oldPose,1,1500);
        inputs.phaseX=.25f;capture(motion,oldPose,1,1500,2);inputs.phaseX=0;
        capture(motion,nowPose,1,1501);output(motion,1501,2,"different prior phases prevent duplicate GPU equivalence",false,false,7);
    }
    {
        FlatForegroundMotion motion;capture(motion,nowPose,2,1600);
        output(motion,1600,2,"invalid current GPU identity rejects history with actual depth",false,false,2);
    }
    {
        // An index with flag bits is not an authentic pool index even where its slot is a real row.
        FlatForegroundMotion motion;capture(motion,oldPose,1,1610);
        capture(motion,nowPose,1,1611,1,0x80000000u);
        output(motion,1611,2,"an instance index with flag bits is not authentic and rejects history",false,false,2);
    }
    {
        // The frame before had no readable identity (slot 2 is no row): the draw's priors are supplied and none can be read.
        FlatForegroundMotion motion;capture(motion,oldPose,2,1620);
        capture(motion,nowPose,1,1621);
        output(motion,1621,2,"a prior without a readable identity cannot serve and rejects with its own reason",false,false,6);
    }
    {
        // The prior's vertices are not finite (an infinite projection): its identity reads and matches, its positions cannot serve.
        FlatForegroundMotion motion;Pose broken=oldPose;broken.projection=std::numeric_limits<float>::infinity();
        capture(motion,broken,1,1630);
        capture(motion,nowPose,1,1631);
        output(motion,1631,2,"a matching prior with positions that are not finite rejects with its own reason",false,false,4);
    }
    {
        // The adapter passes no prior on when the previous draw's pool is another resource: counted, and the draw says no prior was supplied.
        FlatForegroundMotion motion;capture(motion,oldPose,1,1640);
        Microsoft::WRL::ComPtr<ID3D11Device> device;ctx->GetDevice(&device);
        D3D11_BUFFER_DESC d{};d.ByteWidth=sizeof(poolData);d.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        d.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;d.StructureByteStride=336;
        D3D11_SUBRESOURCE_DATA init{poolData,0,0};
        Microsoft::WRL::ComPtr<ID3D11Buffer> otherPool;Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> otherView;
        hr(device->CreateBuffer(&d,&init,&otherPool));hr(device->CreateShaderResourceView(otherPool.Get(),nullptr,&otherView));
        raster(nowPose,1,1,0);ctx->VSSetShaderResources(33,1,otherView.GetAddressOf());inputs.writerToken=1;
        check(motion.capture(ctx,issue,9,1,0,0,0,1641,inputs),"a draw under another pool captures");
        output(motion,1641,2,"a previous draw under another pool is not a prior",false,false,3);
        check(motion.stats().noPriorPool==1 && motion.stats().noPriorNear==0 && motion.stats().noPriorAbsent==0 && motion.stats().noCandidate==1,
              "a draw whose previous draw used another pool is counted as refused by the pool");
    }
    {
        // A previous draw the adapter does not hold as a GPU-identity draw is no prior either (here a CPU-identity one), and nothing else refused it.
        FlatForegroundMotion motion;auto cpu=inputs;cpu.gpuIdentity=false;cpu.identity={1,740,1631,1,1,nullptr};cpu.writerToken=1;
        raster(oldPose,1,1,0);
        check(motion.capture(ctx,issue,9,1,0,0,0,1650,cpu),"a CPU-identity draw captures");
        capture(motion,nowPose,1,1651);
        output(motion,1651,2,"a previous draw of the other identity mode is not a prior",false,false,3);
        check(motion.stats().noPriorAbsent==1 && motion.stats().noPriorPool==0 && motion.stats().noPriorNear==0,
              "a draw whose candidates name no draw the adapter can use is counted as absent");
    }
    {
        FlatForegroundMotion motion;float warmWorld[6][4]{};warmWorld[3][2]=inputs.camera[3][2];
        FlatForegroundMotion::Output warm;motion.beginFrame(1700);
        check(motion.prepareH(ctx,owners,depth,warmWorld,1700,width,height,warm) && warm.qualified && !warm.resetRequired,
              "world-only history is warm under the same canonical depth convention");
        capture(motion,nowPose,1,1701);
        output(motion,1701,2,"new geometry in a continuous world rejects only local history",true,false,3);
        inputs.camera[3][2]=.0125f;nowPose.clip=.0125f;
        capture(motion,nowPose,1,1702);
        output(motion,1702,2,"changed near convention keeps valid new geometry and requests one reset",false,true,3);
        check(motion.stats().noPriorNear==1 && motion.stats().noPriorPool==0 && motion.stats().noPriorAbsent==0,
              "a draw whose previous draw was made under another near is counted as refused by the near");
        capture(motion,nowPose,1,1703);
        output(motion,1703,1,"changed near convention settles with actual GPU history",true);
    }
    {
        // SECTION 104, the pistol's identity-differs window. One draw in thirteen that the map will match by identity has its identity words and its
        // priors' read back a few frames later and classified by the map's own two tests. Fourteen draws of one geometry a frame: the first frame
        // has no priors and samples nothing; the second offers fourteen and the thirteenth is read, and with the pool unchanged the map matches it.
        // Then the record's first word moves (another bone base): the next sample is another record, and says which word.
        auto fourteen=[&](FlatForegroundMotion& motion,unsigned frame) {for(unsigned i=0;i<14;++i)capture(motion,oldPose,1,frame,1);};
        auto verdictCount=[&](const FlatForegroundMotion& motion,edvr::IdentityVerdict v){return motion.stats().identityBy[unsigned(v)];};
        inputs.vs=0xA1;inputs.ps=0xB2;
        FlatForegroundMotion motion;
        fourteen(motion,1800);motion.pollIdentity(ctx,1802,true);
        check(motion.stats().identitySamples==0,"a frame whose draws have no priors samples no identity");
        fourteen(motion,1801);motion.pollIdentity(ctx,1803,true);
        check(motion.stats().identitySamples==1 && verdictCount(motion,edvr::IdentityVerdict::Match)==1,
              "fourteen draws with priors offer fourteen and one is read back: the identity the map matches is classified a match");
        FlatIdentitySampler::Sample none[FlatForegroundMotion::kIdentityExamples];
        check(motion.takeIdentityExamples(none,FlatForegroundMotion::kIdentityExamples)==0,"a match is no example");
        const unsigned savedBase=poolData[84];++poolData[84];ctx->UpdateSubresource(pool,0,nullptr,poolData,0,0);
        fourteen(motion,1802);motion.pollIdentity(ctx,1804,true);
        check(motion.stats().identitySamples==2 && verdictCount(motion,edvr::IdentityVerdict::XDiffers)==1 && verdictCount(motion,edvr::IdentityVerdict::Match)==1,
              "after the record's first word moves the sampled draw is x-differs, as the map decides it");
        FlatIdentitySampler::Sample example[FlatForegroundMotion::kIdentityExamples];
        const unsigned shown=motion.takeIdentityExamples(example,FlatForegroundMotion::kIdentityExamples);
        check(shown==1 && example[0].verdict==edvr::IdentityVerdict::XDiffers && example[0].priors>=1 && example[0].current.x==example[0].prior[0].x+1 &&
                  example[0].current.y==example[0].prior[0].y && example[0].current.z==1 && example[0].vs==0xA1 && example[0].ps==0xB2 && example[0].key.count==9,
              "the example carries the words of the draw and its candidate, which differ in the first word alone, and the draw's shaders and key");
        poolData[84]=savedBase;ctx->UpdateSubresource(pool,0,nullptr,poolData,0,0);
    }
}
