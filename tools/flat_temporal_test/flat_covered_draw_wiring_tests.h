#pragma once
// THE GRENADE HOLD (design section 104): a first-person draw refused its capture is covered per pixel, not the frame's refusal.
//
// The rigs that run the pieces (tools\weapon_motion_test: the adapter, the history, the map; tools\flat_mono_resolve_test: the prep) cannot run
// the runtime's draw scope, where the decision is taken: a refusal becomes a per-pixel one only if the owner mark of the draw started. These are
// the pins on that wiring and on the adapter's and the history's shape, each with its mutation control. The bench's first_person_pieces case
// (tools\flat_sdk_integration_test) runs the real runtime end to end on a draw over the cap.
inline int flatCoveredDrawWiringTests() {
    int failures = 0;
    const auto expect = [&](bool ok, const char* name) {
        if (!ok) { std::printf("FAIL: covered draw wiring %s\n", name); ++failures; }
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
    const auto body = [](const std::string& text, const char* signature) {
        const size_t at = text.find(signature);
        if (at == std::string::npos) return std::string();
        const size_t end = text.find("\n}\n", at);
        return text.substr(at, end == std::string::npos ? std::string::npos : end + 3 - at);
    };
    const auto count = [](const std::string& text, const std::string& needle) {
        size_t n = 0, at = 0;
        while ((at = text.find(needle, at)) != std::string::npos) { ++n; at += needle.size(); }
        return n;
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

    const std::string runtimeSource = slurp("src/d3d11/flat_runtime.cpp");
    const std::string motionSource = slurp("src/d3d11/flat_foreground_motion.h");
    const std::string historySource = slurp("src/d3d11/animated_vertex_history.h");
    expect(!runtimeSource.empty() && !motionSource.empty() && !historySource.empty(), "the runtime, adapter and history sources are readable from the repo root");
    const std::string runtime = compact(runtimeSource), motion = compact(motionSource), history = compact(historySource);

    // -- the draw scope: the capture's refusal waits for the marker --
    const std::string scope = compact(body(runtimeSource, "void FlatRuntimeDrawScope::beginActualDraw("));
    const auto scopeValid = [&](const std::string& text) {
        return ordered(text, {
            "const char*deferredCapture=nullptr;", "candidate->motion.capture(", "else deferredCapture=candidate->motion.drawRefusal();",
            "domainStarted=engineVelocityFlatDomainBeginDraw(", "if(deferredCapture){", "if(domainStarted){",
            "candidate->motion.coverDraw(deferredCapture);", "domainCovered(s,deferredCapture,failureKey,",
            "}elsefailDomain(\"history\",deferredCapture);"}) &&
            count(text, "failDomain(\"history\"") == 1 && count(text, "candidate->motion.coverDraw(") == 1 &&
            text.find(compact("failDomain(\"history\",candidate->motion.refusal())")) == std::string::npos;
    };
    expect(scopeValid(scope), "the draw scope keeps the capture's refusal until the owner mark has started: covered per pixel if it did, the frame's if not");
    expect(!scopeValid(without(scope, "candidate->motion.coverDraw(deferredCapture);")),
           "mutation control: a refusal that is never covered fails the wiring");
    expect(!scopeValid(without(scope, "}else failDomain(\"history\",deferredCapture);")),
           "mutation control: an uncovered draw that is silently dropped (its pixels unmarked) fails the wiring");
    expect(!scopeValid(replaced(scope, "if(domainStarted){candidate->motion.coverDraw(deferredCapture);", "if(true){candidate->motion.coverDraw(deferredCapture);")),
           "mutation control: a cover that does not ask whether the owner mark started fails the wiring");
    expect(!scopeValid(replaced(scope, "else deferredCapture=candidate->motion.drawRefusal();", "else failDomain(\"history\",candidate->motion.refusal());")),
           "mutation control: the old whole-frame refusal on every capture failure fails the wiring");
    {
        std::string moved = scope;   // covered before the marker is attempted
        const std::string cover = compact("if(deferredCapture){");
        const std::string marker = compact("domainStarted=engineVelocityFlatDomainBeginDraw(");
        const size_t at = moved.find(cover), before = moved.find(marker);
        if (at != std::string::npos && before != std::string::npos && before < at) { moved.erase(at, cover.size()); moved.insert(before, cover); }
        expect(!scopeValid(moved), "mutation control: a refusal judged before the marker is attempted fails the wiring");
    }
    expect(scope.find(compact("if(!domainStarted)failDomain(\"marker\",reason);")) != std::string::npos &&
           scope.find(compact("failDomain(\"marker\",\"foreground-foreign-old-producer\");")) != std::string::npos,
           "a marker that does not start is still the frame's refusal, and so is a foreign draw under the old producer");
    expect(runtime.find(compact("if(domainStarted){engineVelocityFlatDomainEndDraw(ctx);domainStarted=false;"
                                "if(auto*candidate=domainCandidate(state(),domainDepth))candidate->motion.fail(\"foreground-abandoned-writer\");}")) != std::string::npos,
           "a marker that is abandoned (the draw never completed) leaves the frame refused, covered draws included");

    // -- H: the frames saved are counted where H qualifies --
    const std::string helper = compact(body(runtimeSource, "static void foregroundContractAtH("));
    const auto helperValid = [&](const std::string& text) {
        return ordered(text, {"if(foregroundOutput.qualified){", "++s.foregroundCounts.hQualified;",
                              "if(foregroundOutput.coveredDraws)++s.foregroundCounts.hCoveredFrames;"}) &&
            ordered(text, {"s.foregroundCoveredKinds=candidate->coveredKinds;", "s.foregroundFirstCovered=candidate->firstCovered;"});
    };
    expect(helperValid(helper), "H counts a qualified frame that holds a covered draw, and keeps the covered inventory beside the frame refusals'");
    expect(!helperValid(without(helper, "if(foregroundOutput.coveredDraws)++s.foregroundCounts.hCoveredFrames;")),
           "mutation control: frames saved that are never counted fail the wiring");
    expect(!helperValid(replaced(helper, "if(foregroundOutput.coveredDraws)++s.foregroundCounts.hCoveredFrames;", "++s.foregroundCounts.hCoveredFrames;")),
           "mutation control: every qualified frame counted as saved fails the wiring");
    const std::string report = compact(body(runtimeSource, "static void reportForegroundDomain("));
    expect(report.find(compact("H-qualified-with-per-pixel-refusals=%llu")) != std::string::npos &&
               report.find(compact("per-pixel-refused-draws=%llu")) != std::string::npos &&
               report.find(compact("(unsigned long long)n.hCoveredFrames,")) != std::string::npos &&
               report.find(compact("flat foreground per-pixel refusal inventory: frame=%llu")) != std::string::npos &&
               report.find(compact("flat foreground per-pixel refusal kind:")) != std::string::npos &&
               report.find(compact("flat foreground per-pixel refusal first:")) != std::string::npos,
           "the 5 s lines carry the frames saved, the draws covered by cause, and the covered inventory with its first receipt");

    // -- the adapter: only what no owner mark could cover is the frame's at once --
    const std::string capture = compact(body(motionSource.substr(motionSource.find("bool capture(")), "bool capture("));
    const auto captureValid = [&](const std::string& text) {
        return ordered(text, {
            "if(!ctx)returnreject(\"foreground-missing-context\");",
            "returnreject(\"foreground-writer-token-unavailable\");",
            "returndefer(\"foreground-primitive-bound\");", "returndefer(inputs.identity.refusal?", "returndefer(\"foreground-dynamic-VS-linkage\");",
            "returndefer(\"foreground-certificate-bound\");", "returndefer(\"foreground-draw-bound\");",
            "history_.capture(ctx,draw,count,instances,start,base,startInstance,frame,d.capture,true,true)",
            "drawRefusal_=d.capture.refusal?d.capture.refusal:\"history-refused\";returnfalse;}"}) &&
            count(text, "returnreject(") == 2 && text.find(compact("fail(d.capture.refusal)")) == std::string::npos &&
            ordered(text, {"auto defer=[&](constchar*reason){++stats_.preflightRefused;drawRefusal_=reason;returnfalse;};"});
    };
    expect(captureValid(capture), "the adapter's frame refusals are the missing context and the writer token only; every other capture failure is the draw's");
    expect(!captureValid(replaced(capture, "returndefer(\"foreground-draw-bound\");", "returnreject(\"foreground-draw-bound\");")),
           "mutation control: a draw bound that refuses the whole frame again fails the wiring");
    expect(!captureValid(replaced(capture, "drawRefusal_=d.capture.refusal?d.capture.refusal:\"history-refused\";returnfalse;}",
                                  "fail(d.capture.refusal);returnfalse;}")),
           "mutation control: a history refusal that is the frame's again fails the wiring");
    expect(!captureValid(replaced(capture, "frame,d.capture,true,true)", "frame,d.capture,true,false)")),
           "mutation control: a flat adapter on the default (VR) history policy fails the wiring");
    expect(!captureValid(replaced(capture, "returnreject(\"foreground-writer-token-unavailable\");", "returndefer(\"foreground-writer-token-unavailable\");")),
           "mutation control: a draw whose writer token the owner plane cannot carry, deferred, fails the wiring");
    expect(motion.find(compact("const bool gpuFrame=gpuAttempted_||std::any_of(current_.begin(),current_.end(),")) != std::string::npos &&
               motion.find(compact("if(inputs.gpuIdentity)gpuAttempted_=true;")) != std::string::npos,
           "the map's empty clear follows the identity mode of the draws attempted, refused ones included");
    expect(motion.find(compact("out.coveredDraws=covered_;")) != std::string::npos &&
               motion.find(compact("covered_=0;gpuAttempted_=false;drawRefusal_=nullptr;")) != std::string::npos,
           "the covered count reaches H's output and clears with the frame");

    // -- the history: the extended policy is the flat adapter's alone --
    const std::string prepare = compact(body(historySource, "bool prepareCapture("));
    const auto historyValid = [&](const std::string& text) {
        return ordered(text, {
            "constunsignedlimit=extended?maxExtendedOccurrences:maxOccurrences;", "if(priorCount==limit)returnrefuse(\"occurrence-cap\");",
            "if(occurrences>=limit)returnrefuse(\"occurrence-cap\");", "constunsignedfirst=priorCount>4?(std::min)(occurrences>0?occurrences-1:0u,priorCount-4):0u;",
            "records_[prior[first+i]]", "return r.invalidated||(extended&&spentForHistory(r,frame));"}) &&
            text.find(compact("bool extended=false)")) != std::string::npos;
    };
    expect(historyValid(prepare) && historySource.find("maxOccurrences = 4, maxExtendedOccurrences = 64") != std::string::npos,
           "the history: the default policy keeps four draws and four priors; the extended policy allows 64, windows the priors by ordinal, and reclaims spent records");
    expect(!historyValid(replaced(prepare, "constunsignedfirst=priorCount>4?(std::min)(occurrences>0?occurrences-1:0u,priorCount-4):0u;", "constunsignedfirst=0;")),
           "mutation control: priors that are the first four, whatever the draw's ordinal, fail the wiring");
    expect(!historyValid(without(prepare, "||(extended&&spentForHistory(r,frame))")),
           "mutation control: no reclaim of spent records fails the wiring");
    expect(!historyValid(replaced(prepare, "constunsignedlimit=extended?maxExtendedOccurrences:maxOccurrences;", "constunsignedlimit=maxOccurrences;")),
           "mutation control: an extended policy with the default cap fails the wiring");
    expect(count(history, compact("return r.invalidated||(extended&&spentForHistory(r,frame));")) == 1 &&
               history.find(compact("if(r.frame[parity]==~0u)continue;used=true;if(frame-r.frame[parity]<2)returnfalse;")) != std::string::npos,
           "a record is spent only when it has been used and neither its frames is this one or the one before (a prior is never reclaimed; a pending record is not spent)");

    // -- the naming veto: a first-person draw cannot name the world (design section 104, the grenade hold) --
    const auto vetoCall = [&](const std::string& text) {
        return ordered(text, {"const boolsourceShape=d.supported&&!weaponMotionFamilyVs(k.vs)&&k.camera&&k.depth&&sceneExtent&&",
                              "(k.format==23||k.format==26)&&flat_mono_detail::fullViewport(k,k.width,k.height);",
                              "constboolsourceCandidate=sourceShape&&!(!s.namedDepth&&namingVetoed(s,d,k));",
                              "flatUntrustedNomination(d,s.namedDepth,", "sourceCandidate&&!s.namedDepth);",
                              "if(sourceCandidate&&!s.namedDepth){", "s.namedDepth=k.depth;"}) &&
            count(text, "namingVetoed(s,d,k)") == 1;
    };
    expect(vetoCall(runtime), "the source candidate asks the veto before the world is named, ahead of the nomination and the naming that both use it");
    expect(!vetoCall(replaced(runtime, "constboolsourceCandidate=sourceShape&&!(!s.namedDepth&&namingVetoed(s,d,k));", "constboolsourceCandidate=sourceShape;")),
           "mutation control: a source candidate that never asks the veto fails the wiring");
    expect(!vetoCall(replaced(runtime, "constboolsourceCandidate=sourceShape&&!(!s.namedDepth&&namingVetoed(s,d,k));", "constboolsourceCandidate=sourceShape&&!namingVetoed(s,d,k);")),
           "mutation control: a veto that also judges the draws after the world is named fails the wiring");
    const std::string vetoBody = compact(body(runtimeSource, "template<class Draw,class Key>static bool namingVetoed("));
    const auto vetoValid = [&](const std::string& text) {
        return ordered(text, {"if(!s.worldReference.valid()||!k.camera)returnfalse;", "std::memcpy(rows,d.camera,sizeof(rows));",
                              "if(flatDomainPredictsWorld(rows,s.worldReference,&p))returnfalse;", "s.namingVetoedThisFrame=true;++s.namingVetoes;",
                              "flat world naming vetoed %u/12", "returntrue;"});
    };
    expect(vetoValid(vetoBody), "the veto refuses a draw only when a world reference exists and the draw's camera is not predicted by it, and says so");
    expect(!vetoValid(without(vetoBody, "if(!s.worldReference.valid()||!k.camera)returnfalse;")), "mutation control: a veto with no reference to judge by fails the wiring");
    expect(!vetoValid(replaced(vetoBody, "if(flatDomainPredictsWorld(rows,s.worldReference,&p))returnfalse;", "if(!flatDomainPredictsWorld(rows,s.worldReference,&p))returnfalse;")),
           "mutation control: a veto that refuses the world's own draw fails the wiring");
    expect(!vetoValid(without(vetoBody, "s.namingVetoedThisFrame=true;")), "mutation control: a veto the frame boundary never hears of fails the wiring");

    const auto selectionValid = [&](const std::string& text) {
        return ordered(text, {"s.untrustedSupportedAlternate=sel.selected()&&sel.mixedCamera;", "if(sel.selected()){",
                              "constautoreference=flatDomainWorldReference(sel.camera);", "if(reference.valid())s.worldReference=reference;}",
                              "if(sel.selected()&&s.namedDepth&&flatCameraHash(s.namedCamera)!=sel.cameraHash){"});
    };
    expect(selectionValid(runtime), "the camera H selected replaces the world reference, before the check that the naming agrees with it");
    expect(!selectionValid(without(runtime, "if(reference.valid())s.worldReference=reference;")),
           "mutation control: a reference H never corrects, so a first-person camera that set it once keeps it, fails the wiring");

    const auto boundaryValid = [&](const std::string& text) {
        return ordered(text, {"if(s.namingVetoedThisFrame){", "if(s.namedDepth)s.namingVetoStreak=0;",
                              "elseif(++s.namingVetoStreak>=kFlatNamingVetoFrames){s.worldReference=FlatDomainWorldReference{};",
                              "++s.namingVetoReleases;}", "s.namingVetoedThisFrame=false;", "}elses.namingVetoStreak=0;",
                              "s.namedDepth=s.namedConstants=nullptr;"});
    };
    expect(boundaryValid(runtime), "a frame that vetoed and never named counts toward giving the reference up; one that named resets the count; both before the naming resets");
    expect(!boundaryValid(without(runtime, "elseif(++s.namingVetoStreak>=kFlatNamingVetoFrames){s.worldReference=FlatDomainWorldReference{};")),
           "mutation control: a veto that can never release a reference that is wrong fails the wiring");
    expect(!boundaryValid(replaced(runtime, "if(s.namedDepth)s.namingVetoStreak=0;", "if(false)s.namingVetoStreak=0;")),
           "mutation control: a frame that named after a veto counted as a failed one fails the wiring");
    expect(slurp("src/d3d11/flat_domain_admission.h").find("constexpr uint32_t kFlatNamingVetoFrames = 3;") != std::string::npos,
           "three frames in a row give the reference up");
    expect(report.find(compact("naming-vetoes=%llu naming-veto-releases=%llu")) != std::string::npos &&
               report.find(compact("(unsigned long long)s.namingVetoes,(unsigned long long)s.namingVetoReleases,")) != std::string::npos,
           "the 5 s line counts the vetoes and the releases");
    return failures;
}
