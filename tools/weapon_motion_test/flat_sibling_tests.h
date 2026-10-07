#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <vector>

// A DRAW WITH NO HISTORY OF ITS OWN TAKES ITS SIBLINGS' MOTION (design section 104, the pistol's LOD swap). Real WARP (or hardware) draws through
// the production adapter, owner plane, the sibling pass's two compute shaders and the map's shaders (src\d3d11\flat_foreground_motion_shader.h);
// the policy is src\d3d11\flat_foreground_sibling.h, and the shader's tables are read back and held to it for every draw of every scene.
//
// The pool has twelve rows, one identity each (the covered-draw scenes'). A scene that needs two rows to be one object gives the second the
// first's two identity words (word 0, the bone base; word 7, the record's signature), and puts them back. A piece is one draw of the mesh; the
// pieces of a scene sit in screen regions of their own, and a piece's key is its start in an index buffer of nine-index copies of the quad, so a
// LOD swap is the same piece drawn with another count. The harness draws row r with writer token r + 1.
//   scene S1  a piece swapped for another level of detail beside a sibling that matched: it takes the sibling's motion;
//   scene S2  a swapped piece with no sibling is the view's own (no motion, history kept), and with the pass off the base map refuses it;
//   scene S3  a swapped piece whose only siblings belong to another identity does not borrow their motion;
//   scene S4  siblings that disagree by more than a pixel are refused as before, and by less are averaged;
//   scene S5  a piece that had priors and whose identity differs from them takes its siblings' motion when the pass is engaged for it;
//   scene S6  the identity sampler arms the pass for the draws with priors, so a frame with only such draws is served too;
//   scene S7  the pass costs nothing in a frame with no draw that needs it: no dispatch;
//   scene S8  the jitter phases of the two frames are not in the motion it gives: the siblings' unjittered motion;
//   scene S9  byte 30 of the record, a per-instance parameter, is not part of the identity that makes two draws siblings;
//   scene S10 a record the donor pass wrote for a draw in one frame is not a donor of the next frame's draw at that index;
//   scene S11 a draw the map refused for ambiguous priors (reason 7) donates nothing, though its identity matched.
template<class Raster,class ReadMap>
void flatSiblingTests(ID3D11Device* dev,ID3D11DeviceContext* ctx,ID3D11ShaderResourceView* owners,ID3D11ShaderResourceView* depth,
                      unsigned width,unsigned height,edvr::FlatForegroundMotion::Inputs inputs,const float world[6][4],
                      ID3D11Buffer* sceneIb,ID3D11Buffer* pool,unsigned (&poolData)[12*84],Raster raster,ReadMap readMap) {
    using edvr::FlatForegroundMotion;
    using edvr::SiblingOutcome;
    FlatForegroundMotion::RigSiblingPass sibling(true);
    inputs.gpuIdentity=true;inputs.identity={};inputs.certificate={};inputs.phaseX=inputs.phaseY=0;
    const auto place=[](float centre,float projection) {Pose p{};p.projection=projection;p.mouse=centre/projection;return p;};
    // Eight nine-index copies of the quad: piece k is start 9k, and a count of 6 draws the same two triangles under another key.
    std::vector<UINT> lodIndices;
    for(unsigned k=0;k<8;++k)for(UINT i:{0u,1u,2u,0u,2u,3u,0u,0u,0u})lodIndices.push_back(i);
    D3D11_BUFFER_DESC ibd{};ibd.ByteWidth=UINT(lodIndices.size()*4);ibd.Usage=D3D11_USAGE_DEFAULT;ibd.BindFlags=D3D11_BIND_INDEX_BUFFER;
    D3D11_SUBRESOURCE_DATA ibi{lodIndices.data(),0,0};
    Microsoft::WRL::ComPtr<ID3D11Buffer> lodIb;hr(dev->CreateBuffer(&ibd,&ibi,&lodIb));
    unsigned rowWords[12][2];
    for(unsigned r=0;r<12;++r){rowWords[r][0]=poolData[r*84];rowWords[r][1]=poolData[r*84+7];}
    const auto share=[&](unsigned row,unsigned like) {
        poolData[row*84]=poolData[like*84];poolData[row*84+7]=poolData[like*84+7];ctx->UpdateSubresource(pool,0,nullptr,poolData,0,0);};
    const auto restore=[&] {for(unsigned r=0;r<12;++r){poolData[r*84]=rowWords[r][0];poolData[r*84+7]=rowWords[r][1];}
        ctx->UpdateSubresource(pool,0,nullptr,poolData,0,0);};
    // A piece: the quad at `pose`, drawn into the owner plane as pool row `row` (writer token row + 1, the harness's), and captured with `count`
    // indices at the mesh's start `start`.
    const auto piece=[&](FlatForegroundMotion& motion,const Pose& pose,unsigned row,unsigned frame,bool first,unsigned count,unsigned start) {
        const unsigned token=row+1;
        raster(pose,row,token,first);inputs.writerToken=token;
        ctx->IASetIndexBuffer(lodIb.Get(),DXGI_FORMAT_R32_UINT,0);
        const bool ok=motion.capture(ctx,issue,count,1,start,0,0,frame,inputs);
        ctx->IASetIndexBuffer(sceneIb,DXGI_FORMAT_R32_UINT,0);
        return ok;
    };
    const auto qualify=[&](FlatForegroundMotion& motion,unsigned frame,FlatForegroundMotion::Output& out) {
        return motion.prepareH(ctx,owners,depth,world,frame,width,height,out);
    };
    struct Pixels {std::vector<float> map,owner;};
    const auto read=[&](const FlatForegroundMotion::Output& out) {return Pixels{readMap(out.motion.Get()),readMap(owners)};};
    // What the map holds at the pixels the owner plane gives the row's piece.
    struct Piece {unsigned owned=0,valid=0,rejected=0,noSample=0;float lo[2]={1e30f,1e30f},hi[2]={-1e30f,-1e30f},sum[2]={0,0};unsigned reasons[8]{};
                  float mean(unsigned a) const{return valid?sum[a]/valid:0.f;}
                  float spread(unsigned a) const{return valid?hi[a]-lo[a]:0.f;}};
    const auto census=[&](const Pixels& px,unsigned row) {
        Piece c;
        for(unsigned p=0;p<width*height;++p) {
            if(px.owner[4*p]>-1.5f || unsigned(px.owner[4*p+3])!=row+1)continue;
            ++c.owned;
            const float x=px.map[4*p],y=px.map[4*p+1],z=px.map[4*p+2],w=px.map[4*p+3];
            if(w==0 && x==0 && y==0 && z==-1){++c.noSample;continue;}
            if(w==2){++c.rejected;if(x>=0 && x<8)++c.reasons[unsigned(x)];continue;}
            if(w==1){++c.valid;c.sum[0]+=x;c.sum[1]+=y;c.lo[0]=(std::min)(c.lo[0],x);c.hi[0]=(std::max)(c.hi[0],x);c.lo[1]=(std::min)(c.lo[1],y);c.hi[1]=(std::max)(c.hi[1],y);}
        }
        return c;
    };
    // The shader's tables against the CPU form of the second shader's decision, for every draw of the frame, whatever the scene. `rows` are the
    // pool rows of the frame's draws, in the order they were captured.
    const auto tablesAgree=[&](FlatForegroundMotion& motion,const std::vector<unsigned>& rows,const char* what) {
        const unsigned draws=unsigned(rows.size());
        std::vector<edvr::SiblingDonor> donors;std::vector<std::array<float,8>> fit;unsigned n=0;
        const bool got=motion.readSiblingTables(ctx,donors,fit,n);
        check(got && n==draws,(std::string(what)+": the sibling tables are read back for every draw of the frame").c_str());
        std::vector<edvr::SiblingFit> model;
        if(!got || n!=draws)return model;
        bool agree=true;
        for(unsigned i=0;i<n;++i) {
            const uint32_t id[4]={poolData[rows[i]*84],poolData[rows[i]*84+7],1,0};
            const auto m=edvr::siblingDecide(donors.data(),n,i,id);
            model.push_back(m);
            const auto& g=fit[i];
            const bool decided=m.outcome!=SiblingOutcome::Matched && m.outcome!=SiblingOutcome::IdentityUnreadable;
            const bool same=unsigned(g[2]+.5f)==unsigned(m.outcome) &&
                std::fabs(g[0]-m.motion[0])<1e-4f && std::fabs(g[1]-m.motion[1])<1e-4f &&
                (!decided || std::fabs(g[3]-m.spread)<1e-3f) &&
                (!decided || (unsigned(g[4]+.5f)==m.donors && std::fabs(g[5]-m.vertices)<.5f)) &&
                ((g[6]!=0)==donors[i].matched);
            agree=agree && same;
        }
        check(agree,(std::string(what)+": every draw's fit is what the policy decides from the donors' own records (flat_foreground_sibling.h)").c_str());
        return model;
    };
    const auto pollAll=[&](FlatForegroundMotion& motion,unsigned frame){motion.pollSibling(ctx,frame+8,true);};
    const unsigned countChange=unsigned(edvr::HistoryGap::CountChange);
    const float pr=.3f;
    // The game's compute stage, as a sentinel in every slot the pass binds (t0..t10, u0, b0 and the shader): put back exactly, by identity.
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> sentinelCs;
    Microsoft::WRL::ComPtr<ID3D11Buffer> sentinelBuffer,sentinelUavBuffer,sentinelCb;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> sentinelSrv;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> sentinelUav;
    {
        const char* source="[numthreads(1,1,1)] void main(){}";
        Microsoft::WRL::ComPtr<ID3DBlob> code,errors;
        hr(D3DCompile(source,std::strlen(source),"sibling sentinel",nullptr,nullptr,"main","cs_5_0",0,0,&code,&errors));
        hr(dev->CreateComputeShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&sentinelCs));
        D3D11_BUFFER_DESC d{};d.ByteWidth=64;d.Usage=D3D11_USAGE_DEFAULT;d.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS;
        d.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;d.StructureByteStride=16;
        hr(dev->CreateBuffer(&d,nullptr,&sentinelBuffer));hr(dev->CreateShaderResourceView(sentinelBuffer.Get(),nullptr,&sentinelSrv));
        hr(dev->CreateBuffer(&d,nullptr,&sentinelUavBuffer));hr(dev->CreateUnorderedAccessView(sentinelUavBuffer.Get(),nullptr,&sentinelUav));
        D3D11_BUFFER_DESC c{};c.ByteWidth=64;c.Usage=D3D11_USAGE_DEFAULT;c.BindFlags=D3D11_BIND_CONSTANT_BUFFER;hr(dev->CreateBuffer(&c,nullptr,&sentinelCb));
    }
    const auto holdComputeState=[&] {
        ctx->CSSetShader(sentinelCs.Get(),nullptr,0);
        ID3D11ShaderResourceView* views[11];for(auto& v:views)v=sentinelSrv.Get();ctx->CSSetShaderResources(0,11,views);
        ID3D11UnorderedAccessView* uav=sentinelUav.Get();ctx->CSSetUnorderedAccessViews(0,1,&uav,nullptr);
        ID3D11Buffer* cb=sentinelCb.Get();ctx->CSSetConstantBuffers(0,1,&cb);
    };
    const auto computeStateKept=[&] {
        Microsoft::WRL::ComPtr<ID3D11ComputeShader> shader;ctx->CSGetShader(&shader,nullptr,nullptr);
        ID3D11ShaderResourceView* views[11]{};ctx->CSGetShaderResources(0,11,views);
        ID3D11UnorderedAccessView* uav=nullptr;ctx->CSGetUnorderedAccessViews(0,1,&uav);
        ID3D11Buffer* cb=nullptr;ctx->CSGetConstantBuffers(0,1,&cb);
        bool kept=shader.Get()==sentinelCs.Get() && uav==sentinelUav.Get() && cb==sentinelCb.Get();
        for(auto* v:views){kept=kept && v==sentinelSrv.Get();if(v)v->Release();}
        if(uav)uav->Release();if(cb)cb->Release();
        ID3D11ShaderResourceView* none[11]{};ctx->CSSetShaderResources(0,11,none);ID3D11UnorderedAccessView* noUav=nullptr;ctx->CSSetUnorderedAccessViews(0,1,&noUav,nullptr);
        ctx->CSSetShader(nullptr,nullptr,0);ID3D11Buffer* noCb=nullptr;ctx->CSSetConstantBuffers(0,1,&noCb);
        return kept;
    };
    const auto still=[](const Piece& c){return c.owned>300 && c.valid==c.owned && std::fabs(c.lo[0])<1.f/256 && std::fabs(c.hi[0])<1.f/256 &&
                                              std::fabs(c.lo[1])<1.f/256 && std::fabs(c.hi[1])<1.f/256;};

    // ---- S1: a swapped piece beside a sibling that matched ------------------------------------------------------------------------------
    {
        share(1,0);
        FlatForegroundMotion motion;FlatForegroundMotion::Output out;
        const Pose body=place(-.45f,pr),detail=place(.05f,pr);
        piece(motion,body,0,5000,true,9,0);piece(motion,detail,1,5000,false,9,9);
        check(qualify(motion,5000,out) && out.qualified,"S1: the seed frame qualifies");
        Pose bodyNow=body,detailNow=detail;bodyNow.mouse+=.1f;detailNow.mouse+=.1f;
        piece(motion,bodyNow,0,5001,true,9,0);
        check(piece(motion,detailNow,1,5001,false,6,9),"S1: the swapped piece captures");
        check(motion.stats().missBy[countChange]==1 && motion.stats().noCandidate==3,"S1: the swapped piece is a count-change miss with no candidate (the seed frame's two draws had none either)");
        holdComputeState();
        check(qualify(motion,5001,out) && out.qualified && motion.siblingRan(),"S1: the frame qualifies, and the sibling pass ran for it");
        check(computeStateKept(),"S1: the game's compute stage (shader, t0-t10, u0, b0) is exactly as it was after the pass");
        const auto px=read(out);
        const Piece b=census(px,0),d=census(px,1);
        check(b.owned>300 && b.valid==b.owned && std::fabs(b.mean(0))>.5f,"S1: the sibling that matched has valid, real motion");
        check(d.owned>300 && d.valid==d.owned && d.rejected==0,"S1: the swapped piece's pixels are all valid: it is not refused to the raw frame");
        check(d.spread(0)<1.f/64 && d.spread(1)<1.f/64,"S1: and its motion is one value at every pixel (a uniform translation)");
        check(std::fabs(d.mean(0)-b.mean(0))<.1f && std::fabs(d.mean(1)-b.mean(1))<.1f,
              "S1: which is the sibling's mean motion, to a tenth of a pixel (the donors' vertices against their pixels)");
        check(d.mean(0)>=b.lo[0]-1.f/64 && d.mean(0)<=b.hi[0]+1.f/64,"S1: and lies inside the range the sibling's own pixels span");
        const auto model=tablesAgree(motion,{0,1},"S1");
        check(model.size()==2 && model[0].outcome==SiblingOutcome::Matched && model[1].outcome==SiblingOutcome::Sibling && model[1].donors==1,
              "S1: the first draw matched its own history; the second has one donor and takes it");
        pollAll(motion,5001);
        check(motion.stats().siblingBy[countChange][0]==1 && motion.stats().siblingFrames==2 && motion.stats().siblingReads==2 &&
                  motion.stats().siblingDispatches==3 && motion.stats().siblingBy[unsigned(edvr::HistoryGap::PreviousFrameEmpty)][1]==2,
              "S1: the read-back files the swapped piece under count-change, sibling; the seed frame's two draws, which had no donor, view-attached; "
              "three dispatches in all (the seed frame's fit, then the one donor and the fit)");
        restore();
    }

    // ---- S2: a swapped piece with no sibling is the view's own -------------------------------------------------------------------------
    {
        const Pose before=place(-.2f,pr);
        Pose now=before;now.mouse+=.1f;
        {
            FlatForegroundMotion motion;FlatForegroundMotion::Output out;
            piece(motion,before,0,5100,true,9,0);check(qualify(motion,5100,out),"S2: the seed frame qualifies");
            piece(motion,now,0,5101,true,6,0);
            check(qualify(motion,5101,out) && out.qualified && motion.siblingRan(),"S2: the frame qualifies with the pass run");
            check(still(census(read(out),0)),"S2: a piece with no sibling is valid with no motion at every pixel: the view's own, its history kept");
            const auto model=tablesAgree(motion,{0},"S2");
            check(model.size()==1 && model[0].outcome==SiblingOutcome::ViewAttached && model[0].donors==0,"S2: the fit says view-attached, with no donor");
            pollAll(motion,5101);
            check(motion.stats().siblingBy[countChange][1]==1,"S2: the read-back files it under count-change, view-attached");
        }
        {
            FlatForegroundMotion::RigSiblingPass off(false);
            FlatForegroundMotion motion;FlatForegroundMotion::Output out;
            piece(motion,before,0,5110,true,9,0);check(qualify(motion,5110,out),"S2: with the pass off the seed frame qualifies");
            piece(motion,now,0,5111,true,6,0);
            check(qualify(motion,5111,out) && out.qualified && !motion.siblingRan(),"S2: with the pass off it does not run");
            const Piece c=census(read(out),0);
            check(c.owned>300 && c.rejected==c.owned && c.reasons[3]==c.rejected,
                  "S2: and the base map refuses the same piece, with reason 3 (no prior): the control the pass changes");
        }
    }

    // ---- S3: another object's siblings are not borrowed ---------------------------------------------------------------------------------
    {
        FlatForegroundMotion motion;FlatForegroundMotion::Output out;
        const Pose a=place(-.45f,pr),b=place(.05f,pr);
        piece(motion,a,0,5200,true,9,0);piece(motion,b,2,5200,false,9,9);
        check(qualify(motion,5200,out),"S3: the seed frame qualifies");
        Pose aNow=a,bNow=b;aNow.mouse+=.1f;bNow.mouse+=.1f;
        piece(motion,aNow,0,5201,true,9,0);piece(motion,bNow,2,5201,false,6,9);
        check(qualify(motion,5201,out) && out.qualified && motion.siblingRan(),"S3: the frame qualifies with the pass run");
        const auto px=read(out);
        const Piece pa=census(px,0);
        check(pa.valid==pa.owned && std::fabs(pa.mean(0))>.5f,"S3: the first object's piece matched and moves");
        check(still(census(px,2)),
              "S3: the second object's swapped piece has none of the first object's motion: no sibling of its own identity, so the view's own");
        const auto model=tablesAgree(motion,{0,2},"S3");
        check(model.size()==2 && model[1].outcome==SiblingOutcome::ViewAttached && model[1].donors==0,
              "S3: the second piece's fit finds no donor: the first object's identity is not its own");
    }

    // ---- S4: siblings that disagree, and siblings that nearly agree ---------------------------------------------------------------------
    {
        share(1,0);share(4,0);
        const auto run=[&](float secondDelta,unsigned frame,const char* label,SiblingOutcome expected) {
            FlatForegroundMotion motion;FlatForegroundMotion::Output out;
            const Pose a=place(-.7f,pr),b=place(-.25f,pr),c=place(.3f,pr);
            piece(motion,a,0,frame,true,9,0);piece(motion,b,1,frame,false,6,9);piece(motion,c,4,frame,false,9,18);
            check(qualify(motion,frame,out),(std::string(label)+": the seed frame qualifies").c_str());
            Pose aNow=a,bNow=b,cNow=c;aNow.mouse+=.05f;bNow.mouse+=secondDelta;cNow.mouse+=.05f;
            piece(motion,aNow,0,frame+1,true,9,0);piece(motion,bNow,1,frame+1,false,6,9);piece(motion,cNow,4,frame+1,false,6,18);
            check(qualify(motion,frame+1,out) && out.qualified && motion.siblingRan(),(std::string(label)+": the frame qualifies with the pass run").c_str());
            const Piece pc=census(read(out),4);
            const auto model=tablesAgree(motion,{0,1,4},label);
            check(model.size()==3 && model[2].outcome==expected,(std::string(label)+": the policy decides "+edvr::siblingOutcomeName(unsigned(expected)-1)).c_str());
            return std::make_pair(pc,model.size()==3?model[2]:edvr::SiblingFit());
        };
        const auto agree=run(.058f,5300,"S4 nearly agreeing donors",SiblingOutcome::Sibling);
        check(agree.first.owned>300 && agree.first.valid==agree.first.owned && agree.second.spread>0 && agree.second.spread<=edvr::kFlatSiblingSpreadPixels &&
                  agree.second.donors==2,
              "S4: donors within a pixel of each other give the third piece their mean, weighted by their vertices (nine and six), all its pixels valid");
        const auto disagree=run(.30f,5400,"S4 disagreeing donors",SiblingOutcome::Disagree);
        check(disagree.first.owned>300 && disagree.first.rejected==disagree.first.owned && disagree.first.reasons[3]==disagree.first.rejected &&
                  disagree.second.spread>edvr::kFlatSiblingSpreadPixels,
              "S4: donors more than a pixel apart are refused as before: the third piece's pixels carry reason 3");
        restore();
    }

    // ---- S5: a draw with priors and another identity takes its siblings' motion ---------------------------------------------------------
    {
        share(1,0);
        FlatForegroundMotion motion;FlatForegroundMotion::Output out;
        const Pose a=place(-.7f,pr),b=place(-.25f,pr),c=place(.3f,pr);
        // The seed frame: the first object's body, and a piece at start 9 that belongs to another record (row 2).
        piece(motion,a,0,5500,true,9,0);piece(motion,b,2,5500,false,9,9);
        check(qualify(motion,5500,out),"S5: the seed frame qualifies");
        // This frame: the same piece is under the first object's record (row 1, the first's identity): its prior has another identity. A third
        // piece with no prior engages the pass.
        Pose aNow=a,bNow=b,cNow=c;aNow.mouse+=.1f;bNow.mouse+=.1f;cNow.mouse+=.1f;
        piece(motion,aNow,0,5501,true,9,0);piece(motion,bNow,1,5501,false,9,9);piece(motion,cNow,4,5501,false,9,18);
        check(motion.stats().priorsOne==2,"S5: the body and the piece each have one prior, the adapter's");
        check(qualify(motion,5501,out) && out.qualified && motion.siblingRan(),"S5: the frame qualifies with the pass run");
        const auto px=read(out);
        const Piece pa=census(px,0),pb=census(px,1);
        check(pa.valid==pa.owned && pb.owned>300 && pb.valid==pb.owned && pb.rejected==0 && std::fabs(pb.mean(0)-pa.mean(0))<.1f,
              "S5: the piece whose prior is another record takes the first object's motion, not a refusal (the base map says reason 5)");
        const auto model=tablesAgree(motion,{0,1,4},"S5");
        check(model.size()==3 && model[1].outcome==SiblingOutcome::Sibling,"S5: the policy gives it the sibling's motion");
        pollAll(motion,5501);
        check(motion.stats().siblingBy[edvr::kSiblingIdentityDiffers][0]==1,"S5: the read-back files it under identity-differs, sibling");
        restore();
    }

    // ---- S6: the identity sampler arms the pass for draws with priors ------------------------------------------------------------------
    {
        share(1,0);
        FlatForegroundMotion motion;FlatForegroundMotion::Output out;
        // The second piece is another record in the frame before (row 2) and the first object's in this one (row 1). The sampler reads the
        // thirteenth draw offered with priors: eleven probes (one key, a prior each), the body, and then that piece.
        const auto frameOf=[&](unsigned frame,unsigned secondRow,float delta,bool probes) {
            bool first=true;
            if(probes)for(unsigned k=0;k<11;++k){piece(motion,place(.3f,pr),4,frame,first,9,18);first=false;}
            Pose body=place(-.7f,pr);body.mouse+=delta;
            piece(motion,body,0,frame,first,9,0);
            Pose second=place(-.25f,pr);second.mouse+=delta;
            piece(motion,second,secondRow,frame,false,9,9);
        };
        frameOf(5600,2,0,true);
        check(qualify(motion,5600,out),"S6: the seed frame qualifies");
        frameOf(5601,1,.1f,true);
        motion.pollIdentity(ctx,5603,true);
        check(motion.stats().identitySamples==1 && motion.stats().identityBy[unsigned(edvr::IdentityVerdict::Match)]==0,
              "S6: the sampler read the piece the map will not match by identity (the thirteenth draw offered with priors)");
        // The next frame has no draw without a prior, so nothing the CPU knows would engage the pass: the arming does. Its second piece is
        // another record than the frame before's again (row 2), so the map would refuse it.
        frameOf(5602,2,.2f,false);
        check(qualify(motion,5602,out) && out.qualified && motion.siblingRan(),"S6: armed by the sample, the pass runs for a frame whose every draw has priors");
        const auto px=read(out);
        const Piece pb=census(px,2);
        check(pb.owned>300 && pb.valid==pb.owned && pb.rejected==0,"S6: and the draw whose prior is another record is valid, not refused (the base map says reason 5)");
        restore();
    }

    // ---- S7: no draw that needs the pass, no pass ----------------------------------------------------------------------------------------
    {
        FlatForegroundMotion motion;FlatForegroundMotion::Output out;
        const Pose a=place(-.45f,pr);
        piece(motion,a,0,5800,true,9,0);check(qualify(motion,5800,out),"S7: the seed frame qualifies (its draw has no prior: the pass runs for it)");
        const uint64_t frames=motion.stats().siblingFrames,dispatches=motion.stats().siblingDispatches;
        Pose aNow=a;aNow.mouse+=.1f;
        piece(motion,aNow,0,5801,true,9,0);
        check(qualify(motion,5801,out) && out.qualified && !motion.siblingRan() && motion.stats().siblingFrames==frames &&
                  motion.stats().siblingDispatches==dispatches,
              "S7: a frame in which every draw matched runs no sibling pass and dispatches nothing");
        check(census(read(out),0).valid>300,"S7: its map is the base map's");
    }
    // ---- S8: the jitter phases of the two frames are not in the motion ---------------------------------------------------------------------
    {
        share(1,0);
        FlatForegroundMotion motion;FlatForegroundMotion::Output out;
        const Pose body=place(-.45f,pr),detail=place(.05f,pr);
        inputs.phaseX=.25f;inputs.phaseY=-.375f;
        piece(motion,body,0,5900,true,9,0);piece(motion,detail,1,5900,false,9,9);
        check(qualify(motion,5900,out),"S8: the seed frame qualifies, under a jitter phase");
        inputs.phaseX=-.125f;inputs.phaseY=.5f;
        Pose bodyNow=body,detailNow=detail;bodyNow.mouse+=.1f;detailNow.mouse+=.1f;
        piece(motion,bodyNow,0,5901,true,9,0);piece(motion,detailNow,1,5901,false,6,9);
        check(qualify(motion,5901,out) && out.qualified && motion.siblingRan(),"S8: the next frame, under another phase, qualifies with the pass run");
        const auto px=read(out);
        const Piece b=census(px,0),d=census(px,1);
        check(b.valid==b.owned && d.owned>300 && d.valid==d.owned,"S8: the matched piece and the swapped one are valid");
        check(std::fabs(d.mean(0)-b.mean(0))<.1f && std::fabs(d.mean(1)-b.mean(1))<.1f,
              "S8: the swapped piece's motion is the sibling's, with both phases removed (the phases differ by .375 and .875 pixels: a residue would show)");
        check(std::fabs(d.mean(0)-b.mean(0))<std::fabs(.375f)/2 && std::fabs(d.mean(1)-b.mean(1))<std::fabs(.875f)/2,
              "S8: and it is far inside either phase step");
        tablesAgree(motion,{0,1},"S8");
        inputs.phaseX=inputs.phaseY=0;
        restore();
    }

    // ---- S9: byte 30 is a per-instance parameter, not identity ------------------------------------------------------------------------------
    {
        // Both records carry a byte 30, and not the same one: the first's is 3, the second's 5.
        poolData[0*84+7]^=0x3u<<16;share(1,0);
        poolData[1*84+7]^=0x6u<<16;ctx->UpdateSubresource(pool,0,nullptr,poolData,0,0);
        FlatForegroundMotion motion;FlatForegroundMotion::Output out;
        const Pose body=place(-.45f,pr),detail=place(.05f,pr);
        piece(motion,body,0,6000,true,9,0);piece(motion,detail,1,6000,false,9,9);
        check(qualify(motion,6000,out),"S9: the seed frame qualifies");
        Pose bodyNow=body,detailNow=detail;bodyNow.mouse+=.1f;detailNow.mouse+=.1f;
        piece(motion,bodyNow,0,6001,true,9,0);piece(motion,detailNow,1,6001,false,6,9);
        check(qualify(motion,6001,out) && out.qualified && motion.siblingRan(),"S9: the frame qualifies with the pass run");
        const auto px=read(out);
        const Piece b=census(px,0),d=census(px,1);
        check(b.valid==b.owned && d.owned>300 && d.valid==d.owned && std::fabs(d.mean(0)-b.mean(0))<.1f && std::fabs(d.mean(0))>.5f,
              "S9: a piece whose record differs from its sibling's in byte 30 alone is still a sibling: it takes the motion");
        const auto model=tablesAgree(motion,{0,1},"S9");
        check(model.size()==2 && model[1].outcome==SiblingOutcome::Sibling,"S9: the policy says so");
        restore();
    }

    // ---- S10: yesterday's donor record is not today's ---------------------------------------------------------------------------------------
    {
        share(1,0);
        FlatForegroundMotion motion;FlatForegroundMotion::Output out;
        const Pose body=place(-.45f,pr),detail=place(.05f,pr);
        const Pose third=place(.5f,pr);
        piece(motion,body,0,6100,true,9,0);piece(motion,detail,1,6100,false,9,9);qualify(motion,6100,out);
        // A third piece of a new key engages the pass in the frame between, so the donors' records of both matched draws are written.
        Pose bodyNow=body,detailNow=detail;bodyNow.mouse+=.1f;detailNow.mouse+=.1f;
        piece(motion,bodyNow,0,6101,true,9,0);piece(motion,detailNow,1,6101,false,9,9);piece(motion,third,4,6101,false,9,18);
        check(qualify(motion,6101,out) && motion.siblingRan(),"S10: the frame between, with a piece of a new key, runs the pass; both other draws are donors in it");
        // The next frame the second draw has a key of its own and no prior: its index held a donor's record the frame before.
        Pose bodyLater=bodyNow,detailLater=detailNow;bodyLater.mouse+=.1f;detailLater.mouse+=.1f;
        piece(motion,bodyLater,0,6102,true,9,0);piece(motion,detailLater,1,6102,false,9,27);
        check(qualify(motion,6102,out) && out.qualified && motion.siblingRan(),"S10: the frame with a piece of a new key qualifies, the pass run");
        const auto px=read(out);
        const Piece b=census(px,0),d=census(px,1);
        check(b.valid==b.owned && d.owned>300 && d.valid==d.owned && d.rejected==0 && std::fabs(d.mean(0)-b.mean(0))<.1f,
              "S10: the new-key piece takes the sibling's motion, and not a stale record's: its own index holds nothing from the frame before");
        const auto model=tablesAgree(motion,{0,1},"S10");
        check(model.size()==2 && model[0].outcome==SiblingOutcome::Matched && model[1].outcome==SiblingOutcome::Sibling,"S10: the policy says so");
        restore();
    }
    // ---- S11: an ambiguous draw is no donor ---------------------------------------------------------------------------------------------
    {
        share(1,0);
        FlatForegroundMotion motion;FlatForegroundMotion::Output out;
        const Pose body=place(-.45f,pr),detail=place(.05f,pr);
        Pose other=body;other.mouse+=.07f;
        // Two draws of one key at two poses in the frame before: the body has two priors that differ, and no pose is chosen.
        piece(motion,body,0,6200,true,9,0);piece(motion,other,0,6200,false,9,0);piece(motion,detail,1,6200,false,9,9);
        check(qualify(motion,6200,out),"S11: the seed frame qualifies");
        Pose bodyNow=body,detailNow=detail;bodyNow.mouse+=.1f;detailNow.mouse+=.1f;
        piece(motion,bodyNow,0,6201,true,9,0);piece(motion,detailNow,1,6201,false,6,9);
        check(qualify(motion,6201,out) && out.qualified && motion.siblingRan(),"S11: the frame qualifies with the pass run");
        const auto px=read(out);
        const Piece b=census(px,0);
        check(b.owned>300 && b.rejected==b.owned && b.reasons[7]==b.rejected,"S11: the body is refused as before, with reason 7 (ambiguous priors)");
        check(still(census(px,1)),"S11: the swapped piece has no donor, for the body's priors were ambiguous and its motion is nobody's: the view's own");
        const auto model=tablesAgree(motion,{0,1},"S11");
        check(model.size()==2 && model[1].outcome==SiblingOutcome::ViewAttached && model[1].donors==0,"S11: the policy says so: the ambiguous body is a matched draw with no vertices");
        restore();
    }
    restore();
}
