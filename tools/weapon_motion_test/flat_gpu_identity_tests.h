#pragma once

// Actual GPU identity snapshots are the oracle. Deliberately omit every CPU
// row and input certificate, including slots first seen after publication.
template<class Pose,class Raster,class ReadMap>
void flatGpuIdentityTests(ID3D11DeviceContext* ctx,ID3D11ShaderResourceView* owners,
                         ID3D11ShaderResourceView* depth,unsigned width,unsigned height,
                         edvr::FlatForegroundMotion::Inputs inputs,const float world[6][4],
                         Pose oldPose,Pose nowPose,Raster raster,ReadMap readMap,
                         ID3D11Buffer* pool,unsigned (&poolData)[168]) {
    using edvr::FlatForegroundMotion;
    inputs.gpuIdentity=true;inputs.identity={};
    inputs.identity.refusal="identity-pool-slot-unobserved";
    inputs.certificate={};inputs.phaseX=inputs.phaseY=0;
    auto capture=[&](FlatForegroundMotion& motion,Pose pose,unsigned slot,unsigned frame,unsigned writer=1) {
        raster(pose,slot,writer);inputs.writerToken=writer;
        check(motion.capture(ctx,issue,9,1,0,0,0,frame,inputs),"GPU identities capture without CPU row or certificate");
    };
    auto output=[&](FlatForegroundMotion& motion,unsigned frame,float expectedClass,const char* what,bool localOnly=false,bool conventionReset=false) {
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
        const auto pixels=readMap(result.motion.Get());unsigned matching=0;
        for(unsigned p=0;p<width*height;++p)if(pixels[4*p+3]==expectedClass) {
            ++matching;check(pixels[4*p+2]>0 && pixels[4*p+2]<=1,
                             "matched or rejected GPU history retains actual canonical depth");
        }
        check(matching>2000,what);return pixels;
    };
    {
        FlatForegroundMotion motion;capture(motion,oldPose,1,1000);output(motion,1000,2,"new GPU identity rejects old history locally");
        capture(motion,nowPose,1,1001);auto pixels=output(motion,1001,1,"steady GPU identity selects actual previous positions");
        for(unsigned p=0;p<width*height;++p)if(pixels[4*p+3]==1)
            check(std::fabs(pixels[4*p]-(oldPose.mouse-nowPose.mouse)*width*.5f*pixels[4*p+2]/oldPose.clip)<1.f/256,
                  "GPU identity motion matches real previous original-VS output");
    }
    {
        FlatForegroundMotion motion;capture(motion,oldPose,1,1100);
        const unsigned saved=poolData[0];poolData[0]=poolData[84];ctx->UpdateSubresource(pool,0,nullptr,poolData,0,0);
        capture(motion,nowPose,0,1101);output(motion,1101,2,"changed GPU slot cannot borrow same-identity history from another slot");
        poolData[0]=saved;ctx->UpdateSubresource(pool,0,nullptr,poolData,0,0);
    }
    {
        FlatForegroundMotion motion;capture(motion,oldPose,1,1200);
        ++poolData[91];ctx->UpdateSubresource(pool,0,nullptr,poolData,0,0);
        capture(motion,nowPose,1,1201);output(motion,1201,2,"reused GPU slot with another allocation rejects stale history");
        --poolData[91];ctx->UpdateSubresource(pool,0,nullptr,poolData,0,0);
    }
    {
        FlatForegroundMotion motion;capture(motion,oldPose,1,1300);capture(motion,oldPose,1,1300,2);
        capture(motion,nowPose,1,1301);output(motion,1301,1,"identical duplicate GPU triangles coalesce without CPU certificates");
    }
    {
        FlatForegroundMotion motion;Pose different=oldPose;different.mouse+=.07f;
        capture(motion,oldPose,1,1400);capture(motion,different,1,1400,2);
        capture(motion,nowPose,1,1401);output(motion,1401,2,"different duplicate GPU poses reject history rather than choosing a pose");
    }
    {
        FlatForegroundMotion motion;capture(motion,oldPose,1,1500);
        inputs.phaseX=.25f;capture(motion,oldPose,1,1500,2);inputs.phaseX=0;
        capture(motion,nowPose,1,1501);output(motion,1501,2,"different prior phases prevent duplicate GPU equivalence");
    }
    {
        FlatForegroundMotion motion;capture(motion,nowPose,2,1600);
        output(motion,1600,2,"invalid current GPU identity rejects history with actual depth");
    }
    {
        FlatForegroundMotion motion;float warmWorld[6][4]{};warmWorld[3][2]=inputs.camera[3][2];
        FlatForegroundMotion::Output warm;motion.beginFrame(1700);
        check(motion.prepareH(ctx,owners,depth,warmWorld,1700,width,height,warm) && warm.qualified && !warm.resetRequired,
              "world-only history is warm under the same canonical depth convention");
        capture(motion,nowPose,1,1701);
        output(motion,1701,2,"new geometry in a continuous world rejects only local history",true);
        inputs.camera[3][2]=.0125f;nowPose.clip=.0125f;
        capture(motion,nowPose,1,1702);
        output(motion,1702,2,"changed near convention keeps valid new geometry and requests one reset",false,true);
        capture(motion,nowPose,1,1703);
        output(motion,1703,1,"changed near convention settles with actual GPU history",true);
    }
}
