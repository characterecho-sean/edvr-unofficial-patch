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
    expect(flatProjectionDispatchRecipes(0x823CC578F5510B24ull,1920,1080).count==0,"offline-only lighting variant is not live-associated");
    const auto lighting=flatProjectionDispatchRecipes(0x5998146D464F5C0Eull,1920,1080);
    expect(lighting.count==1 && lighting.requests[0].stage==FlatProjectionStage::Compute &&
        lighting.requests[0].patches[0].lighting.gridX==16 && lighting.requests[0].patches[0].lighting.gridY==9,"measured lighting recipe requests matching grid");
    return failures;
}
