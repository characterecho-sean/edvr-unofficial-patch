#pragma once
// A DRAW WITH NO HISTORY OF ITS OWN TAKES ITS SIBLINGS' MOTION (design section 104, the pistol's LOD swap): the policy in plain C++
// (src\d3d11\flat_foreground_sibling.h), the 5 s line, and the pins on the wiring of the shaders and of the capture class that runs them.
// tools\weapon_motion_test holds the shaders to this policy on a real device, for every draw of every scene; here are the cases that need no
// device, and the mutation controls that keep the pins honest.
#include "../../src/d3d11/flat_foreground_sibling.h"

inline int flatSiblingPolicyTests() {
    using namespace edvr;
    int failures = 0;
    const auto expect = [&](bool ok, const char* name) {
        if (!ok) { std::printf("FAIL: sibling policy %s\n", name); ++failures; }
    };
    // A donor record: matched, `vertices` vertices of mean (mx, my) spanning [lo, hi] per axis, identity (x, y).
    const auto donor = [](uint32_t vertices, float mx, float my, float lox, float loy, float hix, float hiy, uint32_t x = 7, uint32_t y = 9) {
        SiblingDonor d;
        d.matched = true; d.vertices = vertices; d.identityX = x; d.identityY = y & kFlatSiblingIdentityMask;
        d.mean[0] = mx; d.mean[1] = my; d.lo[0] = lox; d.lo[1] = loy; d.hi[0] = hix; d.hi[1] = hiy;
        return d;
    };
    const uint32_t own[4] = {7, 9, 1, 0};
    const auto lone = [] { SiblingDonor d; return d; };   // the draw itself: no match

    // -- the cases the pass decides --
    {
        const SiblingDonor draws[2] = {donor(300, 2.f, -.5f, 1.8f, -.6f, 2.2f, -.4f), lone()};
        const SiblingFit f = siblingDecide(draws, 2, 1, own);
        expect(f.outcome == SiblingOutcome::Sibling && f.donors == 1 && f.vertices == 300 && std::fabs(f.motion[0] - 2.f) < 1e-6f &&
                   std::fabs(f.motion[1] + .5f) < 1e-6f && std::fabs(f.spread - .4f) < 1e-5f,
               "one donor within the limit gives its mean, and the spread is the larger axis's");
    }
    {
        const SiblingDonor draws[3] = {donor(100, 1.f, 0, .9f, 0, 1.1f, 0), donor(300, 1.5f, 0, 1.4f, 0, 1.6f, 0), lone()};
        const SiblingFit f = siblingDecide(draws, 3, 2, own);
        expect(f.outcome == SiblingOutcome::Sibling && std::fabs(f.motion[0] - 1.375f) < 1e-6f && std::fabs(f.spread - .7f) < 1e-5f,
               "donors whose combined extremes are 0.7 apart are averaged by vertices: (100 x 1 + 300 x 1.5) / 400");
    }
    {
        const SiblingDonor atLimit[2] = {donor(50, 0, 0, 0, 0, kFlatSiblingSpreadPixels, 0), lone()};
        const SiblingDonor over[2] = {donor(50, 0, 0, 0, 0, kFlatSiblingSpreadPixels + .01f, 0), lone()};
        expect(siblingDecide(atLimit, 2, 1, own).outcome == SiblingOutcome::Sibling && siblingDecide(over, 2, 1, own).outcome == SiblingOutcome::Disagree,
               "the limit is inclusive: a spread of exactly one pixel is taken, one hundredth over is refused");
        const SiblingDonor y[2] = {donor(50, 0, 0, 0, 0, 0, kFlatSiblingSpreadPixels + .01f), lone()};
        expect(siblingDecide(y, 2, 1, own).outcome == SiblingOutcome::Disagree, "the vertical axis is held to the limit too");
        expect(std::fabs(siblingDecide(over, 2, 1, own).spread - (kFlatSiblingSpreadPixels + .01f)) < 1e-5f, "a refusal still says the spread it found");
    }
    {
        const SiblingDonor draws[3] = {donor(100, 1.f, 0, .9f, 0, 1.1f, 0), donor(100, 5.f, 0, 4.9f, 0, 5.1f, 0), lone()};
        const SiblingFit f = siblingDecide(draws, 3, 2, own);
        expect(f.outcome == SiblingOutcome::Disagree && f.donors == 2 && f.motion[0] == 0 && f.motion[1] == 0,
               "donors that each agree with themselves and not with each other are refused, with no motion given");
    }
    {
        const SiblingDonor none[1] = {lone()};
        const SiblingFit f = siblingDecide(none, 1, 0, own);
        expect(f.outcome == SiblingOutcome::ViewAttached && f.donors == 0 && f.motion[0] == 0 && f.motion[1] == 0,
               "no donor: the view's own motion, none");
    }
    {
        const SiblingDonor draws[2] = {donor(300, 2.f, 0, 2.f, 0, 2.f, 0, 8, 9), lone()};
        expect(siblingDecide(draws, 2, 1, own).outcome == SiblingOutcome::ViewAttached,
               "a donor of another bone base is not a sibling: its motion is not borrowed");
        const SiblingDonor sig[2] = {donor(300, 2.f, 0, 2.f, 0, 2.f, 0, 7, 10), lone()};
        expect(siblingDecide(sig, 2, 1, own).outcome == SiblingOutcome::ViewAttached,
               "nor is one of another signature word");
        const SiblingDonor param[2] = {donor(300, 2.f, 0, 2.f, 0, 2.f, 0, 7, 9 ^ (1u << 16)), lone()};
        expect(siblingDecide(param, 2, 1, own).outcome == SiblingOutcome::Sibling,
               "but byte 30, a per-instance parameter, is not part of the identity: a donor that differs in it alone is a sibling");
        const uint32_t withParameter[4] = {7, 9 ^ (5u << 16), 1, 0};
        expect(siblingDecide(param, 2, 1, withParameter).outcome == SiblingOutcome::Sibling,
               "nor does byte 30 of the draw's own record: the comparison masks both sides");
        const SiblingDonor flagByte[2] = {donor(300, 2.f, 0, 2.f, 0, 2.f, 0, 7, 9 ^ (1u << 24)), lone()};
        expect(siblingDecide(flagByte, 2, 1, own).outcome == SiblingOutcome::ViewAttached, "a change of byte 31 is another record");
    }
    {
        SiblingDonor unmatched = donor(300, 2.f, 0, 2.f, 0, 2.f, 0);
        unmatched.matched = false;
        const SiblingDonor draws[2] = {unmatched, lone()};
        expect(siblingDecide(draws, 2, 1, own).outcome == SiblingOutcome::ViewAttached,
               "a draw that did not match its own history is no donor, whatever its record holds");
        const SiblingDonor empty[2] = {donor(0, 2.f, 0, 2.f, 0, 2.f, 0), lone()};
        expect(siblingDecide(empty, 2, 1, own).outcome == SiblingOutcome::ViewAttached,
               "a matched draw none of whose vertices could be taken is no donor");
        const SiblingDonor few[2] = {donor(kFlatSiblingMinVertices - 1, 2.f, 0, 2.f, 0, 2.f, 0), lone()};
        const SiblingDonor enough[2] = {donor(kFlatSiblingMinVertices, 2.f, 0, 2.f, 0, 2.f, 0), lone()};
        expect(siblingDecide(few, 2, 1, own).outcome == SiblingOutcome::ViewAttached && siblingDecide(enough, 2, 1, own).outcome == SiblingOutcome::Sibling,
               "fewer donor vertices than one triangle is no sibling, one triangle is");
    }
    {
        SiblingDonor matched = donor(300, 2.f, 0, 2.f, 0, 2.f, 0);
        const SiblingDonor draws[2] = {matched, matched};
        expect(siblingDecide(draws, 2, 0, own).outcome == SiblingOutcome::Matched,
               "a draw that matched its own history needs no fit and gets none");
        const uint32_t unreadable[4] = {7, 9, 0, 0};
        const SiblingDonor two[2] = {matched, lone()};
        expect(siblingDecide(two, 2, 1, unreadable).outcome == SiblingOutcome::IdentityUnreadable,
               "a draw whose own pool identity cannot be read is refused, siblings or not");
        expect(siblingDecide(two, 2, 5, own).outcome == SiblingOutcome::Matched, "a draw index past the frame decides nothing");
    }
    {
        // Every count of a no-history draw has a place in the line: the outcomes number four, the patterns are the history's twenty and two more.
        expect(kSiblingOutcomes == 4 && static_cast<unsigned>(SiblingOutcome::Count) == kSiblingOutcomes + 1 &&
                   static_cast<unsigned>(SiblingOutcome::IdentityUnreadable) == kSiblingOutcomes,
               "the outcomes a no-history draw can take are the four after matched, in the order the line prints them");
        expect(kSiblingPatterns == kHistoryGapCount + 2 && kSiblingPriorFiltered == kHistoryGapCount && kSiblingIdentityDiffers == kHistoryGapCount + 1,
               "the patterns are the history's, then prior-filtered, then identity-differs");
        bool distinct = true;
        for (unsigned i = 0; i < kSiblingPatterns; ++i) {
            distinct = distinct && std::strcmp(siblingPatternName(i), "unknown") != 0;
            for (unsigned j = 0; j < i; ++j) distinct = distinct && std::strcmp(siblingPatternName(i), siblingPatternName(j)) != 0;
        }
        expect(distinct && std::strcmp(siblingPatternName(kSiblingPatterns), "unknown") == 0 &&
                   std::strcmp(siblingPatternName(unsigned(HistoryGap::CountChange)), "count-change") == 0 &&
                   std::strcmp(siblingPatternName(kSiblingIdentityDiffers), "identity-differs") == 0,
               "every pattern has its own name, the history's names unchanged");
        bool named = true;
        for (unsigned o = 0; o < kSiblingOutcomes; ++o) named = named && std::strcmp(siblingOutcomeName(o), "unknown") != 0;
        expect(named && std::strcmp(siblingOutcomeName(0), "sibling") == 0 && std::strcmp(siblingOutcomeName(kSiblingOutcomes), "unknown") == 0,
               "every outcome has a name");
    }
    {
        FlatSiblingWindow w;
        w.engagedFrames = 3; w.dispatches = 7; w.readbacks = 3; w.notReady = 1; w.failed = 0;
        w.by[unsigned(HistoryGap::CountChange)][0] = 5;
        w.by[unsigned(HistoryGap::NewKey)][1] = 2;
        w.by[kSiblingIdentityDiffers][2] = 4;
        w.by[unsigned(HistoryGap::AbsentLong)][3] = 1;
        char line[4096];
        const int n = flatSiblingLine(line, sizeof(line), w);
        expect(n > 0 && n < int(sizeof(line)) && w.draws() == 12,
               "the line fits its buffer, and the window counts every draw once");
        expect(std::strstr(line, "engaged-frames=3 dispatches=7 draws-read=12 (frames read=3 unread=1 failed=0) sibling=5 view-attached=2 disagree=4 identity-unreadable=1;") != nullptr,
               "the line carries the totals by outcome, the frames and the dispatches");
        expect(std::strstr(line, " count-change=5/0/0/0") && std::strstr(line, " new-key=0/2/0/0") && std::strstr(line, " identity-differs=0/0/4/0") &&
                   std::strstr(line, " absent-long=0/0/0/1") && std::strstr(line, " prior-filtered=0/0/0/0"),
               "and every pattern with its four counts, zeros included");
        bool all = true;
        for (unsigned p = 0; p < kSiblingPatterns; ++p) all = all && std::strstr(line, siblingPatternName(p)) != nullptr;
        expect(all, "the line names all twenty-two patterns");
        // The runtime prints it into 4096 bytes: every counter at its widest.
        FlatSiblingWindow widest;
        widest.engagedFrames = widest.dispatches = widest.readbacks = widest.notReady = widest.failed = ~0ull;
        for (auto& row : widest.by) for (auto& cell : row) cell = ~0ull / 64;
        char wide[4096];
        const int wideN = flatSiblingLine(wide, sizeof(wide), widest);
        expect(wideN > 0 && wideN < int(sizeof(wide)), "and the line at its widest fits the runtime's buffer, untruncated");
        char tight[64];
        expect(flatSiblingLine(tight, sizeof(tight), w) > 0, "a buffer too small for the line is truncated, not overrun");
    }
    return failures;
}

inline int flatSiblingWiringTests() {
    using namespace edvr;
    int failures = 0;
    const auto expect = [&](bool ok, const char* name) {
        if (!ok) { std::printf("FAIL: sibling wiring %s\n", name); ++failures; }
    };
    const auto slurp = [](const char* path) {
        std::ifstream in(path, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    };
    const auto compact = [](const std::string& in) {
        std::string out; out.reserve(in.size());
        for (char c : in) if (c != ' ' && c != '\r' && c != '\n' && c != '\t') out += c;
        return out;
    };
    const auto ordered = [&](const std::string& compacted, std::initializer_list<const char*> needles) {
        size_t pos = 0;
        bool ok = !compacted.empty();
        for (const char* needle : needles) {
            const size_t at = compacted.find(compact(needle), pos);
            if (at == std::string::npos) { ok = false; break; }
            pos = at + compact(needle).size();
        }
        return ok;
    };
    const auto without = [&](std::string text, const char* needle) {
        const std::string n = compact(needle);
        const size_t at = text.find(n);
        if (at != std::string::npos) text.erase(at, n.size());
        return text;
    };
    const auto replaced = [&](std::string text, const char* from, const char* to) {
        const std::string f = compact(from);
        const size_t at = text.find(f);
        if (at != std::string::npos) text.replace(at, f.size(), compact(to));
        return text;
    };
    const auto count = [](const std::string& text, const std::string& needle) {
        size_t n = 0, at = 0;
        while ((at = text.find(needle, at)) != std::string::npos) { ++n; at += needle.size(); }
        return n;
    };
    const auto bodyOf = [](const std::string& text, const char* from, const char* to) {
        const size_t a = text.find(from);
        if (a == std::string::npos) return std::string();
        const size_t b = text.find(to, a);
        return text.substr(a, b == std::string::npos ? std::string::npos : b - a);
    };

    const std::string shaderSource = slurp("src/d3d11/flat_foreground_motion_shader.h");
    const std::string motionSource = slurp("src/d3d11/flat_foreground_motion.h");
    const std::string runtimeSource = slurp("src/d3d11/flat_runtime.cpp");
    expect(!shaderSource.empty() && !motionSource.empty() && !runtimeSource.empty(), "the shader, class and runtime sources are readable from the repo root");
    const std::string vs = compact(bodyOf(shaderSource, "inline constexpr char kFlatForegroundMotionVsBody", "inline constexpr char kFlatForegroundMotionPs"));
    const std::string fit = compact(bodyOf(shaderSource, "inline constexpr char kFlatForegroundFitBody", "namespace flat_foreground_detail"));
    const std::string donor = compact(bodyOf(shaderSource, "inline constexpr char kFlatForegroundDonorBody", "inline constexpr char kFlatForegroundFitBody"));
    const std::string match = compact(bodyOf(shaderSource, "inline constexpr char kFlatForegroundMatchHlsl", "inline constexpr char kFlatForegroundMotionVsBody"));
    const std::string motion = compact(motionSource), runtime = compact(runtimeSource);

    // -- the vertex shader takes the fit for the three reasons of a draw with no history, and no others --
    const auto vsValid = [&](const std::string& text) {
        return ordered(text, {"Resolved r=resolveGpuIdentity(id,currentValid);",
                              "o.slot=r.slot;o.valid=r.valid;o.reason=r.reason;o.old=r.old;o.oldPhase=r.oldPhase;",
                              "if(r.valid==2&&sibling.x!=0&&(r.reason==kReasonNoPrior||r.reason==kReasonIdentityDiffers||r.reason==kReasonPriorIdentityInvalid)){",
                              "float4 fit=Fit[sibling.y*2];", "if(fit.z==1.0||fit.z==2.0){",
                              "float2 shift=float2(fit.x/(.5*extentPhase.x),fit.y/(-.5*extentPhase.y));",
                              "o.old=float4(o.p.xy+shift*o.p.w,o.p.zw);o.oldPhase=extentPhase.zw;o.valid=1;o.reason=kReasonNone;"});
    };
    expect(vsValid(vs), "the map's vertex shader gives a draw with no history of its own (reasons 3, 5, 6) the fit's motion, in homogeneous clip space, as a valid sample");
    expect(!vsValid(without(vs, "sibling.x!=0&&")), "mutation control: a map that takes the fit whether or not the pass ran fails the pin");
    expect(!vsValid(replaced(vs, "r.reason==kReasonNoPrior||r.reason==kReasonIdentityDiffers||r.reason==kReasonPriorIdentityInvalid",
                             "r.reason==kReasonNoPrior||r.reason==kReasonIdentityDiffers||r.reason==kReasonPriorIdentityInvalid||r.reason==kReasonAmbiguous")),
           "mutation control: a fit that also covers ambiguous priors (reason 7) fails the pin");
    expect(!vsValid(replaced(vs, "if(fit.z==1.0||fit.z==2.0){", "if(fit.z!=0.0){")), "mutation control: a fit that also takes the refusals (modes 3 and 4) fails the pin");
    expect(!vsValid(replaced(vs, "o.oldPhase=extentPhase.zw;", "o.oldPhase=previousPhaseDepth.xy;")),
           "mutation control: a previous phase that is not this frame's (the sample would carry the jitter step) fails the pin");
    expect(!vsValid(replaced(vs, "fit.y/(-.5*extentPhase.y)", "fit.y/(.5*extentPhase.y)")), "mutation control: the wrong vertical sign fails the pin");

    // -- the donors: the map's own decision, vertex by vertex, and a record of three uint4 --
    const auto donorValid = [&](const std::string& text) {
        return ordered(text, {"Resolved r=resolveGpuIdentity(id,currentValid);", "if(r.valid!=1||!(r.old.w>0)||!(now.w>0)||!all(isfinite(r.old)))continue;",
                              "float2 m=(prev-r.oldPhase)-(cur-extentPhase.zw);", "if(!all(isfinite(m))||any(abs(m)>kMotionLimit))continue;",
                              "const uint matched=(authentic&&matchedPriors(identity,readable)!=0)?1u:0u;",
                              "Donors[draw*3]=uint4(matched,uint(n),identity.x,identity.y&kIdentityParameterMask);",
                              "Donors[draw*3+1]=uint4(asuint(mean.x),asuint(mean.y),0,0);",
                              "Donors[draw*3+2]=uint4(asuint(gMin[0].x),asuint(gMin[0].y),asuint(gMax[0].x),asuint(gMax[0].y));"});
    };
    expect(donorValid(donor), "the donor pass runs the map's decision for every vertex, takes the pixel shader's expression for it, and writes the record the policy reads");
    expect(!donorValid(replaced(donor, "if(r.valid!=1||", "if(")), "mutation control: a donor that takes vertices the map refused fails the pin");
    expect(!donorValid(replaced(donor, "float2 m=(prev-r.oldPhase)-(cur-extentPhase.zw);", "float2 m=prev-cur;")),
           "mutation control: a donor motion that keeps the jitter step fails the pin");
    expect(!donorValid(replaced(donor, "identity.y&kIdentityParameterMask);", "identity.y);")), "mutation control: a donor identity that keeps byte 30 fails the pin");
    expect(!donorValid(replaced(donor, "?1u:0u;", "?1u:1u;")), "mutation control: a donor that is always matched fails the pin");

    // -- the fit: one thread a draw, identity and the matched flag, the limits from the constants --
    const auto fitValid = [&](const std::string& text) {
        return ordered(text, {"if(own.x!=0){Fit[d*2]=float4(0,0,0,0);Fit[d*2+1]=float4(0,0,1,0);return;}",
                              "if(id.z==0){Fit[d*2]=float4(0,0,4,0);",
                              "if(m.x==0||m.y==0||m.z!=id.x||m.w!=(id.y&kIdentityParameterMask))continue;",
                              "if(donors==0||n<limits.y){Fit[d*2]=float4(0,0,2,0);",
                              "if(worst<=limits.x)Fit[d*2]=float4(sum/n,1,worst);", "else Fit[d*2]=float4(0,0,3,worst);"});
    };
    expect(fitValid(fit), "the fit pass leaves a matched draw alone, refuses an unreadable identity, finds donors by identity, and decides by the limits");
    expect(!fitValid(without(fit, "m.z!=id.x||")), "mutation control: a fit that does not compare the bone base fails the pin");
    expect(!fitValid(replaced(fit, "m.w!=(id.y&kIdentityParameterMask)", "m.w!=id.y")), "mutation control: a fit that compares byte 30 fails the pin");
    expect(!fitValid(replaced(fit, "m.x==0||m.y==0||", "m.y==0||")), "mutation control: a fit that counts unmatched draws as donors fails the pin");
    expect(!fitValid(replaced(fit, "if(worst<=limits.x)", "if(worst<limits.x+100)")), "mutation control: a fit with no limit fails the pin");
    expect(!fitValid(replaced(fit, "if(own.x!=0){", "if(false){")), "mutation control: a fit that decides for a draw that matched fails the pin");

    // -- the match is one text for the map and the donors --
    expect(count(compact(shaderSource), "Resolvedresolve") == 1 && count(vs, "resolveGpuIdentity(") == 1 && count(donor, "resolveGpuIdentity(") == 1 &&
               match.find("Resolvedresolve") != std::string::npos,
           "the map and the donor pass call the one decision, defined once");
    expect(compact(shaderSource).find("kFlatForegroundMotionVsText=flat_foreground_detail::concat(kFlatForegroundMatchHlsl,kFlatForegroundMotionVsBody)") != std::string::npos &&
               compact(shaderSource).find("kFlatForegroundDonorCsText=flat_foreground_detail::concat(kFlatForegroundMatchHlsl,kFlatForegroundDonorBody)") != std::string::npos,
           "and the vertex shader and the donor shader are that text and their own body, joined at compile time");

    // -- the class: when the pass runs, what it binds, and what it gives back --
    const std::string run = compact(bodyOf(motionSource, "bool runSibling(", "// Section 104's frame counters and examples"));
    const auto runValid = [&](const std::string& text) {
        return ordered(text, {"if(!siblingPassEnabled())returnfalse;", "bool receiver=frame<=siblingArmedUntil_;",
                              "for(const auto&d:current_)if(d.inputs.gpuIdentity&&d.priorCount==0)receiver=true;", "if(!receiver)returnfalse;",
                              "CsStageSavesaved;saved.save(ctx);", "ctx->ClearUnorderedAccessViewUint(donorsUav_.Get(),zero);",
                              "if(!d.inputs.gpuIdentity||!d.priorCount)continue;", "ctx->Dispatch(1,1,1);",
                              "limits.limits[0]=kFlatSiblingSpreadPixels;limits.limits[1]=float(kFlatSiblingMinVertices);",
                              "ctx->CSSetUnorderedAccessViews(0,1,&noUav,nullptr);ctx->CSSetShaderResources(0,2,noViews);", "saved.restore(ctx);",
                              "siblingOk_=true;returntrue;"}) &&
            count(text, "ctx->Dispatch(") == 2;
    };
    expect(runValid(run), "the pass runs for a draw with no prior or while armed, saves and restores the compute stage, clears the donors, dispatches once a draw with priors and once over all, and unbinds the fit");
    expect(!runValid(without(run, "for(const auto&d:current_)if(d.inputs.gpuIdentity&&d.priorCount==0)receiver=true;")),
           "mutation control: a pass that never learns of a draw with no prior fails the pin");
    expect(!runValid(without(run, "CsStageSavesaved;saved.save(ctx);")), "mutation control: a pass that leaves the game's compute stage as it found it by luck fails the pin");
    expect(!runValid(without(run, "ctx->ClearUnorderedAccessViewUint(donorsUav_.Get(),zero);")), "mutation control: donor records left from the frame before fail the pin");
    expect(!runValid(replaced(run, "limits.limits[0]=kFlatSiblingSpreadPixels;", "limits.limits[0]=100.f;")), "mutation control: a limit that is not the policy's constant fails the pin");
    expect(!runValid(without(run, "ctx->CSSetUnorderedAccessViews(0,1,&noUav,nullptr);ctx->CSSetShaderResources(0,2,noViews);")),
           "mutation control: a fit still bound as a UAV when the map reads it fails the pin");
    const auto prepareValid = [&](const std::string& text) {
        return ordered(text, {"previousNear_=commonNear;", "constboolsiblings=runSibling(ctx,width,height,commonNear,frame);",
                              "ctx->OMSetRenderTargets(1,target_.GetAddressOf(),nullptr);",
                              "c.sibling[0]=(siblings&&d.inputs.gpuIdentity)?1u:0u;c.sibling[1]=drawIndex;c.sibling[2]=d.capture.geometry.count;",
                              "if(siblings)vsViews[11]=fitSrv_.Get();"});
    };
    expect(prepareValid(motion), "prepareH runs the pass before it binds the map's state, tells the map which draw and whether the pass ran, and binds the fit at t11");
    expect(!prepareValid(replaced(motion, "constboolsiblings=runSibling(ctx,width,height,commonNear,frame);", "constboolsiblings=false;")),
           "mutation control: a map told the pass never ran fails the pin");
    expect(!prepareValid(without(motion, "if(siblings)vsViews[11]=fitSrv_.Get();")), "mutation control: a map with no fit bound fails the pin");
    expect(!prepareValid(replaced(motion, "c.sibling[0]=(siblings&&d.inputs.gpuIdentity)?1u:0u;", "c.sibling[0]=1u;")),
           "mutation control: a legacy-identity draw told to read a fit fails the pin");
    expect(count(motion, "pollSibling(ctx,frame);") == 1 && count(motion, "siblingArmedUntil_=s.frame+kSiblingArmedFrames;") == 1,
           "the capture reads the sibling outcomes back once a frame, and a sampled draw with no match arms the pass");
    expect(count(motion, "kFlatSiblingSpreadPixels") == 1 && count(motion, "kFlatSiblingMinVertices") == 1 &&
               count(motion, "limits.limits[0]=kFlatSiblingSpreadPixels") == 1,
           "the policy's two constants reach the shader from one place each");
    expect(runtime.find("flatSiblingLine(line,sizeof(line),sibling);Log::get().note(\"%s\",line);") != std::string::npos &&
               runtime.find("sibling.by[p][o]=delta(captures.siblingBy[p][o],was.siblingBy[p][o]);") != std::string::npos,
           "the 5 s foreground report prints the sibling line from the window of the capture counters");
    return failures;
}
