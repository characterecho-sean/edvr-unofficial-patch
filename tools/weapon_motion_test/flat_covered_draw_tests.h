#pragma once
#include <algorithm>
#include <cmath>
#include <string>

// THE GRENADE HOLD (design section 104). Real WARP (or hardware) draws through the production adapter, owner plane and map shader.
//
// A first-person draw refused its capture (the occurrence cap, the history budget, any history-stage reason) is not the frame's refusal: its
// pixels are marked first-person in the owner plane by the marker that runs beside the capture, and the map carries no sample for them, so
// the prep refuses their history and gives them no motion (flat_sdk_foreground_gpu_tests.h pins that half). Here:
//   scene A  a draw over the occurrence cap: the frame qualifies, the draw before it keeps real history, its pixels have no sample.
//   scene B  a draw over the history budget (the log's frame 39036): the same.
//   scene C  every draw refused: the map still follows the GPU convention (z of -1 for "no sample"), not the legacy clear's depth of zero.
//   scene D  eight draws of one mesh with eight identities (the grenade's pieces), past the old cap of four: each keeps its own history
//            and its own motion, in order and with two neighbours swapped; in the reverse order a piece whose prior is outside its window
//            is refused locally and none ever takes another piece's motion.
//   scene E  a covered draw is not a free pass: uncovered (no owner mark), the same refusal is the frame's and prepareH refuses.
// The pool here has twelve rows (word 0 the bone base: 1000 + row, word 7 the signature: 1631), one identity per row.
// "The draw before" and "the draw over" a cap are two pieces on screen of one mesh, the first captured and the second refused.
template<class Raster,class ReadMap>
void flatCoveredDrawTests(ID3D11Device* dev,ID3D11DeviceContext* ctx,ID3D11ShaderResourceView* owners,ID3D11ShaderResourceView* depth,
                          unsigned width,unsigned height,edvr::FlatForegroundMotion::Inputs inputs,const float world[6][4],
                          ID3D11Buffer* sceneIb,Raster raster,ReadMap readMap) {
    using edvr::FlatForegroundMotion;
    // The base map's contract: reasons 3, 5 and 6 for a draw with no history (the sibling pass, flat_foreground_sibling.h, replaces them).
    FlatForegroundMotion::RigSiblingPass baseMap(false);
    inputs.gpuIdentity=true;inputs.identity={};inputs.certificate={};inputs.phaseX=inputs.phaseY=0;
    const auto place=[](float centre,float projection) {   // the quad's centre in NDC at w=1, and its half width .6 * projection
        Pose p{};p.projection=projection;p.mouse=centre/projection;return p;
    };
    // piece p of n at screen slot p: draws one mesh with pool row p, writer token p+1
    const auto piece=[&](FlatForegroundMotion& motion,const Pose& pose,unsigned row,unsigned token,unsigned frame,bool first) {
        raster(pose,row,token,first);inputs.writerToken=token;
        return motion.capture(ctx,issue,9,1,0,0,0,frame,inputs);
    };
    // Filler captures with the state the last piece left bound: the same key (an occurrence of it), no raster, no owner mark.
    const auto sameKeyFillers=[&](FlatForegroundMotion& motion,unsigned count,unsigned firstToken,unsigned frame) {
        auto filler=inputs;
        for(unsigned i=0;i<count;++i){filler.writerToken=firstToken+i;
            check(motion.capture(ctx,issue,9,1,0,0,0,frame,filler),"a same-key filler draw is admitted");}
    };
    // Tiny distinct keys (three indices at a start of their own): they take records, and no owner pixel.
    auto fillerIb=flatHistoryPressureIndexBuffer(dev,[]{std::vector<UINT> v(edvr::AnimatedVertexHistory::maxRecords*3);
        for(size_t i=0;i<v.size();++i)v[i]=UINT(i%3);return v;}());
    const auto distinctFillers=[&](FlatForegroundMotion& motion,unsigned from,unsigned count,unsigned firstToken,unsigned frame) {
        ctx->IASetIndexBuffer(fillerIb.Get(),DXGI_FORMAT_R32_UINT,0);
        auto filler=inputs;
        for(unsigned i=0;i<count;++i){filler.writerToken=firstToken+i;
            check(motion.capture(ctx,issue,3,1,(from+i)*3,0,0,frame,filler),"a distinct-key filler draw is admitted");}
        ctx->IASetIndexBuffer(sceneIb,DXGI_FORMAT_R32_UINT,0);
    };
    const auto qualify=[&](FlatForegroundMotion& motion,unsigned frame,FlatForegroundMotion::Output& out) {
        return motion.prepareH(ctx,owners,depth,world,frame,width,height,out);
    };
    struct Pixels {std::vector<float> map,owner;};
    const auto read=[&](const FlatForegroundMotion::Output& out) {return Pixels{readMap(out.motion.Get()),readMap(owners)};};
    // The pixels the owner plane marks first-person for the token (a marker below -1.5, the writer in w), counted by what the map holds.
    struct Census {unsigned owned=0,noSample=0,matched=0,rejected=0,wrongMotion=0;unsigned reasons[8]{};};
    const auto census=[&](const Pixels& px,unsigned token,float (*expectedX)(void*,float),void* context) {
        Census c;
        for(unsigned p=0;p<width*height;++p) {
            if(px.owner[4*p]>-1.5f || unsigned(px.owner[4*p+3])!=token)continue;
            ++c.owned;
            const float x=px.map[4*p],y=px.map[4*p+1],z=px.map[4*p+2],w=px.map[4*p+3];
            if(w==0 && x==0 && y==0 && z==-1){++c.noSample;continue;}
            if(w==2){++c.rejected;if(x>=0 && x<8)++c.reasons[unsigned(x)];continue;}
            if(w==1) {
                ++c.matched;
                if(expectedX && !(std::fabs(x-expectedX(context,z))<1.f/128 && std::fabs(y)<1.f/128))++c.wrongMotion;
            }
        }
        return c;
    };
    struct Motion {float deltaMouse,projection;};
    const auto motionX=[](void* context,float z)->float {   // the real previous output against this one: (previous - now) * projection * W/2 * depth / clip
        const auto* m=static_cast<const Motion*>(context);return m->deltaMouse*m->projection*64.f*z/.025f;
    };

    // ---- A: a draw over the occurrence cap ----------------------------------------------------------------------------------------------
    {
        FlatForegroundMotion motion;FlatForegroundMotion::Output out;
        const Pose left=place(-.4f,.3f),right=place(.4f,.3f);
        Pose leftNow=left;leftNow.mouse+=.1f;
        for(unsigned frame=3000;frame<3002;++frame) {
            const Pose& p0=frame==3000?left:leftNow;
            check(piece(motion,p0,0,1,frame,true),"scene A: the draw before the cap captures");
            sameKeyFillers(motion,edvr::AnimatedVertexHistory::maxExtendedOccurrences-1,11,frame);
            check(!piece(motion,right,1,2,frame,false) && !motion.refusal() && motion.drawRefusal() &&
                  !std::strcmp(motion.drawRefusal(),"occurrence-cap"),
                  "scene A: the draw over the cap is refused as the draw's refusal, not the frame's");
            motion.coverDraw(motion.drawRefusal());
            check(qualify(motion,frame,out) && out.qualified && out.coveredDraws==1,
                  "scene A: a frame with a covered draw still qualifies, and says it holds one");
            const auto px=read(out);
            Motion m{-.1f,.3f};
            const auto first=census(px,1,frame==3001?+motionX:nullptr,&m);
            const auto refused=census(px,2,nullptr,nullptr);
            check(first.owned>300 && refused.owned>300,"scene A: both draws own pixels");
            check(refused.noSample==refused.owned,"scene A: every pixel of the draw over the cap has no map sample (a rejection is not a sample either)");
            check(frame==3000?first.rejected==first.owned && first.reasons[3]==first.rejected:
                              first.matched==first.owned && first.wrongMotion==0,
                  frame==3000?"scene A: the first frame of the draw before the cap rejects with no prior":
                              "scene A: the draw before the cap keeps real history and its real motion beside the covered one");
        }
        check(motion.stats().coveredOccurrence==2 && motion.stats().coveredBudget==0 && motion.stats().coveredOther==0,
              "scene A: the covered draws are counted under the occurrence cap");
    }

    // ---- B: a draw over the history budget -----------------------------------------------------------------------------------------------
    {
        FlatForegroundMotion motion;FlatForegroundMotion::Output out;
        constexpr unsigned cap=edvr::AnimatedVertexHistory::maxRecords;
        const Pose left=place(-.4f,.3f),right=place(.4f,.3f);
        Pose leftNow=left;leftNow.mouse+=.1f;
        // The frame before: the left draw and cap-25 distinct keys. This frame: the left draw again (its own record), 23 more keys, which
        // take the history to the record limit exactly, and then the right draw, a second occurrence of the left's key: no record left.
        check(piece(motion,left,0,1,3100,true),"scene B: the seed frame captures the left draw");
        distinctFillers(motion,0,cap-24-1,100,3100);
        check(piece(motion,leftNow,0,1,3101,true),"scene B: the left draw keeps its record");
        distinctFillers(motion,cap-24-1,24,300,3101);
        check(!piece(motion,right,1,2,3101,false) && !motion.refusal() && motion.drawRefusal() &&
              !std::strcmp(motion.drawRefusal(),"history-budget"),
              "scene B: the draw that finds no record is refused as the draw's refusal, not the frame's");
        check(motion.budgetReceipt().valid==1 && motion.budgetReceipt().recordLimitHit==1 && motion.budgetReceipt().records==cap &&
              motion.budgetReceipt().byteLimitHit==0 && motion.budgetReceipt().older==0,
              "scene B: the budget receipt names the record limit, with no spent record to give back");
        motion.coverDraw(motion.drawRefusal());
        check(qualify(motion,3101,out) && out.qualified && out.coveredDraws==1,"scene B: the frame qualifies with one covered draw");
        const auto px=read(out);
        Motion m{-.1f,.3f};
        const auto first=census(px,1,+motionX,&m);
        const auto refused=census(px,2,nullptr,nullptr);
        check(first.owned>300 && first.matched==first.owned && first.wrongMotion==0,"scene B: the draw with a record keeps real history");
        check(refused.owned>300 && refused.noSample==refused.owned,"scene B: the draw without one has no sample at any of its pixels");
        check(motion.stats().coveredBudget==1 && motion.stats().coveredOccurrence==0,"scene B: the covered draw is counted under the budget");
    }

    // ---- C: every draw refused ------------------------------------------------------------------------------------------------------------
    {
        FlatForegroundMotion motion;FlatForegroundMotion::Output out;
        auto unreadable=inputs;unreadable.camera[3][2]=0;
        raster(place(0,1),0,1,true);unreadable.writerToken=1;
        check(!motion.capture(ctx,issue,9,1,0,0,0,3200,unreadable) && !motion.refusal() && motion.drawRefusal() &&
              !std::strcmp(motion.drawRefusal(),"foreground-identity-or-camera"),
              "scene C: a draw whose camera is unreadable is the draw's refusal");
        motion.coverDraw(motion.drawRefusal());
        check(qualify(motion,3200,out) && out.qualified && out.coveredDraws==1,"scene C: a frame whose every draw was refused still qualifies");
        const auto px=read(out);
        const auto c=census(px,1,nullptr,nullptr);
        check(c.owned>2000 && c.noSample==c.owned,
              "scene C: with no draw in the map its marked pixels still carry the GPU no-sample (z of -1), not the legacy clear's depth of zero");
        check(motion.stats().coveredOther==1,"scene C: the covered draw is counted under the other causes");
    }

    // ---- D: eight pieces of one mesh, eight identities ----------------------------------------------------------------------------------
    {
        constexpr unsigned pieces=8;
        const float pr=.18f;
        const auto pose=[&](unsigned i,float delta) {Pose p{};p.projection=pr;p.mouse=(-.875f+.25f*i)/pr+delta;return p;};
        const auto delta=[](unsigned i,unsigned step){return .03f*(i+1)*step;};   // piece i is at delta(i, step) mouse units at step
        FlatForegroundMotion motion;FlatForegroundMotion::Output out;
        const auto frameOf=[&](unsigned frame,unsigned step,const unsigned (&order)[pieces]) {
            for(unsigned k=0;k<pieces;++k) {
                const unsigned i=order[k];
                check(piece(motion,pose(i,delta(i,step)),i,i+1,frame,k==0),"scene D: a piece captures");
            }
            check(qualify(motion,frame,out) && out.qualified && out.coveredDraws==0,"scene D: a frame of eight same-mesh draws qualifies with none covered");
            return read(out);
        };
        const unsigned inOrder[pieces]={0,1,2,3,4,5,6,7};
        frameOf(3300,0,inOrder);
        auto px=frameOf(3301,1,inOrder);
        for(unsigned i=0;i<pieces;++i) {
            Motion m{-(delta(i,1)-delta(i,0)),pr};
            const auto c=census(px,i+1,+motionX,&m);
            char label[160];std::snprintf(label,sizeof(label),"scene D: piece %u of %u keeps its own history and its own motion in the steady frame",i,pieces);
            check(c.owned>300 && c.matched==c.owned && c.wrongMotion==0,label);
        }
        check(motion.stats().windowed>0,"scene D: the pieces past the fourth were handed windows of priors");
        const unsigned swapped[pieces]={0,1,2,4,3,5,6,7};
        px=frameOf(3302,2,swapped);
        for(unsigned i=0;i<pieces;++i) {
            Motion m{-(delta(i,2)-delta(i,1)),pr};
            const auto c=census(px,i+1,+motionX,&m);
            char label[160];std::snprintf(label,sizeof(label),"scene D: piece %u keeps its history with two neighbours drawn in the other order",i);
            check(c.owned>300 && c.matched==c.owned && c.wrongMotion==0,label);
        }
        // Reversed, a piece's prior can lie outside its window of four: it is then refused locally. None takes another piece's motion.
        const unsigned reversed[pieces]={7,6,5,4,3,2,1,0};
        px=frameOf(3303,3,reversed);
        unsigned refusedPieces=0,keptPieces=0;
        for(unsigned i=0;i<pieces;++i) {
            Motion m{-(delta(i,3)-delta(i,2)),pr};
            const auto c=census(px,i+1,+motionX,&m);
            char label[160];std::snprintf(label,sizeof(label),"scene D: piece %u of the reversed frame never takes another piece's motion",i);
            check(c.owned>300 && c.wrongMotion==0 && c.noSample==0 && c.matched+c.rejected==c.owned,label);
            if(c.matched==c.owned)++keptPieces;else ++refusedPieces;
        }
        check(refusedPieces>0 && keptPieces>0,"scene D: the window is a window: some reversed pieces keep their history, some are refused locally");
    }

    // ---- E: no owner mark, no cover --------------------------------------------------------------------------------------------------------
    {
        FlatForegroundMotion motion;FlatForegroundMotion::Output out;
        check(piece(motion,place(-.4f,.3f),0,1,3400,true),"scene E: the draw before the cap captures");
        sameKeyFillers(motion,edvr::AnimatedVertexHistory::maxExtendedOccurrences-1,11,3400);
        check(!piece(motion,place(.4f,.3f),1,2,3400,false) && motion.drawRefusal(),"scene E: the draw over the cap is refused");
        motion.fail(motion.drawRefusal());   // the runtime's path when the marker did not start
        check(!qualify(motion,3400,out) && !out.qualified && out.refusal && !std::strcmp(out.refusal,"occurrence-cap"),
              "scene E: a draw nothing marks leaves the frame refused, with the draw's reason");
    }
}
