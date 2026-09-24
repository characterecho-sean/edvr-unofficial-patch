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
    expect(flatProjectionDrawRecipes(0x5EAFFCD01B97D0C4ull,0xDD371C57C9093BB8ull).count==0 &&
        !flatProjectionDrawUnchanged(0x5EAFFCD01B97D0C4ull,0xDD371C57C9093BB8ull),"missing-bytecode pair remains unknown");
    expect(flatProjectionDispatchRecipes(0x823CC578F5510B24ull,1920,1080).count==0,"offline-only lighting variant is not live-associated");
    const auto lighting=flatProjectionDispatchRecipes(0x5998146D464F5C0Eull,1920,1080);
    expect(lighting.count==1 && lighting.requests[0].stage==FlatProjectionStage::Compute &&
        lighting.requests[0].patches[0].lighting.gridX==16 && lighting.requests[0].patches[0].lighting.gridY==9,"measured lighting recipe requests matching grid");
    return failures;
}
