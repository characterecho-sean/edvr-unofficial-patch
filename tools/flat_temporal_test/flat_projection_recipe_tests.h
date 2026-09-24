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
    };
    for (const auto& pair : unchanged) {
        expect(flatProjectionDrawUnchanged(pair[0],pair[1]) &&
            flatProjectionDrawRecipes(pair[0],pair[1]).count==0,"inert Epic pair explicitly unchanged");
        expect(!flatProjectionDrawUnchanged(pair[0],pair[1]^1ull),"inert classification requires exact PS");
    }
    // Complete ce715126 unknown-pair census. The three exclusions below have
    // no proven complete clip/PS contract in the captured creation blobs.
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
    };
    expect(sizeof(latest)/sizeof(latest[0])==28,"latest supported ordinary pair census size");
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
        {0x436193B352A2897Eull,0x51EE1F922FD220B0ull}, // PS blob unavailable
        {0xF512712C40D93C12ull,0xD0B9213C1F248335ull}, // PS blob unavailable
        {0xCC2BA2E2A927CBD3ull,0x8A7FB2DB7A33279Eull}, // both blobs unavailable
    };
    for (const auto& pair : unresolved)
        expect(flatProjectionDrawRecipes(pair[0],pair[1]).count==0 &&
            !flatProjectionDrawUnchanged(pair[0],pair[1]),"unproven latest pair stays unknown");
    expect(flatProjectionDispatchRecipes(0x823CC578F5510B24ull,1920,1080).count==0,"offline-only lighting variant is not live-associated");
    const auto lighting=flatProjectionDispatchRecipes(0x5998146D464F5C0Eull,1920,1080);
    expect(lighting.count==1 && lighting.requests[0].stage==FlatProjectionStage::Compute &&
        lighting.requests[0].patches[0].lighting.gridX==16 && lighting.requests[0].patches[0].lighting.gridY==9,"measured lighting recipe requests matching grid");
    return failures;
}
