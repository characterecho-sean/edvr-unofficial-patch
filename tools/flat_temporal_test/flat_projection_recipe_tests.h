#pragma once
#include "../../src/d3d11/flat_projection_recipes.h"
inline int flatProjectionRecipeTests() {
    using namespace edvr;int failures=0;
    auto expect=[&](bool ok,const char* name){if(!ok){std::printf("FAIL: projection recipes %s\n",name);++failures;}};
    const auto deferred=flatProjectionDrawRecipes(0x7E38A6AA1269C901ull,0);
    expect(deferred.count==1 && deferred.requests[0].slot==2 && deferred.requests[0].patchCount==2 &&
        deferred.requests[0].patches[0].layout==FlatProjectionPatchLayout::ForwardDp4 &&
        deferred.requests[0].patches[1].layout==FlatProjectionPatchLayout::InverseUvRay,"deferred forward and inverse travel together");
    expect(flatProjectionDrawRecipes(0xDEF19B035D5EDEDCull,0xCB95394B50D737D6ull).count==0,"view-Z conversion has no projection consumer");
    expect(flatProjectionDrawRecipes(0x5453D19B6D362364ull,0).count==0 &&
        flatProjectionDrawRecipes(0x5453D19B6D362364ull,0xF321711CF47EB970ull).requests[0].patches[0].byteOffset==6*16,"embedded HUD requires captured pair");
    const auto glare=flatProjectionDrawRecipes(0x94D5C556DFD6D705ull,0x912477AEF6958379ull);
    expect(glare.count==1 && glare.requests[0].slot==0 && glare.requests[0].patches[0].byteOffset==4*16,"glare leaves viewport and extent constants untouched");
    const auto screen=flatProjectionDrawRecipes(0,0x7EAC71963E66C5FEull);
    expect(screen.count==1 && screen.requests[0].stage==FlatProjectionStage::Pixel && screen.requests[0].slot==2,"screen inverse uses independent pixel binding");
    const auto sky=flatProjectionDrawRecipes(0xF8FA801F2CB1E27Cull,0x84965D3C050FB01Bull);
    expect(sky.count==1 && sky.requests[0].patches[0].layout==FlatProjectionPatchLayout::InverseClip &&
        sky.requests[0].patches[0].byteOffset==11*16,"sky inverse recipe retained");
    struct ObservedPair { uint64_t vs,ps; UINT slot; FlatProjectionPatchLayout layout; uint32_t row; };
    // All 33 projection-bearing unknown pairs in the verified Epic f1ea02fe census.
    const ObservedPair observed[] = {
        {0xBFE51414CC3024B4ull,0xDB79AE788E049DFDull,1,FlatProjectionPatchLayout::ForwardColumns,270},
        {0xEB5234DB6ADB491Dull,0xB7D50283329322C3ull,1,FlatProjectionPatchLayout::ForwardColumns,270},
        {0x7B0DC42D383F694Cull,0x0DF03E64DF9DBEF1ull,1,FlatProjectionPatchLayout::ForwardColumns,270},
        {0xC53F124D7D591509ull,0x41DDAD26FE2034A7ull,1,FlatProjectionPatchLayout::ForwardColumns,270},
        {0x87FCE198053AA4B9ull,0x50A516DA6DFE2A7Cull,1,FlatProjectionPatchLayout::ForwardColumns,270},
        {0xDE545DC8EE4FBB87ull,0x91F8937EDA723663ull,1,FlatProjectionPatchLayout::ForwardColumns,270},
        {0x5DA53D8B0133341Eull,0xE23C45251B7ECDFEull,1,FlatProjectionPatchLayout::ForwardColumns,270},
        {0x5DA53D8B0133341Eull,0xBF0CE0DA543D491Full,1,FlatProjectionPatchLayout::ForwardColumns,270},
        {0x4A0748B67A27F71Eull,0x1E1E004BD5442A7Aull,1,FlatProjectionPatchLayout::ForwardColumns,270},
        {0x4A0748B67A27F71Eull,0x4411D5EF62CDC66Aull,1,FlatProjectionPatchLayout::ForwardColumns,270},
        {0x39CC20727A27FD17ull,0xBFD75730622BB17Cull,1,FlatProjectionPatchLayout::ForwardColumns,270},
        {0x68DDDEF04D9894AFull,0x06332CA168B6DA63ull,1,FlatProjectionPatchLayout::ForwardColumns,270},
        {0xF7A6E916F14A3B1Aull,0x06332CA168B6DA63ull,1,FlatProjectionPatchLayout::ForwardColumns,270},
        {0xD95905C18B7FAD93ull,0x5BCB6B95BE7C0700ull,1,FlatProjectionPatchLayout::ForwardColumns,270},
        {0xD1281DF454A153ADull,0x97DBC87FCAA429C4ull,1,FlatProjectionPatchLayout::ForwardColumns,270},
        {0x025B4B9FF54622EDull,0xC5A5C7E8216CB9AFull,1,FlatProjectionPatchLayout::ForwardColumns,270},
        {0x6DB587D29F43A9A6ull,0xB2DE0A41A4C2B4F5ull,1,FlatProjectionPatchLayout::ForwardColumns,270},
        {0x0B5981F2AEF7D80Aull,0xC5A5C7E8216CB9AFull,1,FlatProjectionPatchLayout::ForwardColumns,270},
        {0x8C091FFD08644E02ull,0x4E4FF61E8A08FC7Eull,1,FlatProjectionPatchLayout::ForwardColumns,270},
        {0x5559BD94B6852E83ull,0xEA02FAC2BD6C643Cull,1,FlatProjectionPatchLayout::ForwardColumns,270},
        {0x5559BD94B6852E83ull,0xE95634B0F61D218Full,1,FlatProjectionPatchLayout::ForwardColumns,270},
        {0x9F4BBCFCD3B68BC9ull,0x2BAE3742FEB916D9ull,1,FlatProjectionPatchLayout::ForwardColumns,270},
        {0x5E417E9DF2E7F9E6ull,0xBD801F2FB02522EBull,0,FlatProjectionPatchLayout::ForwardDp4,4},
        {0x88DCF1164C640EC3ull,0x494506A63091DF8Cull,0,FlatProjectionPatchLayout::ForwardDp4,4},
        {0xE904D334BC8B11EAull,0x095030F27D2C362Aull,0,FlatProjectionPatchLayout::ForwardDp4,4},
        {0xB7790CBFC6554097ull,0x8DEF46452FA459F5ull,0,FlatProjectionPatchLayout::ForwardDp4,4},
        {0x81216C77F90DEDD6ull,0xA2965EC2931A39C8ull,0,FlatProjectionPatchLayout::ForwardDp4,4},
        {0x0357BBB2DEE43C1Full,0x81812EF97FB4A361ull,2,FlatProjectionPatchLayout::ForwardDp4,10},
        {0x8289669D93A18C1Dull,0xC6E6E419DA9F6FADull,2,FlatProjectionPatchLayout::ForwardDp4,10},
        {0x963B52C73B4143ACull,0x50364C9D994141D5ull,2,FlatProjectionPatchLayout::ForwardDp4,10},
        {0xDF3503CD07F9B10Cull,0x8C08EB252B0F6095ull,2,FlatProjectionPatchLayout::ForwardColumns,6},
        {0xB932058F26B76691ull,0x65861AC394D51526ull,2,FlatProjectionPatchLayout::ForwardColumns,6},
        {0x9611A454527F7FEBull,0x1E1C49DC51C0E509ull,2,FlatProjectionPatchLayout::ForwardColumns,7},
    };
    expect(sizeof(observed)/sizeof(observed[0])==33,"Epic projection pair census size");
    for (const auto& pair : observed) {
        const auto recipe=flatProjectionDrawRecipes(pair.vs,pair.ps);
        expect(recipe.count==1 && recipe.requests[0].stage==FlatProjectionStage::Vertex &&
            recipe.requests[0].slot==pair.slot && recipe.requests[0].patchCount==1 &&
            recipe.requests[0].patches[0].layout==pair.layout &&
            recipe.requests[0].patches[0].byteOffset==pair.row*16,"Epic exact pair has measured matrix recipe");
        expect(flatProjectionDrawRecipes(pair.vs,pair.ps^1ull).count==0,"unobserved PS companion rejected");
    }
    const uint64_t unchanged[][2] = {
        {0xFC1193AFFC596F74ull,0x258B95AC99520C1Full},
        {0xE8FDC0D92EEBA6D7ull,0x258B95AC99520C1Full},
        {0x53211E8C072CD02Eull,0xB403F48CB35D9739ull},
        {0xCFA91824129ECBBCull,0xFCFAD73924BF45B9ull},
        {0x525D47E3D5E2EFF4ull,0xF0BAE053476F8730ull},
    };
    for (const auto& pair : unchanged) {
        expect(flatProjectionDrawUnchanged(pair[0],pair[1]) &&
            flatProjectionDrawRecipes(pair[0],pair[1]).count==0,"inert Epic pair explicitly unchanged");
        expect(!flatProjectionDrawUnchanged(pair[0],pair[1]^1ull),"inert classification requires exact PS");
    }
    // Complete ce715126 unknown-pair census. F512's missing PS arrived in
    // the 6e9bde74 capture below, and 4361's PS in ad7607c6; CC2B remains unknown.
    const ObservedPair latest[] = {
        {0x84F6596FAF22CCFAull,0,1,FlatProjectionPatchLayout::ForwardColumns,270},
        {0xDE545DC8EE4FBB87ull,0x03B17F89B31C4788ull,1,FlatProjectionPatchLayout::ForwardColumns,270},
        {0x124D7F3F649138D4ull,0x8085AE8DD1906CDCull,1,FlatProjectionPatchLayout::ForwardColumns,270},
        {0x361CD4B7FF213A01ull,0xFA7411BF7E4C4088ull,1,FlatProjectionPatchLayout::ForwardColumns,270},
        {0x3064D7F445192FDDull,0xCCDB2A91490F8755ull,1,FlatProjectionPatchLayout::ForwardColumns,270},
        {0x9AEC596A2B036EA6ull,0x3789CA2062E196FBull,1,FlatProjectionPatchLayout::ForwardColumns,270},
        {0xEB787F983BC1F5A3ull,0x8591A46B10497299ull,1,FlatProjectionPatchLayout::ForwardColumns,270},
        {0x8106B439CD518CFCull,0x3BC6B5B66B852BB5ull,1,FlatProjectionPatchLayout::ForwardColumns,270},
        {0xAFBCD3ADB9092F78ull,0x2BAE3742FEB916D9ull,1,FlatProjectionPatchLayout::ForwardColumns,270},
        {0xCFC9094F7EEE21E2ull,0xFD77C2EBFFAC7D9Cull,0,FlatProjectionPatchLayout::ForwardDp4,4},
        {0xCFC9094F7EEE21E2ull,0xAE3D10F3D40D688Cull,0,FlatProjectionPatchLayout::ForwardDp4,4},
        {0xCFC9094F7EEE21E2ull,0x4DBE9258D3C4D3DAull,0,FlatProjectionPatchLayout::ForwardDp4,4},
        {0x41E245D488BFE83Eull,0x6EF82262EB12A037ull,0,FlatProjectionPatchLayout::ForwardDp4,4},
        {0xB12F7A618E1BDE98ull,0x42AC0CACC9CDF72Bull,0,FlatProjectionPatchLayout::ForwardDp4,4},
        {0x203DF51758AADC4Dull,0xEEAAC839A9F09448ull,0,FlatProjectionPatchLayout::ForwardDp4,4},
        {0x5EAFFCD01B97D0C4ull,0xDD371C57C9093BB8ull,0,FlatProjectionPatchLayout::ForwardDp4,4},
        {0x46546443FD3C3F88ull,0xAD050E528C0E8B17ull,0,FlatProjectionPatchLayout::ForwardDp4,4},
        {0x95D01BA609BF7500ull,0x067CBE05E7EF7F32ull,0,FlatProjectionPatchLayout::ForwardDp4,4},
        {0xE508648660A352B2ull,0x63ABD86359B57D01ull,0,FlatProjectionPatchLayout::ForwardDp4,4},
        {0x381D80284FE236F8ull,0x72BBDA3D4CD3E39Bull,0,FlatProjectionPatchLayout::ForwardDp4,4},
        {0x71DD8B8B09060A81ull,0x2D037A047171BF3Bull,0,FlatProjectionPatchLayout::ForwardDp4,4},
        {0x939D01F28D1FEEA8ull,0xC5DF9CC943476289ull,0,FlatProjectionPatchLayout::ForwardDp4,4},
        {0x5C1D8EF529324A22ull,0xC49F999F7D3C801Dull,0,FlatProjectionPatchLayout::ForwardDp4,4},
        {0x820E5C131B99361Dull,0x6EAA86EFE135B2D4ull,0,FlatProjectionPatchLayout::ForwardDp4,4},
        {0xB75A6FF2CA9FA5D6ull,0xD56F859BE4781431ull,0,FlatProjectionPatchLayout::ForwardDp4,4},
        {0x24214E7C45496BE0ull,0x0C8FCDB6A3BECCE6ull,2,FlatProjectionPatchLayout::ForwardColumns,8},
        {0xA1B7CFCD0BE7493Eull,0x2DB678B6B558B604ull,2,FlatProjectionPatchLayout::ForwardDp4,10},
        {0xCE24A73943632F55ull,0x1F64463B15189104ull,2,FlatProjectionPatchLayout::ForwardDp4,10},
        // Complete menu VS/PS blobs from the Epic 5c78c34d flight.
        {0x61AE8EB05FDC18DDull,0x4504BC268E109C31ull,1,FlatProjectionPatchLayout::ForwardColumns,270},
        {0x0357BBB2DEE43C1Full,0x222188632125D14Bull,2,FlatProjectionPatchLayout::ForwardDp4,10},
        {0x4EF6DDB075A927FAull,0x098C0764D28FC42Cull,0,FlatProjectionPatchLayout::ForwardDp4,4},
        {0x95D01BA609BF7500ull,0xF10792B40AE3ED42ull,0,FlatProjectionPatchLayout::ForwardDp4,4},
        // Complete automatic capture from Epic 85bf7652.
        {0x0C4E76889907B963ull,0xA90825082F36756Eull,1,FlatProjectionPatchLayout::ForwardColumns,270},
        {0x989E043933A369ABull,0xCE844D87026C684Cull,0,FlatProjectionPatchLayout::ForwardDp4,4},
        // Projected effect from the subsequent Epic 85d590e0 run.
        {0x2D8263CC54D55398ull,0x89B662E266E5D73Eull,0,FlatProjectionPatchLayout::ForwardDp4,4},
    };
    expect(sizeof(latest)/sizeof(latest[0])==35,"latest supported ordinary pair census size");
    FlatProjectionJitter jitter{};
    expect(flatProjectionJitter(.375f,-.25f,1280,720,jitter),"recipe pixel offset constructed");
    for (const auto& pair : latest) {
        const auto recipe=flatProjectionDrawRecipes(pair.vs,pair.ps);
        expect(recipe.count==1 && recipe.requests[0].stage==FlatProjectionStage::Vertex &&
            recipe.requests[0].slot==pair.slot && recipe.requests[0].patchCount==1 &&
            recipe.requests[0].patches[0].layout==pair.layout &&
            recipe.requests[0].patches[0].byteOffset==pair.row*16,"latest exact pair patches measured clip span");
        expect(flatProjectionDrawRecipes(pair.vs,pair.ps^1ull).count==0,"latest companion identity required");
        // For the exact selected layout, a test point moves by the requested
        // subpixel offset; depth and W stay identical. This tests the algebra
        // that makes each recorded span a projection rather than just a hash.
        if (pair.layout==FlatProjectionPatchLayout::ForwardColumns) {
            float rows[4][4]={{1,0,0,0},{0,1,0,0},{0,0,1,0},{2,-3,4,1}};
            expect(flatJitterForwardColumns(rows,jitter) &&
                std::abs((rows[0][0]*.5f+rows[1][0]*-.25f+rows[2][0]+rows[3][0])-(2.5f+jitter.ndcX))<.00001f &&
                std::abs((rows[0][1]*.5f+rows[1][1]*-.25f+rows[2][1]+rows[3][1])-(-3.25f+jitter.ndcY))<.00001f &&
                rows[3][2]==4 && rows[3][3]==1,"column clip shift preserves depth and W");
        } else {
            float rows[4][4]={{1,0,0,2},{0,1,0,-3},{0,0,1,4},{0,0,0,1}};
            expect(flatJitterForwardDp4(rows,jitter) &&
                std::abs(rows[0][3]-(2+jitter.ndcX))<.00001f &&
                std::abs(rows[1][3]-(-3+jitter.ndcY))<.00001f &&
                rows[2][3]==4 && rows[3][3]==1,"dp4 clip shift preserves depth and W");
        }
    }
    // Decal VS 0C4E instructions 37..45 emit both raster clip XYZW and
    // clip XYW for PS A908 instructions 0..2. Exercise perspective W != 1:
    // both must move by the same pixel offset, without moving clip Z/W.
    float decalRows[4][4]={{2,0,0,.25f},{0,3,0,-.5f},{0,0,1,1},{.5f,-.25f,.125f,2}};
    const float decalPoint[4]={.2f,-.3f,4,1};
    float decalOld[4]{}, decalNew[4]{};
    for (int col=0;col<4;++col)
        for (int row=0;row<4;++row) decalOld[col]+=decalPoint[row]*decalRows[row][col];
    expect(flatJitterForwardColumns(decalRows,jitter),"decal shared clip matrix patched");
    for (int col=0;col<4;++col)
        for (int row=0;row<4;++row) decalNew[col]+=decalPoint[row]*decalRows[row][col];
    const float decalOldUV[2]={.5f*decalOld[0]/decalOld[3]+.5f,-.5f*decalOld[1]/decalOld[3]+.5f};
    const float decalVarying[3]={decalNew[0],decalNew[1],decalNew[3]};
    const float decalDepthUV[2]={.5f*decalVarying[0]/decalVarying[2]+.5f,-.5f*decalVarying[1]/decalVarying[2]+.5f};
    expect(std::abs(decalDepthUV[0]-decalOldUV[0]-jitter.uvX)<.000001f &&
        std::abs(decalDepthUV[1]-decalOldUV[1]-jitter.uvY)<.000001f &&
        std::abs(decalNew[0]/decalNew[3]-decalOld[0]/decalOld[3]-jitter.ndcX)<.000001f &&
        std::abs(decalNew[1]/decalNew[3]-decalOld[1]/decalOld[3]-jitter.ndcY)<.000001f &&
        decalNew[2]==decalOld[2] && decalNew[3]==decalOld[3],
        "decal raster and depth lookup receive identical perspective jitter");
    // Projected effect 2D82 emits CB0 dot-product clip XYW to both raster
    // and PS89B6's depth sampler. Its depth comparison uses W, not Z.
    float effectRows[4][4]={{2,0,0,.5f},{0,3,0,-.25f},{0,0,1,.125f},{.25f,-.5f,1,2}};
    float effectOld[4]{}, effectNew[4]{};
    for (int row=0;row<4;++row)
        for (int col=0;col<4;++col) effectOld[row]+=effectRows[row][col]*decalPoint[col];
    expect(flatJitterForwardDp4(effectRows,jitter),"projected effect clip matrix patched");
    for (int row=0;row<4;++row)
        for (int col=0;col<4;++col) effectNew[row]+=effectRows[row][col]*decalPoint[col];
    // Mirror PS instructions 0..2, including the separate Y flip.
    const float effectOldUV[2]={.5f*effectOld[0]/effectOld[3]+.5f,1-(.5f*effectOld[1]/effectOld[3]+.5f)};
    const float effectDepthUV[2]={.5f*effectNew[0]/effectNew[3]+.5f,1-(.5f*effectNew[1]/effectNew[3]+.5f)};
    expect(std::abs(effectDepthUV[0]-effectOldUV[0]-jitter.uvX)<.000001f &&
        std::abs(effectDepthUV[1]-effectOldUV[1]-jitter.uvY)<.000001f &&
        std::abs(effectNew[0]/effectNew[3]-effectOld[0]/effectOld[3]-jitter.ndcX)<.000001f &&
        std::abs(effectNew[1]/effectNew[3]-effectOld[1]/effectOld[3]-jitter.ndcY)<.000001f &&
        effectNew[2]==effectOld[2] && effectNew[3]==effectOld[3],
        "projected effect raster/depth UV align and depth-occlusion W stays invariant");
    const auto oldCab=flatProjectionDrawRecipes(0x98397963AAEC45D3ull,0xCAB49794BB439D03ull);
    expect(oldCab.count==1 && oldCab.requests[0].slot==1 && oldCab.requests[0].patchCount==1 &&
        oldCab.requests[0].patches[0].layout==FlatProjectionPatchLayout::ForwardColumns &&
        oldCab.requests[0].patches[0].byteOffset==270*16 &&
        flatProjectionDrawRecipes(0x98397963AAEC45D3ull,0xCAB49794BB439D02ull).count==0,
        "earlier pair admitted after exact Epic CAB pixel blob capture");
    // B75's CB0[4,5,7] is used to project an anchor, then divided before
    // building its occlusion sample centers and nonlinear billboard shape.
    // Test that anchor/centers track jitter and the comparison W is intact;
    // the resulting quad is not promised a uniform screen translation.
    float anchor[4][4]={{2,0,0,0},{0,2,0,0},{0,0,1,0},{0,0,0,2}};
    const float anchorX=4,anchorY=-2,anchorZ=3,anchorW=2;
    expect(flatJitterForwardDp4(anchor,jitter),"billboard anchor matrix patched");
    const float shiftedX=anchor[0][0]*2+anchor[0][3];
    const float shiftedY=anchor[1][1]*-1+anchor[1][3];
    const float shiftedZ=anchor[2][2]*3+anchor[2][3];
    const float shiftedW=anchor[3][3];
    expect(std::abs(shiftedX/shiftedW-anchorX/anchorW-jitter.ndcX)<.00001f &&
        std::abs(shiftedY/shiftedW-anchorY/anchorW-jitter.ndcY)<.00001f &&
        std::abs((.5f+.5f*shiftedX/shiftedW)-(.5f+.5f*anchorX/anchorW)-jitter.uvX)<.00001f &&
        std::abs((.5f+.5f*shiftedY/shiftedW)-(.5f+.5f*anchorY/anchorW)+jitter.uvY)<.00001f &&
        shiftedZ==anchorZ && shiftedW==anchorW,
        "billboard divided anchor and depth sample centers follow jitter with Z/W intact");
    const auto branching=flatProjectionDrawRecipes(0x1F17BF54DB6EE407ull,0xA75C1DB6562B8CA7ull);
    expect(branching.count==1 && branching.requests[0].slot==1 && branching.requests[0].patchCount==2 &&
        branching.requests[0].patches[0].byteOffset==41*16 &&
        branching.requests[0].patches[1].byteOffset==45*16 &&
        branching.requests[0].patches[1].layout==FlatProjectionPatchLayout::ForwardColumns &&
        flatProjectionDrawRecipes(0x1F17BF54DB6EE407ull,0xA75C1DB6562B8CA6ull).count==0,
        "both conditional clip branches patched together");
    for (const auto& patch : branching.requests[0].patches) {
        if (patch.byteOffset!=41*16 && patch.byteOffset!=45*16) continue;
        float branchRows[4][4]={{1,0,0,0},{0,1,0,0},{0,0,1,0},{7,-2,3,1}};
        expect(flatJitterForwardColumns(branchRows,jitter) &&
            std::abs(branchRows[3][0]-(7+jitter.ndcX))<.00001f &&
            std::abs(branchRows[3][1]-(-2+jitter.ndcY))<.00001f &&
            branchRows[3][2]==3 && branchRows[3][3]==1,
            "either conditional clip branch shifts xy and preserves zw");
    }
    const uint64_t unresolved[][2] = {
        {0xCC2BA2E2A927CBD3ull,0x8A7FB2DB7A33279Eull}, // both blobs unavailable
    };
    for (const auto& pair : unresolved)
        expect(flatProjectionDrawRecipes(pair[0],pair[1]).count==0 &&
            !flatProjectionDrawUnchanged(pair[0],pair[1]),"unproven latest pair stays unknown");
    // Epic 6e9bde74: all captured VS blobs below feed SV_Position through
    // CB1[270..273], and their exact PS blobs have no projection/inverse span.
    const uint64_t epic6e9b[][2] = {
        {0xF516BF0201303B87ull,0xB40B0462256E31C2ull},
        {0x318693134643131Aull,0x4EBA794D13603735ull},
        {0xD99AFDC250D19A3Full,0xE86271E464CCDC1Dull},
        {0x114AF608F86D9ED8ull,0xA17504A2627767F2ull},
        {0x01DA82F8D0EA0C99ull,0x2C3ADCD2FCD47298ull},
        {0x6D2158CFE759A3BEull,0xBD59C4EF0DA3B1E2ull},
        {0xA4A19FAF8D08E1D6ull,0xBE3EA29C554ABFF3ull},
        {0x33A5025C48FC8259ull,0x6B8C26FAC5558C6Cull},
        {0xA8E4D93B8B294505ull,0x75D12D8561BA8525ull},
        {0xA6A39338C06E03A1ull,0xED91F94EC94FA5C2ull},
        {0x617C6E44A034E0C2ull,0x627FEC646836683Full},
        {0xB43A856E285815E3ull,0x702C3974A260DE14ull},
        {0xF512712C40D93C12ull,0xD0B9213C1F248335ull},
        {0x359BF8FF5CFAA4C3ull,0x92FF8499ED345759ull},
        {0xEFE42AC6142C1815ull,0x29D8624FD690277Full},
        {0x01A029C7DBD48554ull,0x130FC0A72CE36CDBull},
        {0xA1CE8A95F0D23260ull,0xC6CD9AEBDEA803EEull},
        {0x2684F02B9B0BB0DEull,0x2376A8D9AA874372ull},
    };
    expect(sizeof(epic6e9b)/sizeof(epic6e9b[0])==18,"Epic 6e9b projection pair count");
    for (const auto& pair : epic6e9b) {
        const auto recipe=flatProjectionDrawRecipes(pair[0],pair[1]);
        expect(recipe.count==1 && recipe.requests[0].stage==FlatProjectionStage::Vertex &&
            recipe.requests[0].slot==1 && recipe.requests[0].patchCount==1 &&
            recipe.requests[0].patches[0].layout==FlatProjectionPatchLayout::ForwardColumns &&
            recipe.requests[0].patches[0].byteOffset==270*16,
            "Epic 6e9b exact pair patches measured SV_Position columns");
        expect(flatProjectionDrawRecipes(pair[0],pair[1]^1ull).count==0,
            "Epic 6e9b companion identity required");
    }
    // Epic 22fe85d2: all five unknown pairs seen after display changes.
    // Only the measured vertex span changes; no pixel binding is requested.
    const ObservedPair epic22fe[] = {
        {0x71DD9863DCFC0986ull,0x43E5E6EB67AC751Bull,1,FlatProjectionPatchLayout::ForwardColumns,270},
        {0x3530A6FD15EDE145ull,0x13B224F056C39D85ull,1,FlatProjectionPatchLayout::ForwardColumns,270},
        {0x19F70CE80DA3242Bull,0xC8FBD8A982C0729Cull,1,FlatProjectionPatchLayout::ForwardColumns,270},
        {0x9FFA5D5E79F04873ull,0x8134D09E3462E904ull,0,FlatProjectionPatchLayout::ForwardDp4,4},
        {0xA52ECB960783BB35ull,0x84965D3C050FB01Bull,2,FlatProjectionPatchLayout::InverseClip,11},
    };
    for (const auto& pair : epic22fe) {
        const auto recipe=flatProjectionDrawRecipes(pair.vs,pair.ps);
        expect(recipe.count==1 && recipe.requests[0].stage==FlatProjectionStage::Vertex &&
            recipe.requests[0].slot==pair.slot && recipe.requests[0].patchCount==1 &&
            recipe.requests[0].patches[0].layout==pair.layout &&
            recipe.requests[0].patches[0].byteOffset==pair.row*16,
            "Epic 22fe measured scene or sky projection span");
        expect(flatProjectionDrawRecipes(pair.vs,pair.ps^1ull).count==0 &&
            flatProjectionDrawRecipes(pair.vs,0).count==0 &&
            flatProjectionDrawRecipes(pair.vs^1ull,pair.ps).count==0,
            "Epic 22fe projection requires both captured shader identities");
        expect(!flatProjectionDrawUnchanged(pair.vs,pair.ps),
            "Epic 22fe projection consumer cannot bypass jitter as unchanged");
    }
    // Epic ad7607c6: complete station/hangar/concourse unknown-pair census.
    // The unchanged screen composite is tested separately from these 30.
    const ObservedPair epicStation[] = {
        {0xEB5234DB6ADB491Dull,0xDC603C35BBE74B31ull,1,FlatProjectionPatchLayout::ForwardColumns,270},
        {0x637C27B86091BD60ull,0x48D45E37C62839E9ull,1,FlatProjectionPatchLayout::ForwardColumns,270},
        {0xC171BD0C4B585221ull,0x6855D1919FC5E0C0ull,1,FlatProjectionPatchLayout::ForwardColumns,270},
        {0x436193B352A2897Eull,0x51EE1F922FD220B0ull,1,FlatProjectionPatchLayout::ForwardColumns,270},
        {0x4D24A7A6C2D12733ull,0xB70DF49F678E806Full,1,FlatProjectionPatchLayout::ForwardColumns,270},
        {0xDE545DC8EE4FBB87ull,0xA6070F9DD1CFB601ull,1,FlatProjectionPatchLayout::ForwardColumns,270},
        {0x98397963AAEC45D3ull,0x8717694A527EC745ull,1,FlatProjectionPatchLayout::ForwardColumns,270},
        {0xD005EBB14A22EA0Eull,0x302226F2D8C0938Aull,0,FlatProjectionPatchLayout::ForwardDp4,4},
        {0xD005EBB14A22EA0Eull,0xE92C14AA3E51C743ull,0,FlatProjectionPatchLayout::ForwardDp4,4},
        {0xE308565BF97FDE0Bull,0x0544F1CC95FD1F12ull,1,FlatProjectionPatchLayout::ForwardColumns,270},
        {0x5B0068AF5630F96Bull,0xA5E2331517988BD8ull,1,FlatProjectionPatchLayout::ForwardColumns,270},
        {0x24DE25E496342EB8ull,0x1A53D2791C12CE92ull,2,FlatProjectionPatchLayout::ForwardDp4,10},
        {0x1C5062229AA40CE4ull,0x2519C9050946D545ull,1,FlatProjectionPatchLayout::ForwardColumns,270},
        {0xABF539A8C5CCC1B7ull,0x3F71C89CA34DF25Bull,1,FlatProjectionPatchLayout::ForwardColumns,270},
        {0x2B3F53DDA00256E2ull,0xB2DE0A41A4C2B4F5ull,1,FlatProjectionPatchLayout::ForwardColumns,270},
        {0x38470D38E07CBDEBull,0x0A80DFD89B15A05Bull,0,FlatProjectionPatchLayout::ForwardDp4,4},
        {0x6D8886012A4C6785ull,0x6F3252AB8579C1E3ull,0,FlatProjectionPatchLayout::ForwardDp4,4},
        {0xB018D143700AB803ull,0xB403F48CB35D9739ull,0,FlatProjectionPatchLayout::ForwardDp4,4},
        {0x899165B9EE284E74ull,0x26EA0826BD6824E6ull,1,FlatProjectionPatchLayout::ForwardColumns,270},
        {0xB121A79E457669E8ull,0x777BF099CBAA7C50ull,2,FlatProjectionPatchLayout::ForwardDp4,10},
        {0xA7339D1F8A5AC0D0ull,0xD3891373E13BAD40ull,2,FlatProjectionPatchLayout::ForwardDp4,10},
        {0xC2208C162D010083ull,0x0000000000000000ull,1,FlatProjectionPatchLayout::ForwardColumns,270},
        {0x44C290CC444D1EBEull,0x3154942271AD5810ull,1,FlatProjectionPatchLayout::ForwardColumns,270},
        {0x44C290CC444D1EBEull,0xFD32C5433BD4C015ull,1,FlatProjectionPatchLayout::ForwardColumns,270},
        {0xEB686C4180DFC6A6ull,0xB11CD77D729C2AEEull,1,FlatProjectionPatchLayout::ForwardColumns,270},
        {0x0B71713BCDE4B6C0ull,0xFA7411BF7E4C4088ull,1,FlatProjectionPatchLayout::ForwardColumns,270},
        {0xAFFEF0187F1EBC9Full,0x41152F82C6E8BE1Full,2,FlatProjectionPatchLayout::InverseScreenRay,41},
        {0xA1B7CFCD0BE7493Eull,0x992DE24C01E04A27ull,2,FlatProjectionPatchLayout::ForwardDp4,10},
        {0x889A5279E68F0672ull,0xF70549D991FF0E9Bull,1,FlatProjectionPatchLayout::ForwardColumns,270},
        {0x76ED1E4F8C72C26Eull,0x7ECF7C83FD5AD373ull,0,FlatProjectionPatchLayout::ForwardDp4,4},
    };
    expect(sizeof(epicStation)/sizeof(epicStation[0])==30,
        "station projection census complete");
    for (size_t i=0;i<sizeof(epicStation)/sizeof(epicStation[0]);++i) {
        const auto& pair=epicStation[i];
        const auto recipe=flatProjectionDrawRecipes(pair.vs,pair.ps);
        expect(recipe.count==1 && recipe.requests[0].stage==FlatProjectionStage::Vertex &&
            recipe.requests[0].slot==pair.slot && recipe.requests[0].patchCount==1 &&
            recipe.requests[0].patches[0].layout==pair.layout &&
            recipe.requests[0].patches[0].byteOffset==pair.row*16,
            "station exact pair patches only the measured vertex matrix span");
        expect(flatProjectionDrawRecipes(pair.vs,pair.ps^1ull).count==0 &&
            flatProjectionDrawRecipes(pair.vs^1ull,pair.ps).count==0,
            "station projection requires both captured shader identities");
        if (pair.ps)
            expect(flatProjectionDrawRecipes(pair.vs,0).count==0,
                "station material recipe does not admit an absent companion");
        expect(!flatProjectionDrawUnchanged(pair.vs,pair.ps),
            "station projected geometry or fullscreen ray cannot bypass jitter");
        for (size_t j=0;j<i;++j)
            expect(pair.vs!=epicStation[j].vs || pair.ps!=epicStation[j].ps,
                "station census has no duplicate exact pairs");
    }
    expect(flatProjectionDrawUnchanged(0xB553BB479B7C0B97ull,0x68ABCB9FEF6CA66Cull) &&
        flatProjectionDrawRecipes(0xB553BB479B7C0B97ull,0x68ABCB9FEF6CA66Cull).count==0 &&
        !flatProjectionDrawUnchanged(0xB553BB479B7C0B97ull,0x68ABCB9FEF6CA66Dull) &&
        !flatProjectionDrawUnchanged(0xB553BB479B7C0B96ull,0x68ABCB9FEF6CA66Cull),
        "station screen composite is unchanged only for its exact pair");
    expect(flatProjectionDrawUnchanged(0x53211E8C072CD02Eull,0xB403F48CB35D9739ull) &&
        !flatProjectionDrawUnchanged(0xB018D143700AB803ull,0xB403F48CB35D9739ull),
        "shared constant-output PS does not make a skinned projected VS inert");
    // Epic 20260925_122208 on-foot hangar/concourse: exact captured pairs,
    // including the skinned HDR draw and the multi-UV hull VS's new companion.
    const ObservedPair epicOnFoot[] = {
        {0x0A298DE7DF833A46ull,0x6FD4C38BA927C8C7ull,1,FlatProjectionPatchLayout::ForwardColumns,270},
        {0xD8FCE3CEA16B9B51ull,0x06AA136E4D58CBA2ull,1,FlatProjectionPatchLayout::ForwardColumns,270},
        {0xBA16062A2EB66F1Full,0x33758387B70944A1ull,0,FlatProjectionPatchLayout::ForwardDp4,4},
        {0x66DE2CADB1F4AE6Bull,0xBBDE4E71FB78528Aull,1,FlatProjectionPatchLayout::ForwardColumns,270},
    };
    for (size_t i=0;i<sizeof(epicOnFoot)/sizeof(epicOnFoot[0]);++i) {
        const auto& pair=epicOnFoot[i];
        const auto recipe=flatProjectionDrawRecipes(pair.vs,pair.ps);
        expect(recipe.count==1 && recipe.requests[0].stage==FlatProjectionStage::Vertex &&
            recipe.requests[0].slot==pair.slot && recipe.requests[0].patchCount==1 &&
            recipe.requests[0].patches[0].layout==pair.layout &&
            recipe.requests[0].patches[0].byteOffset==pair.row*16,
            "on-foot exact pair patches only the measured vertex matrix span");
        expect(flatProjectionDrawRecipes(pair.vs,pair.ps^1ull).count==0 &&
            flatProjectionDrawRecipes(pair.vs^1ull,pair.ps).count==0 &&
            flatProjectionDrawRecipes(pair.vs,0).count==0,
            "on-foot projection requires both captured shader identities");
        expect(!flatProjectionDrawUnchanged(pair.vs,pair.ps),
            "on-foot projected geometry cannot bypass jitter");
        for (size_t j=0;j<i;++j)
            expect(pair.vs!=epicOnFoot[j].vs || pair.ps!=epicOnFoot[j].ps,
                "on-foot census has no duplicate exact pairs");
    }
    const auto screenRay=flatProjectionDrawRecipes(0x4AEC439CEC7FFDCEull,0x87EF79B19297B8C4ull);
    expect(screenRay.count==1 && screenRay.requests[0].stage==FlatProjectionStage::Vertex &&
        screenRay.requests[0].slot==1 && screenRay.requests[0].patchCount==1 &&
        screenRay.requests[0].patches[0].layout==FlatProjectionPatchLayout::InverseScreenRay &&
        screenRay.requests[0].patches[0].byteOffset==144*16 &&
        flatProjectionDrawRecipes(0x4AEC439CEC7FFDCEull,0x87EF79B19297B8C5ull).count==0,
        "Epic screen depth ray uses exact VS CB1[144..147]");
    float screenRows[4][4]={{2,3,4,5},{6,7,8,9},{10,11,12,13},{14,15,16,17}};
    float oldRay[3], shiftedRay[3];
    const float ndcX=.25f, ndcY=-.5f;
    for (int i=0;i<3;++i) oldRay[i]=ndcX*screenRows[0][i]+ndcY*screenRows[1][i]+screenRows[2][i]+screenRows[3][i];
    expect(flatJitterInverseScreenRay(screenRows,jitter),"Epic screen ray origin patched");
    bool rayInvariant=true;
    for (int i=0;i<3;++i) {
        shiftedRay[i]=(ndcX+jitter.ndcX)*screenRows[0][i]+(ndcY+jitter.ndcY)*screenRows[1][i]+screenRows[2][i]+screenRows[3][i];
        rayInvariant &= std::abs(shiftedRay[i]-oldRay[i])<.00001f;
    }
    expect(rayInvariant && screenRows[2][0]==10 && screenRows[2][1]==11 &&
        screenRows[2][2]==12 && screenRows[3][3]==17 &&
        screenRows[0][3]==5 && screenRows[1][3]==9,
        "Epic depth reconstruction ray follows screen jitter; depth and W preserved");
    expect(flatProjectionDispatchRecipes(0x823CC578F5510B24ull,1920,1080).count==0,"offline-only lighting variant is not live-associated");
    const auto lighting=flatProjectionDispatchRecipes(0x5998146D464F5C0Eull,1920,1080);
    expect(lighting.count==1 && lighting.requests[0].stage==FlatProjectionStage::Compute &&
        lighting.requests[0].patches[0].lighting.gridX==16 && lighting.requests[0].patches[0].lighting.gridY==9,"measured lighting recipe requests matching grid");
    return failures;
}
