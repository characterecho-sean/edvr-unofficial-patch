#include "../../src/common/plugin_cost.h"
#include "plugin_manifest.inc"
#include "../../src/d3d11/cockpit_cost_sites.h"
#include "../../src/d3d11/binding_cost_sites.h"
#include "../../src/d3d11/draw_cpu_window.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <thread>

namespace pc = edvr::plugin_cost;

std::string functionBody(const std::string& source, const std::string& signature);

bool check(bool condition, const char* label) {
    if (!condition) std::printf("FAIL: %s\n", label);
    return condition;
}

bool drawCpuWindowChecks() {
    bool ok = true;
    edvr::draw_cpu::Window window;

    // The per-draw numerator must use the existing frame-level clamp. If the
    // first frame were clamped draw-by-draw, its 100/130 pair plus the second
    // callback would incorrectly contribute 20 ticks instead of zero.
    window.noteDraw(100);
    window.noteDraw(20);
    window.closeFrame(true, 120, 130);
    window.noteDraw(50);
    window.noteDraw(25);
    window.closeFrame(true, 75, 30);
    ok &= check(window.windowTimedDraws == 4 && window.windowOwnTicks == 45 &&
                std::abs(window.meanMs(1000) - 11.25) < 1e-12,
                "draw CPU window uses frame-level clamp and draw-weighted sample denominator");

    // Invalid/unsampled frames discard their per-frame denominator and do not
    // contaminate the valid window.
    window.noteDraw(100);
    window.closeFrame(false, 100, 0);
    ok &= check(window.windowTimedDraws == 4 && window.windowOwnTicks == 45 &&
                window.frameTimedDraws == 0,
                "unsampled or invalid-frequency frames are excluded and reset per-frame state");

    edvr::draw_cpu::Window zero;
    zero.closeFrame(true, 0, 0);
    ok &= check(!zero.hasTimedDraws(),
                "no timed draw callbacks remains unavailable rather than measured zero");
    zero.noteDraw(10);  // A valid sample whose entire interval was forwarded.
    zero.closeFrame(true, 10, 10);
    ok &= check(zero.hasTimedDraws() && zero.windowTimedDraws == 1 &&
                zero.meanMs(1000) == 0.0,
                "all-forwarded sample is measured zero with a nonzero denominator");

    // This is the report-window boundary. Activity/config toggles are outside
    // this fixed sampler and must not reset these stats; only report close does.
    window.resetWindow();
    ok &= check(!window.hasTimedDraws() && window.windowOwnTicks == 0 &&
                window.frameTimedDraws == 0,
                "report close clears the aggregate draw window");
    return ok;
}

struct FakeClock final {
    static inline uint64_t ticks = 0;
    static inline unsigned reads = 0;
    static uint64_t now() noexcept {
        ++reads;
        ticks += 10;
        return ticks;
    }
};

struct FakeSink final {
    static inline unsigned siteNotes = 0;
    static inline unsigned tickNotes = 0;
    static inline uint64_t tickTotal = 0;
    static void site(pc::Owner, uint16_t, pc::SiteEvent) noexcept { ++siteNotes; }
    static void ticks(pc::Owner, uint16_t, uint64_t value) noexcept {
        ++tickNotes;
        tickTotal += value;
    }
};

template <class Policy>
void policyScope(uint64_t& handlers) {
    Policy::template note<pc::Owner::CockpitVisuals, 7>(pc::SiteEvent::Reached);
    {
        typename Policy::template Scope<pc::Owner::CockpitVisuals, 7> scope;
        (void)scope;
        ++handlers;
    }
}

bool policyChecks() {
    bool ok = true;
    static_assert(edvr::binding_cost::id(edvr::binding_cost::Site::GetResource) == 112);
    static_assert(edvr::binding_cost::id(edvr::binding_cost::Site::GetType) == 113);
    static_assert(edvr::binding_cost::id(edvr::binding_cost::Site::BufferGetDesc) == 114);
    static_assert(edvr::binding_cost::id(edvr::binding_cost::Site::Texture2DGetDesc) == 115);
    static_assert(static_cast<uint8_t>(pc::Owner::Core) == 9);
    static_assert(static_cast<uint8_t>(pc::ApiClass::ReadQuery) == 3);
    static_assert(static_cast<uint8_t>(pc::Owner::TemporalAa) == edvr::plugins::kPluginTemporalAa);
    static_assert(static_cast<uint8_t>(pc::Owner::CockpitVisuals) == edvr::plugins::kPluginCockpitVisuals);
    static_assert(static_cast<uint8_t>(pc::Owner::Exposure) == edvr::plugins::kPluginExposure);
    static_assert(static_cast<uint8_t>(pc::Owner::Scanners) == edvr::plugins::kPluginScanners);
    static_assert(static_cast<uint8_t>(pc::Owner::Intro) == edvr::plugins::kPluginIntro);
    static_assert(static_cast<uint8_t>(pc::Owner::OnFootPanel) == edvr::plugins::kPluginOnFootPanel);
    static_assert(static_cast<uint8_t>(pc::Owner::Comfort) == edvr::plugins::kPluginComfort);
    static_assert(static_cast<uint8_t>(pc::Owner::Performance) == edvr::plugins::kPluginPerformance);
    static_assert(static_cast<uint8_t>(pc::Owner::Diagnostics) == edvr::plugins::kPluginDiagnostics);

    uint64_t handlers = 0;
    pc::NoCpu::template note<pc::Owner::CockpitVisuals, 7>(pc::SiteEvent::Reached);
    policyScope<pc::NoCpu>(handlers);
    ok &= check(handlers == 1 && FakeClock::reads == 0 && FakeSink::siteNotes == 0 &&
                FakeSink::tickNotes == 0,
                "NoCpu policy compiles to no clock, sink, or counter callbacks");

    using Sampled = pc::SampledCpu<FakeClock, FakeSink>;
    Sampled::template note<pc::Owner::CockpitVisuals, 8>(pc::SiteEvent::Reached);
    policyScope<Sampled>(handlers);
    ok &= check(FakeClock::reads == 2 && FakeSink::siteNotes == 2 &&
                FakeSink::tickNotes == 1 && FakeSink::tickTotal == 10,
                "sampled scope clocks only an invoked handler and reports exact ticks");
    return ok;
}

bool nightVisionAnnotationChecks() {
    std::ifstream input("src/plugins/cockpit_visuals/night_vision.cpp", std::ios::binary);
    if (!input) return check(false, "Night Vision source is available for API coverage verification");
    const std::string source((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    std::istringstream lines(source);
    std::set<std::string> usedSites;
    std::set<std::string> declaredSites;
    std::set<unsigned> declaredValues;
    std::size_t calls = 0;
    bool ok = true;
    std::string line;
    bool inSiteEnum = false;
    const std::size_t getTypeCall = source.find("ctx->GetType()");
    const std::size_t immediateReturn = source.find("if(ctx->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE)return;");
    const std::size_t sampleRead = source.find("state.costSample=edvrPluginCostApiSampleContext(ctx)!=0;");
    const std::size_t getTypeNote = source.find("noteNvD3dCall<NvD3dCallSite::GetType>(plugin_cost::ApiClass::ReadQuery);");
    const std::size_t getTypeEnd = immediateReturn == std::string::npos
        ? std::string::npos : source.find(';', immediateReturn);
    ok &= check(getTypeCall != std::string::npos && immediateReturn <= getTypeCall &&
                getTypeEnd != std::string::npos && getTypeCall < getTypeEnd &&
                getTypeEnd < sampleRead && sampleRead < getTypeNote &&
                source.find("edvrPluginCostApiSampleContext(ctx)", sampleRead +
                    std::strlen("edvrPluginCostApiSampleContext(ctx)")) == std::string::npos &&
                source.find("edvrPluginCostApiSampleFrame()") == std::string::npos &&
                source.find("ctx->GetType()", getTypeCall + 1) == std::string::npos,
                "GetType executes once; deferred rejection precedes the owner-context sample gate and conditional note");
    while (std::getline(lines, line)) {
        if (line.find("enum class NvD3dCallSite") != std::string::npos) inSiteEnum = true;
        if (inSiteEnum && line.find("};") != std::string::npos) inSiteEnum = false;
        else if (inSiteEnum) {
            const std::size_t equal = line.find('=');
            if (equal != std::string::npos) {
                const std::size_t nameStart = line.find_first_not_of(" \t", line.find('{') == std::string::npos ? 0 : line.find('{') + 1);
                const std::size_t nameEnd = line.find_first_of(" \t", nameStart);
                const std::size_t valueStart = line.find_first_not_of(" \t", equal + 1);
                if (nameStart != std::string::npos && nameEnd != std::string::npos && valueStart != std::string::npos) {
                    const std::string name = line.substr(nameStart, nameEnd - nameStart);
                    const unsigned value = static_cast<unsigned>(std::strtoul(line.c_str() + valueStart, nullptr, 10));
                    declaredSites.insert(name);
                    declaredValues.insert(value);
                }
            }
        }
        const std::size_t comment = line.find("//");
        const std::size_t codeEnd = comment == std::string::npos ? line.size() : comment;
        std::size_t call = line.find("ctx->");
        std::size_t previousCall = std::string::npos;
        while (call != std::string::npos && call < codeEnd) {
            ++calls;
            const std::size_t note = line.rfind("noteNvD3dCall<NvD3dCallSite::", call);
            const std::size_t methodStart = call + std::strlen("ctx->");
            const std::size_t methodEnd = line.find('(', methodStart);
            const std::string method = methodEnd == std::string::npos
                ? std::string{} : line.substr(methodStart, methodEnd - methodStart);
            if (method == "GetType") {
                ok &= check(usedSites.insert("GetType").second,
                            "GetType source site has its stable deferred-safe note identity");
                previousCall = call;
                call = line.find("ctx->", call + std::strlen("ctx->"));
                continue;
            }
            const bool noteAfterPriorCall = note != std::string::npos &&
                (previousCall == std::string::npos || note > previousCall);
            const std::size_t siteEnd = noteAfterPriorCall ? line.find('>', note) : std::string::npos;
            const std::size_t noteEnd = siteEnd == std::string::npos ? std::string::npos : line.find(';', siteEnd);
            const bool adjacent = noteEnd != std::string::npos &&
                line.substr(noteEnd + 1, call - noteEnd - 1).find_first_not_of(" \t") == std::string::npos;
            if (!noteAfterPriorCall || !adjacent) {
                ok &= check(false, "every direct context API call has one immediately preceding note");
            } else {
                const std::string site = line.substr(note + std::strlen("noteNvD3dCall<NvD3dCallSite::"),
                                                     siteEnd - note - std::strlen("noteNvD3dCall<NvD3dCallSite::"));
                ok &= check(usedSites.insert(site).second,
                            "each direct context API source site has a unique coverage identity");
                const std::size_t apiClassPos = line.find("ApiClass::", siteEnd);
                const std::size_t apiClassEnd = apiClassPos == std::string::npos
                    ? std::string::npos : line.find(')', apiClassPos);
                std::string expectedClass;
                if (method == "Dispatch") expectedClass = "Work";
                else if (method == "UpdateSubresource") expectedClass = "Transfer";
                else if (method.rfind("Get", 0) == 0 || method.rfind("OMGet", 0) == 0 ||
                         method.rfind("CSGet", 0) == 0 || method.rfind("PSGet", 0) == 0 ||
                         method.rfind("RSGet", 0) == 0) expectedClass = "ReadQuery";
                else if (method.rfind("OMSet", 0) == 0 || method.rfind("CSSet", 0) == 0 ||
                         method.rfind("PSSet", 0) == 0) expectedClass = "State";
                const std::string actualClass = apiClassEnd == std::string::npos
                    ? std::string{} : line.substr(apiClassPos + std::strlen("ApiClass::"),
                                                  apiClassEnd - apiClassPos - std::strlen("ApiClass::"));
                ok &= check(!expectedClass.empty() && actualClass == expectedClass,
                            "direct context API note uses its matching operation category");
            }
            previousCall = call;
            call = line.find("ctx->", call + std::strlen("ctx->"));
        }
    }
    for (const std::string& site : usedSites)
        ok &= check(declaredSites.count(site) != 0, "annotated API site has a declared stable identity");
    ok &= check(calls == 39 && usedSites.size() == calls && declaredSites.size() == 40 &&
                declaredValues.size() == declaredSites.size(),
                "first Night Vision API slice covers 39 direct calls with unique stable site IDs");
    return ok;
}

std::string readSource(const char* path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return {};
    return std::string((std::istreambuf_iterator<char>(input)),
                       std::istreambuf_iterator<char>());
}

struct ExpectedApiSite {
    const char* method;
    const char* site;
    const char* apiClass;
    uint16_t id;
};

bool leafApiSourceChecks(const char* path, const ExpectedApiSite* expected,
                         std::size_t expectedCount, const char* label) {
    const std::string source = readSource(path);
    if (source.empty()) return check(false, label);
    bool ok = true;
    std::set<std::string> seen;
    std::size_t calls = 0;
    std::size_t pos = 0;
    while ((pos = source.find("ctx->", pos)) != std::string::npos) {
        const std::size_t lineStart = source.rfind('\n', pos);
        const std::size_t comment = source.rfind("//", pos);
        if (comment != std::string::npos &&
            (lineStart == std::string::npos || comment > lineStart)) {
            pos += 5;
            continue;
        }
        const std::size_t methodStart = pos + 5;
        const std::size_t methodEnd = source.find('(', methodStart);
        if (methodEnd == std::string::npos) break;
        const std::string method = source.substr(methodStart, methodEnd - methodStart);
        const ExpectedApiSite* match = calls < expectedCount ? &expected[calls] : nullptr;
        ok &= check(match != nullptr, "leaf source has only the declared immediate-context API calls");
        if (!match) { pos = methodEnd + 1; continue; }
        ok &= check(method == match->method,
                    "leaf immediate-context methods retain their pinned source order");
        ++calls;

        const std::size_t note = source.rfind("edvrPluginCostNoteD3dCall(", pos);
        const std::size_t noteEnd = note == std::string::npos
            ? std::string::npos : source.find(");", note);
        const std::size_t site = note == std::string::npos
            ? std::string::npos : source.find("cockpit_cost::Site::", note);
        const std::size_t classPos = note == std::string::npos
            ? std::string::npos : source.find("plugin_cost::ApiClass::", note);
        const bool noteOrder = note != std::string::npos && noteEnd != std::string::npos &&
            noteEnd < pos && source.find("edvrPluginCostNoteD3dCall(", note + 1) >= pos;
        std::string between;
        if (noteOrder) between = source.substr(noteEnd + 2, pos - noteEnd - 2);
        between.erase(std::remove_if(between.begin(), between.end(),
            [](unsigned char c) { return std::isspace(c) != 0; }), between.end());
        // Notes are conditional; the closing brace is the only token between
        // the note and the unconditional context call.
        ok &= check(noteOrder && (between.empty() || between == "}"),
                    "each leaf context call has its conditional note immediately before it");
        if (noteOrder && site != std::string::npos && classPos != std::string::npos) {
            const std::size_t siteEnd = source.find_first_of(") ,", site);
            const std::size_t classEnd = source.find(')', classPos);
            const std::string actualSite = source.substr(site + std::strlen("cockpit_cost::Site::"),
                siteEnd - site - std::strlen("cockpit_cost::Site::"));
            const std::string actualClass = source.substr(classPos + std::strlen("plugin_cost::ApiClass::"),
                classEnd - classPos - std::strlen("plugin_cost::ApiClass::"));
            ok &= check(actualSite == match->site && actualClass == match->apiClass,
                        "leaf context call uses its pinned stable site and API class");
            ok &= check(seen.insert(actualSite).second,
                        "each leaf immediate-context source site has a unique ID");
        } else {
            ok &= check(false, "leaf context call has a parseable stable-site note");
        }
        pos = methodEnd + 1;
    }
    ok &= check(calls == expectedCount && seen.size() == expectedCount,
                "leaf API coverage contains exactly the declared, unique source sites");
    return ok;
}

bool targetSharpRemlokAnnotationChecks() {
    static_assert(edvr::cockpit_cost::id(edvr::cockpit_cost::Site::TargetVsGetShader) == 64);
    static_assert(edvr::cockpit_cost::id(edvr::cockpit_cost::Site::TargetPsGetShader) == 65);
    static_assert(edvr::cockpit_cost::id(edvr::cockpit_cost::Site::TargetPsSetShaderApply) == 66);
    static_assert(edvr::cockpit_cost::id(edvr::cockpit_cost::Site::TargetPsSetShaderRestore) == 67);
    static_assert(edvr::cockpit_cost::id(edvr::cockpit_cost::Site::RemlokGetDevice) == 72);
    static_assert(edvr::cockpit_cost::id(edvr::cockpit_cost::Site::RemlokRsGetState) == 73);
    static_assert(edvr::cockpit_cost::id(edvr::cockpit_cost::Site::RemlokRsGetViewportsCurrent) == 74);
    static_assert(edvr::cockpit_cost::id(edvr::cockpit_cost::Site::RemlokRsGetScissorRects) == 75);
    static_assert(edvr::cockpit_cost::id(edvr::cockpit_cost::Site::RemlokRsGetViewportsSaved) == 76);
    static_assert(edvr::cockpit_cost::id(edvr::cockpit_cost::Site::RemlokRsSetViewportsApply) == 77);
    static_assert(edvr::cockpit_cost::id(edvr::cockpit_cost::Site::RemlokRsSetScissorRectsApply) == 78);
    static_assert(edvr::cockpit_cost::id(edvr::cockpit_cost::Site::RemlokRsSetStateApply) == 79);
    static_assert(edvr::cockpit_cost::id(edvr::cockpit_cost::Site::RemlokRsSetStateRestore) == 80);
    static_assert(edvr::cockpit_cost::id(edvr::cockpit_cost::Site::RemlokRsSetScissorRectsRestore) == 81);
    static_assert(edvr::cockpit_cost::id(edvr::cockpit_cost::Site::RemlokRsSetViewportsRestore) == 82);
    const ExpectedApiSite target[] = {
        {"VSGetShader", "TargetVsGetShader", "ReadQuery", 64},
        {"PSGetShader", "TargetPsGetShader", "ReadQuery", 65},
        {"PSSetShader", "TargetPsSetShaderApply", "State", 66},
        {"PSSetShader", "TargetPsSetShaderRestore", "State", 67},
    };
    const ExpectedApiSite remlok[] = {
        {"GetDevice", "RemlokGetDevice", "ReadQuery", 72},
        {"RSGetState", "RemlokRsGetState", "ReadQuery", 73},
        {"RSGetViewports", "RemlokRsGetViewportsCurrent", "ReadQuery", 74},
        {"RSGetScissorRects", "RemlokRsGetScissorRects", "ReadQuery", 75},
        {"RSGetViewports", "RemlokRsGetViewportsSaved", "ReadQuery", 76},
        {"RSSetViewports", "RemlokRsSetViewportsApply", "State", 77},
        {"RSSetScissorRects", "RemlokRsSetScissorRectsApply", "State", 78},
        {"RSSetState", "RemlokRsSetStateApply", "State", 79},
        {"RSSetState", "RemlokRsSetStateRestore", "State", 80},
        {"RSSetScissorRects", "RemlokRsSetScissorRectsRestore", "State", 81},
        {"RSSetViewports", "RemlokRsSetViewportsRestore", "State", 82},
    };
    bool ok = true;
    ok &= leafApiSourceChecks("src/d3d11/target_sharp.cpp", target,
                              sizeof(target) / sizeof(target[0]),
                              "TargetSharp source is available for direct API coverage verification");
    ok &= leafApiSourceChecks("src/d3d11/remlok_fix.cpp", remlok,
                              sizeof(remlok) / sizeof(remlok[0]),
                              "RemLok source is available for direct API coverage verification");

    const std::string sharp = readSource("src/d3d11/target_sharp.cpp");
    const std::string rem = readSource("src/d3d11/remlok_fix.cpp");
    const std::string probe = functionBody(sharp, "bool targetSharpOnEyeDrawImpl(");
    const std::string sharpDefault = functionBody(sharp, "bool targetSharpOnEyeDraw(");
    const std::string sharpObserved = functionBody(sharp, "bool targetSharpOnEyeDrawObserved(");
    const std::string sharpBegin = functionBody(sharp, "void targetSharpBegin(");
    const std::string sharpEnd = functionBody(sharp, "void targetSharpEnd(");
    const std::string remBegin = functionBody(rem, "void remlokScissorBegin(");
    const std::string remEnd = functionBody(rem, "void remlokScissorEnd(");
    const std::string clone = functionBody(rem, "ID3D11RasterizerState* cloneWithScissor(");
    const std::size_t probeSample = probe.find("edvrPluginCostApiSampleContext(ctx)");
    const std::size_t probeNote = probe.find("edvrPluginCostNoteD3dCall(", probeSample);
    const std::size_t probeSite = probe.find("cockpit_cost::Site::TargetVsGetShader", probeNote);
    const std::size_t probeQuery = probe.find("ctx->VSGetShader(");
    ok &= check(!probe.empty() && probeSample != std::string::npos &&
                probeNote != std::string::npos && probeSite != std::string::npos &&
                probeQuery != std::string::npos && probeSample < probeNote &&
                probeNote < probeSite && probeSite < probeQuery,
                "TargetSharp's existing shader probe samples and annotates at the actual query");
    const auto compact = [](std::string body) {
        body.erase(std::remove_if(body.begin(), body.end(),
            [](unsigned char c) { return std::isspace(c) != 0; }), body.end());
        return body;
    };
    const auto wrappersRouteToProbe = [&](const std::string& defaultBody,
                                          const std::string& observedBody) {
        return compact(defaultBody) ==
                   "{returntargetSharpOnEyeDrawImpl<false>(ctx,kind,count,instances,nullptr);}" &&
               compact(observedBody) ==
                   "{returntargetSharpOnEyeDrawImpl<true>(ctx,kind,count,instances,&observation);}";
    };
    ok &= check(wrappersRouteToProbe(sharpDefault, sharpObserved),
                "TargetSharp default and observed entry points both return the shared annotated probe result");
    ok &= check(!wrappersRouteToProbe(
                    "{targetSharpOnEyeDrawImpl<false>(ctx,kind,count,instances,nullptr);return false;}",
                    sharpObserved) &&
                !wrappersRouteToProbe(sharpDefault, "{return false;}"),
                "TargetSharp source pin rejects either wrapper bypassing the shared probe result");
    const std::size_t sharpNoReplacement = sharpBegin.find("if (!ps) return");
    const std::size_t sharpBeginSample = sharpBegin.find("edvrPluginCostApiSampleContext(ctx)");
    const std::size_t sharpGet = sharpBegin.find("ctx->PSGetShader");
    const std::size_t sharpSet = sharpBegin.find("ctx->PSSetShader");
    ok &= check(sharpNoReplacement != std::string::npos &&
                sharpBeginSample != std::string::npos && sharpGet != std::string::npos &&
                sharpSet != std::string::npos &&
                sharpNoReplacement < sharpBeginSample && sharpBeginSample < sharpGet &&
                sharpGet < sharpSet,
                "TargetSharp captures sampling only after replacement succeeds and preserves query-before-set order");
    const std::size_t sharpSavedSample = sharpEnd.find("const bool costSample = g_costSample;");
    const std::size_t sharpClearSample = sharpEnd.find("g_costSample = false;", sharpSavedSample);
    const std::size_t sharpRestore = sharpEnd.find("ctx->PSSetShader");
    ok &= check(sharpSavedSample != std::string::npos && sharpClearSample != std::string::npos &&
                sharpRestore != std::string::npos &&
                sharpSavedSample < sharpClearSample && sharpClearSample < sharpRestore,
                "TargetSharp carries the Begin sample into its matching restore then clears it");
    const std::size_t remSample = remBegin.find("g_costSample = edvrPluginCostApiSampleContext(ctx) != 0;");
    const std::size_t remGetState = remBegin.find("ctx->RSGetState");
    ok &= check(remSample != std::string::npos && remGetState != std::string::npos &&
                remSample < remGetState,
                "RemLok captures the API sample before its first context query");
    ok &= check(rem.find("bool costSample) {") != std::string::npos,
                "RemLok carries its saved API-sample decision into the device-query helper");
    const std::size_t cloneGuard = clone.find("if (costSample)");
    const std::size_t cloneGetDevice = clone.find("ctx->GetDevice");
    ok &= check(cloneGuard != std::string::npos && cloneGetDevice != std::string::npos &&
                cloneGuard < cloneGetDevice,
                "RemLok guards the conditional device query note by its saved sample");
    std::string compactRemBegin = remBegin;
    compactRemBegin.erase(std::remove_if(compactRemBegin.begin(), compactRemBegin.end(),
        [](unsigned char c) { return std::isspace(c) != 0; }), compactRemBegin.end());
    const std::size_t remSavedSample = remEnd.find("const bool costSample = g_costSample;");
    const std::size_t remClearSample = remEnd.find("g_costSample = false;", remSavedSample);
    const std::size_t remRestore = remEnd.find("ctx->RSSetState");
    ok &= check(compactRemBegin.find("g_costSample=false;return;") != std::string::npos &&
                remSavedSample != std::string::npos && remClearSample != std::string::npos &&
                remRestore != std::string::npos &&
                remSavedSample < remClearSample && remClearSample < remRestore,
                "RemLok declines clear their sample and successful End carries it across all restores");
    return ok;
}

std::string functionBody(const std::string& source, const std::string& signature) {
    const std::size_t start = source.find(signature);
    if (start == std::string::npos) return {};
    const std::size_t open = source.find('{', start + signature.size());
    if (open == std::string::npos) return {};
    unsigned depth = 0;
    for (std::size_t i = open; i < source.size(); ++i) {
        if (source[i] == '{') ++depth;
        else if (source[i] == '}' && --depth == 0) return source.substr(open, i - open + 1);
    }
    return {};
}

bool productionCpuRouteChecks() {
    std::ifstream input("src/d3d11/vscreen.cpp", std::ios::binary);
    if (!input) return check(false, "vScreen source is available for CPU policy route verification");
    const std::string source((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    bool ok = true;
    constexpr const char* directDraws[] = {
        "void STDMETHODCALLTYPE hookedDraw(",
        "void STDMETHODCALLTYPE hookedDrawIndexed(",
        "void STDMETHODCALLTYPE hookedDrawInstanced(",
        "void STDMETHODCALLTYPE hookedDrawIndexedInstanced(",
    };
    for (const char* signature : directDraws) {
        const std::string body = functionBody(source, signature);
        const std::size_t flat = body.find("runtimeFlatProfile()");
        const std::size_t internal = body.find("g_vrWorldInternal");
        const std::size_t clock = body.find("DrawClock clock;");
        const std::size_t ladder = body.find("withDrawLadderTrace(");
        const std::size_t ownerGate = body.find("clock.cpuOn && g_state && self == g_state->ownerCtx");
        ok &= check(!body.empty() && flat < internal && internal < clock && clock < ladder &&
                    ladder < ownerGate && ownerGate != std::string::npos,
                    "flat/internal bypasses precede profiling and direct draws sample owner context only");
        if (!body.empty() && internal < clock) {
            const std::string internalRoute = body.substr(internal, clock - internal);
            ok &= check(internalRoute.find("return;") != std::string::npos,
                        "internal world reentry returns before direct-draw CPU sampling");
        }
    }

    constexpr const char* bypasses[] = {
        "void STDMETHODCALLTYPE hookedDrawAuto(",
        "void STDMETHODCALLTYPE hookedDrawIndexedInstancedIndirect(",
        "void STDMETHODCALLTYPE hookedDrawInstancedIndirect(",
    };
    for (const char* signature : bypasses) {
        const std::string body = functionBody(source, signature);
        ok &= check(!body.empty() && body.find("DrawClock") == std::string::npos &&
                    body.find("withDrawLadderTrace(") == std::string::npos &&
                    body.find("SampledCpu") == std::string::npos,
                    "Auto and indirect bypass routes contain no classifier CPU clock path");
    }

    const std::string chooser = functionBody(source, "LadderDecision withDrawLadderTrace(");
    const std::size_t capturing = chooser.find("if (capturing)");
    const std::size_t captureNoCpu = chooser.find("plugin_cost::NoCpu noCpu;");
    const std::size_t captureWork = chooser.find("work(trace, noCpu)");
    const std::size_t selected = chooser.find("if (cpuSample && !suppressCpu)");
    const std::size_t sampled = chooser.find("plugin_cost::SampledCpu<> sampledCpu;");
    const std::size_t unsampled = chooser.find("plugin_cost::NoCpu noCpu;", sampled);
    ok &= check(!chooser.empty() && capturing < captureNoCpu && captureNoCpu < captureWork &&
                captureWork < selected && selected < sampled && sampled < unsampled &&
                chooser.find("return work(noTrace, noCpu);") != std::string::npos,
                "trace capture selects NoCpu before the independent sampled-or-unsampled classifier path");
    if (captureWork != std::string::npos && selected > captureWork) {
        const std::string captureBranch = chooser.substr(capturing, selected - capturing);
        ok &= check(captureBranch.find("SampledCpu") == std::string::npos &&
                    captureBranch.find("work(trace, noCpu)") != std::string::npos,
                    "trace capture path has no sampled CPU policy or QPC handler scope");
    }
    return ok;
}

bool collectorHotPathChecks() {
    std::ifstream input("src/d3d11/plugin_cost.cpp", std::ios::binary);
    if (!input) return check(false, "collector source is available for fixed-memory note-path verification");
    const std::string source((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    bool ok = true;
    constexpr const char* noteFunctions[] = {
        "extern \"C\" void edvrPluginCostNoteSite(",
        "extern \"C\" void edvrPluginCostNoteCpuTicks(",
        "extern \"C\" void edvrPluginCostNoteD3dCall(",
    };
    for (const char* signature : noteFunctions) {
        const std::string body = functionBody(source, signature);
        ok &= check(!body.empty() && body.find("Log::") == std::string::npos &&
                    body.find("new ") == std::string::npos &&
                    body.find("std::vector") == std::string::npos &&
                    body.find("mutex") == std::string::npos &&
                    body.find("qpcNow") == std::string::npos,
                    "per-site cost notes remain fixed-memory with no logger, allocator, lock, or clock");
    }
    const std::string apiGuard = functionBody(source,
        "extern \"C\" uint8_t edvrPluginCostApiSampleContext(");
    const std::string ownerGuard = functionBody(source,
        "extern \"C\" uint8_t edvrPluginCostApiSampleOwnerThread(");
    const std::string boundary = functionBody(source,
        "extern \"C\" uint8_t edvrPluginCostFrameBoundary(");
    const std::string registration = functionBody(source,
        "extern \"C\" void edvrPluginCostSetOwnerContext(");
    const std::string configure = functionBody(source,
        "extern \"C\" void edvrPluginCostConfigure(");
    const std::string shutdown = functionBody(source,
        "extern \"C\" void edvrPluginCostShutdown(");
    ok &= check(!apiGuard.empty() &&
                apiGuard.find("g_ownerContext.load(std::memory_order_acquire) != context") <
                    apiGuard.find("g_ownerThreadToken.load(std::memory_order_acquire)") &&
                apiGuard.find("g_threadToken == 0") != std::string::npos &&
                apiGuard.find("threadToken()") == std::string::npos &&
                apiGuard.find("fetch_add") == std::string::npos &&
                apiGuard.find("g_threadToken != owner") < apiGuard.find("return g_configured && g_apiSampleFrame"),
                "sample getter checks context and existing TLS identity without allocating a foreign-thread token");
    ok &= check(!ownerGuard.empty() &&
                ownerGuard.find("g_ownerContext.load(std::memory_order_acquire) == nullptr") <
                    ownerGuard.find("g_ownerThreadToken.load(std::memory_order_acquire)") &&
                ownerGuard.find("g_threadToken == 0") != std::string::npos &&
                ownerGuard.find("threadToken()") == std::string::npos &&
                ownerGuard.find("fetch_add") == std::string::npos &&
                ownerGuard.find("g_threadToken != owner") < ownerGuard.find("return g_configured && g_apiSampleFrame"),
                "owner-thread getter checks atomic registration and existing TLS before sample flags without allocating identity");
    ok &= check(!boundary.empty() && boundary.find("const uintptr_t ownerToken = threadToken();") != std::string::npos &&
                boundary.find("const uintptr_t previousOwner = g_ownerThreadToken.load(std::memory_order_relaxed);") != std::string::npos &&
                boundary.find("if (previousOwner != ownerToken)") < boundary.find("g_ownerContext.load(std::memory_order_acquire)") &&
                boundary.find("if (previousOwner != publishedOwner)") < boundary.find("g_ownerThreadToken.store(publishedOwner"),
                "owner boundary uses one steady-state token load and accesses context/stores publication only on transfer");
    ok &= check(!registration.empty() &&
                registration.find("g_ownerThreadToken.store(0") <
                    registration.find("g_ownerContext.store(context"),
                "cold owner registration invalidates the old thread before publishing a context");
    ok &= check(!configure.empty() && configure.find("g_ownerContext") == std::string::npos &&
                configure.find("g_ownerThreadToken") == std::string::npos,
                "collector reconfiguration preserves the cold context and owner-thread registration");
    ok &= check(!shutdown.empty() && shutdown.find("g_ownerThreadToken.store(0") != std::string::npos &&
                shutdown.find("g_ownerContext.store(nullptr") != std::string::npos,
                "collector shutdown clears both owner-thread publication and registered context");
    return ok;
}

bool collectorLifecycleChecks() {
    std::ifstream perfInput("src/d3d11/perf_monitor.cpp", std::ios::binary);
    std::ifstream screenInput("src/d3d11/vscreen.cpp", std::ios::binary);
    if (!perfInput || !screenInput)
        return check(false, "production lifecycle sources are available for collector wiring checks");
    const std::string perf((std::istreambuf_iterator<char>(perfInput)), std::istreambuf_iterator<char>());
    const std::string screen((std::istreambuf_iterator<char>(screenInput)), std::istreambuf_iterator<char>());
    bool ok = true;

    const std::string frame = functionBody(perf, "void perfMonitorFrame(");
    const std::size_t drain = frame.find("edvrPluginCostFrameBoundary(");
    const std::size_t publish = frame.find("detail::g_pluginCostApiSampleFrame = nextSampleFrame;");
    const std::size_t reset = frame.find("s.drawWholeTicks = s.drawRealTicks = 0;");
    ok &= check(!frame.empty() && drain < publish && publish < reset &&
                frame.find("const bool closedCpuSampleFrame = detail::g_perfMonitorSampleDraws;") < drain &&
                frame.find("const bool closedApiSampleFrame = detail::g_pluginCostApiSampleFrame;") < drain,
                "frame boundary drains closed CPU/API flags before publishing next API sample and resetting draw totals");

    const std::string install = functionBody(screen, "void installVScreenFixes(");
    const std::size_t commit = install.find("if (!s.hook.commit())");
    const std::size_t failedReturn = install.find("return;", commit);
    const std::size_t configure = install.find("perfMonitorPluginCostConfigure(");
    const std::size_t ownerContext = install.find("edvrPluginCostSetOwnerContext(ctx);");
    ok &= check(!install.empty() && commit < failedReturn && failedReturn < configure &&
                configure < ownerContext,
                "collector config then registers the known owner context only after successful hook commit");

    const std::string shutdown = functionBody(screen, "void shutdownVScreenFixes(");
    const std::size_t uninstall = shutdown.find("g_state->hook.uninstall();");
    const std::size_t stop = shutdown.find("perfMonitorPluginCostShutdown();", uninstall);
    ok &= check(!shutdown.empty() && uninstall < stop,
                "collector shutdown follows hook uninstall and clears owner registration after callbacks quiesce");
    return ok;
}

bool ownerContextChecks() {
    bool ok = true;
    EdvrPluginCostWindowV1 window{};
    int ownerContext = 0;
    int otherContext = 0;
    edvrPluginCostShutdown();
    edvrPluginCostConfigure(1u, 1000000u);
    ok &= check(edvrPluginCostApiSampleOwnerThread() == 0,
                "owner-thread getter rejects an unregistered collector during bootstrap");
    edvrPluginCostSetOwnerContext(&ownerContext);
    ok &= check(edvrPluginCostApiSampleContext(&ownerContext) == 0 &&
                edvrPluginCostApiSampleContext(&otherContext) == 0 &&
                edvrPluginCostApiSampleOwnerThread() == 0,
                "registered context alone does not sample before an owner frame boundary");

    // Configuration discards its first close, but that existing boundary is
    // still the owner-thread publication point and opens the next API sample.
    ok &= check(edvrPluginCostFrameBoundary(1u, 0u, 0u, 1u, 0u, &window) == 0 &&
                edvrPluginCostApiSampleContext(&ownerContext) == 1 &&
                edvrPluginCostApiSampleOwnerThread() == 1,
                "owner frame boundary publishes render-thread identity before enabling the next sample");
    edvrPluginCostConfigure(1u, 1000000u);
    ok &= check(edvrPluginCostApiSampleContext(&ownerContext) == 0 &&
                edvrPluginCostApiSampleOwnerThread() == 0,
                "collector reconfiguration preserves owner registration but closes API sampling");
    edvrPluginCostSetApiSampleFrame(1u);
    ok &= check(edvrPluginCostApiSampleContext(&ownerContext) == 1,
                "collector reconfiguration preserves the published owner-thread identity");
    edvrPluginCostConfigure(1u, 1000000u);
    (void)edvrPluginCostFrameBoundary(2u, 0u, 0u, 1u, 0u, &window);
    ok &= check(edvrPluginCostApiSampleContext(&ownerContext) == 1,
                "the next owner boundary reopens sampling after configuration discards its first close");
    edvrPluginCostSetApiSampleFrame(0u);
    ok &= check(edvrPluginCostApiSampleContext(&ownerContext) == 0 &&
                edvrPluginCostApiSampleOwnerThread() == 0,
                "both sample getters reject a closed API-sample frame");
    // The next frame remains closed when its next-sample flag is zero.
    // A non-report boundary does not populate window; denominator coverage
    // below uses a complete mixed sampled/unsampled window instead.
    (void)edvrPluginCostFrameBoundary(4u, 0u, 0u, 0u, 0u, &window);
    ok &= check(edvrPluginCostApiSampleOwnerThread() == 0,
                "closed API frame leaves owner-thread sampling disabled");
    edvrPluginCostSetApiSampleFrame(1u);
    ok &= check(edvrPluginCostApiSampleContext(&ownerContext) == 1 &&
                edvrPluginCostApiSampleOwnerThread() == 1,
                "owner getters accept only the open API-sample frame");

    std::atomic<uint8_t> foreignAccepted{0};
    std::atomic<uint8_t> foreignContextAccepted{0};
    std::thread foreign([&] {
        foreignContextAccepted.store(edvrPluginCostApiSampleContext(&ownerContext),
                                     std::memory_order_relaxed);
        foreignAccepted.store(edvrPluginCostApiSampleOwnerThread(),
                              std::memory_order_relaxed);
    });
    foreign.join();
    ok &= check(foreignAccepted.load(std::memory_order_relaxed) == 0 &&
                foreignContextAccepted.load(std::memory_order_relaxed) == 0,
                "a different thread with the same context pointer cannot use the owner sample flag");

    // Simulate a quiescent owner transfer: registration clears publication,
    // then the next owner-only boundary establishes the new thread token.
    edvrPluginCostSetOwnerContext(&ownerContext);
    ok &= check(edvrPluginCostApiSampleContext(&ownerContext) == 0,
                "re-registering a context invalidates its previous owner-thread token");
    std::atomic<uint8_t> newOwnerAccepted{0};
    std::atomic<uint8_t> newOwnerContextAccepted{0};
    std::thread newOwner([&] {
        EdvrPluginCostWindowV1 local{};
        (void)edvrPluginCostFrameBoundary(3u, 0u, 0u, 1u, 0u, &local);
        newOwnerContextAccepted.store(edvrPluginCostApiSampleContext(&ownerContext),
                                      std::memory_order_relaxed);
        newOwnerAccepted.store(edvrPluginCostApiSampleOwnerThread(),
                               std::memory_order_relaxed);
    });
    newOwner.join();
    ok &= check(newOwnerAccepted.load(std::memory_order_relaxed) == 1 &&
                newOwnerContextAccepted.load(std::memory_order_relaxed) == 1 &&
                edvrPluginCostApiSampleContext(&ownerContext) == 0 &&
                edvrPluginCostApiSampleOwnerThread() == 0,
                "owner transfer accepts the new frame thread and rejects the previous thread");

    edvrPluginCostSetOwnerContext(nullptr);
    ok &= check(edvrPluginCostApiSampleContext(&ownerContext) == 0 &&
                edvrPluginCostApiSampleOwnerThread() == 0,
                "clearing the owner context disables the API sample getter");
    edvrPluginCostConfigure(1u, 1000000u);
    edvrPluginCostSetOwnerContext(&ownerContext);
    (void)edvrPluginCostFrameBoundary(0u, 0u, 0u, 1u, 0u, &window);
    bool completed = false;
    for (uint32_t frame = 1; frame <= pc::kWindowFrameCount; ++frame) {
        const uint8_t sampled = frame == 1 || frame == 3 ? 1u : 0u;
        const uint8_t nextSample = frame == 2 ? 1u : 0u;
        completed = edvrPluginCostFrameBoundary(frame, 0u, sampled, nextSample, 0u, &window) != 0;
        ok &= check(completed == (frame == pc::kWindowFrameCount),
                    "mixed API window reports only at its final boundary");
    }
    ok &= check(completed && window.completedApiSampleFrames == 2,
                "complete API report counts sampled empty frames and excludes unsampled frames");
    edvrPluginCostShutdown();
    ok &= check(edvrPluginCostApiSampleContext(&ownerContext) == 0 &&
                edvrPluginCostApiSampleOwnerThread() == 0,
                "collector shutdown clears owner context and thread publication");
    return ok;
}

bool collectorChecks() {
    constexpr uint64_t kFrequency = 64000000;
    constexpr uint8_t kProfile = 1;
    bool ok = true;
    EdvrPluginCostWindowV1 window{};

    edvrPluginCostShutdown();
    edvrPluginCostConfigure(kProfile, kFrequency);
    // Configure can occur mid-frame. The first boundary is discarded, while
    // its explicit next flag arms API notes for the following complete frame.
    ok &= check(!edvrPluginCostFrameBoundary(900, 1, 1, 1, 0, &window),
                "configure discards the first potentially partial frame");

    bool completed = false;
    uint32_t finalFrame = 0;
    for (uint32_t step = 0; step < 1801 && !completed; ++step) {
        const uint32_t frame = 901 + step;
        const bool cpu = frame == 901 || frame == 902 || frame == 903 || frame == 904;
        if (frame == 901) {
            edvrPluginCostNoteSite(static_cast<uint8_t>(pc::Owner::CockpitVisuals), 5,
                                   static_cast<uint8_t>(pc::SiteEvent::Reached));
            edvrPluginCostNoteSite(static_cast<uint8_t>(pc::Owner::CockpitVisuals), 5,
                                   static_cast<uint8_t>(pc::SiteEvent::Invoked));
            edvrPluginCostNoteCpuTicks(static_cast<uint8_t>(pc::Owner::CockpitVisuals), 5, 100);
        } else if (frame == 902) {
            edvrPluginCostNoteSite(static_cast<uint8_t>(pc::Owner::CockpitVisuals), 6,
                                   static_cast<uint8_t>(pc::SiteEvent::Reached));
            edvrPluginCostNoteSite(static_cast<uint8_t>(pc::Owner::CockpitVisuals), 6,
                                   static_cast<uint8_t>(pc::SiteEvent::NotEligible));
        } else if (frame == 903) {
            // A suppressed trace frame must not contaminate the CPU estimate.
            edvrPluginCostMarkTraceSuppressed();
            edvrPluginCostNoteSite(static_cast<uint8_t>(pc::Owner::CockpitVisuals), 9,
                                   static_cast<uint8_t>(pc::SiteEvent::Invoked));
            edvrPluginCostNoteCpuTicks(static_cast<uint8_t>(pc::Owner::CockpitVisuals), 9, 999);
        } else if (frame == 904) {
            // A real timed scope with a measured zero is distinct from no row.
            edvrPluginCostNoteSite(static_cast<uint8_t>(pc::Owner::CockpitVisuals), 10,
                                   static_cast<uint8_t>(pc::SiteEvent::Reached));
            edvrPluginCostNoteSite(static_cast<uint8_t>(pc::Owner::CockpitVisuals), 10,
                                   static_cast<uint8_t>(pc::SiteEvent::Invoked));
            edvrPluginCostNoteCpuTicks(static_cast<uint8_t>(pc::Owner::CockpitVisuals), 10, 0);
        }

        // API sampling is controlled only by the preceding boundary's next
        // flag. The two calls on frame 901 and one on the trace frame are raw
        // counts; none is scaled by the CPU stride.
        if (frame == 901) {
            edvrPluginCostNoteD3dCall(static_cast<uint8_t>(pc::Owner::CockpitVisuals), 20,
                                      static_cast<uint8_t>(pc::ApiClass::ReadQuery));
            edvrPluginCostNoteD3dCall(static_cast<uint8_t>(pc::Owner::CockpitVisuals), 20,
                                      static_cast<uint8_t>(pc::ApiClass::ReadQuery));
        } else if (frame == 903) {
            edvrPluginCostNoteD3dCall(static_cast<uint8_t>(pc::Owner::CockpitVisuals), 21,
                                      static_cast<uint8_t>(pc::ApiClass::Work));
        } else if (frame == 904) {
            edvrPluginCostNoteD3dCall(static_cast<uint8_t>(pc::Owner::CockpitVisuals), 22,
                                      static_cast<uint8_t>(pc::ApiClass::Transfer));
            edvrPluginCostNoteD3dCall(static_cast<uint8_t>(pc::Owner::CockpitVisuals), 23,
                                      static_cast<uint8_t>(pc::ApiClass::State));
            edvrPluginCostNoteD3dCall(static_cast<uint8_t>(pc::Owner::CockpitVisuals), 24,
                                      static_cast<uint8_t>(pc::ApiClass::Instrumentation));
            // Pin the high API-site word used by the completed-window report.
            edvrPluginCostNoteD3dCall(static_cast<uint8_t>(pc::Owner::CockpitVisuals), 72,
                                      static_cast<uint8_t>(pc::ApiClass::State));
        }

        completed = edvrPluginCostFrameBoundary(frame, cpu ? 1 : 0, 1, 1, 0, &window);
        if (completed) finalFrame = frame;
    }
    ok &= check(completed, "collector closes a fixed 1800-frame report window");
    ok &= check(window.version == 1 && window.profileBit == kProfile &&
                window.firstFrame == 901 && window.lastFrame == finalFrame,
                "report identifies profile and exact closed-frame window");
    ok &= check(window.completedCpuSampleFrames == 3 &&
                window.cpuTraceSuppressedFrames == 1,
                "CPU denominator uses closed unsuppressed sample frames only");
    ok &= check(window.completedApiSampleFrames == 1800,
                "API denominator counts explicit closed sample flags, not CPU stride");

    const auto& cockpit = window.owners[static_cast<uint8_t>(pc::Owner::CockpitVisuals)];
    ok &= check(cockpit.cpuObserved == 1 && cockpit.cpuTimedScopes == 2 &&
                cockpit.cpuReached == 3 && cockpit.cpuInvoked == 2 &&
                cockpit.cpuNotEligible == 1,
                "CPU report separates reached, eligible invocations, and declines");
    ok &= check(cockpit.cpuSiteMask[0] == ((uint64_t{1} << 5) | (uint64_t{1} << 6) |
                                          (uint64_t{1} << 10)) &&
                cockpit.cpuSiteMask[1] == 0,
                "trace-suppressed CPU observations do not enter site coverage");
    const double expectedMeanMs = (100.0 * 64.0 * 1000.0 / double(kFrequency)) / 3.0;
    ok &= check(std::abs(cockpit.cpuMeanMs - expectedMeanMs) < 1e-12 &&
                cockpit.cpuStdDevMs > 0.0,
                "CPU sample statistics scale ticks by 64 and include zero frames");
    ok &= check(cockpit.apiObserved == 1 && cockpit.apiCalls[static_cast<uint8_t>(pc::ApiClass::ReadQuery)] == 2 &&
                cockpit.apiCalls[static_cast<uint8_t>(pc::ApiClass::Work)] == 1 &&
                cockpit.apiCalls[static_cast<uint8_t>(pc::ApiClass::Transfer)] == 1 &&
                cockpit.apiCalls[static_cast<uint8_t>(pc::ApiClass::State)] == 2 &&
                cockpit.apiCalls[static_cast<uint8_t>(pc::ApiClass::Instrumentation)] == 1 &&
                cockpit.apiSiteMask[0] == ((uint64_t{1} << 20) | (uint64_t{1} << 21) |
                                           (uint64_t{1} << 22) | (uint64_t{1} << 23) |
                                           (uint64_t{1} << 24)) &&
                cockpit.apiSiteMask[1] == (uint64_t{1} << (72 - 64)),
                "API calls are counted once without CPU sampling scale and aggregate site IDs across both mask words");

    // A separate configured window with no owner activity must not invent a
    // zero row. A reached-only owner is observed even when measured ticks are 0.
    edvrPluginCostConfigure(kProfile, kFrequency);
    ok &= check(!edvrPluginCostFrameBoundary(finalFrame + 1, 0, 0, 0, 0, &window),
                "reconfigure resets partial state and skips the first boundary");
    completed = false;
    for (uint32_t step = 0; step < 1801 && !completed; ++step) {
        const uint32_t frame = finalFrame + 2 + step;
        if (frame == finalFrame + 2) {
            edvrPluginCostNoteSite(static_cast<uint8_t>(pc::Owner::Core), 3,
                                   static_cast<uint8_t>(pc::SiteEvent::Reached));
            edvrPluginCostNoteSite(static_cast<uint8_t>(pc::Owner::Core), 3,
                                   static_cast<uint8_t>(pc::SiteEvent::Invoked));
            edvrPluginCostNoteCpuTicks(static_cast<uint8_t>(pc::Owner::Core), 3, 0);
        }
        completed = edvrPluginCostFrameBoundary(frame, frame == finalFrame + 2 ? 1 : 0,
                                                 0, 0, 0, &window);
    }
    ok &= check(completed && window.owners[static_cast<uint8_t>(pc::Owner::Core)].cpuObserved == 1 &&
                window.owners[static_cast<uint8_t>(pc::Owner::Core)].cpuMeanMs == 0.0 &&
                window.owners[static_cast<uint8_t>(pc::Owner::CockpitVisuals)].cpuObserved == 0 &&
                window.owners[static_cast<uint8_t>(pc::Owner::CockpitVisuals)].apiObserved == 0,
                "measured zero is reported while owners with no observations remain absent");

    edvrPluginCostShutdown();
    edvrPluginCostShutdown();
    edvrPluginCostNoteD3dCall(static_cast<uint8_t>(pc::Owner::CockpitVisuals), 1,
                              static_cast<uint8_t>(pc::ApiClass::State));
    return ok;
}

bool drawCpuWindowProductionChecks() {
    std::ifstream input("src/d3d11/perf_monitor.cpp", std::ios::binary);
    std::ifstream screenInput("src/d3d11/vscreen.cpp", std::ios::binary);
    if (!input || !screenInput)
        return check(false, "draw CPU window production sources are available");
    const std::string source((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    const std::string screen((std::istreambuf_iterator<char>(screenInput)), std::istreambuf_iterator<char>());
    bool ok = true;

    const std::string frame = functionBody(source, "void perfMonitorFrame(");
    const std::string ticks = functionBody(source, "void perfMonitorDrawTicks(");
    const std::string active = functionBody(source, "void perfMonitorSetActive(");
    const std::string configure = functionBody(source, "void perfMonitorPluginCostConfigure(");
    const std::size_t estimate = frame.find("(s.drawWholeTicks - s.drawRealTicks) *");
    const std::size_t estimateStride = frame.find(
        "static_cast<int64_t>(kPerfMonitorDrawTimeStride);");
    const std::size_t closeValid = frame.find("s.drawCpuWindow.closeFrame(true, s.drawWholeTicks, s.drawRealTicks);");
    const std::size_t report = frame.find("draw hook CPU: 1800-frame window ending");
    const std::size_t reset = frame.find("s.drawCpuWindow.resetWindow();");
    const std::size_t totalsReset = frame.find("s.drawWholeTicks = s.drawRealTicks = 0;");
    ok &= check(!frame.empty() && estimate != std::string::npos &&
                estimateStride != std::string::npos && estimate < estimateStride &&
                closeValid != std::string::npos &&
                estimate < closeValid && closeValid < report && report < reset && reset < totalsReset,
                "existing scaled frame estimate is preserved and per-draw stats close/reset at the same report boundary");
    ok &= check(frame.find("detail::g_perfMonitorSampleDraws && qpcFrequency() > 0") != std::string::npos &&
                frame.find("s.drawCpuWindow.closeFrame(false, 0, 0);") != std::string::npos &&
                frame.find("s.drawCpuWindow.closeFrame(false, 0, 0);") < totalsReset,
                "unsampled or invalid-frequency frames discard their denominator");
    ok &= check(!ticks.empty() && ticks.find("s.drawCpuWindow.noteDraw(wholeTicks);") != std::string::npos &&
                ticks.find("s.drawCpuWindow.noteDraw(wholeTicks);") >
                    ticks.find("if (realTicks > 0) g_s.drawRealTicks += realTicks;"),
                "draw denominator comes only from existing timed callbacks");
    ok &= check(!active.empty() && active.find("drawCpuWindow") == std::string::npos &&
                !configure.empty() && configure.find("drawCpuWindow") == std::string::npos,
                "monitor activity/configuration changes do not reset the always-on draw window");
    ok &= check(screen.find("if (on) perfMonitorDrawTicks(qpcNow() - t0, real);") != std::string::npos,
                "timed sample denominator uses the unchanged DrawClock selection and callbacks");
    ok &= check(frame.find("per timed draw sample across %llu samples") != std::string::npos &&
                frame.find("per-timed-draw mean unavailable (%llu valid timed draw samples)") != std::string::npos,
                "report labels timed-draw mean and distinguishes no sample from measured zero");
    return ok;
}

bool run(bool full) {
    bool ok = policyChecks() && drawCpuWindowChecks();
    if (full) {
        ok &= collectorChecks();
        ok &= nightVisionAnnotationChecks();
        ok &= targetSharpRemlokAnnotationChecks();
        ok &= productionCpuRouteChecks();
        ok &= collectorHotPathChecks();
        ok &= collectorLifecycleChecks();
        ok &= ownerContextChecks();
        ok &= drawCpuWindowProductionChecks();
        ok &= collectorLifecycleChecks();
    }
    std::puts(ok ? "plugin_cost_test: PASS" : "plugin_cost_test: FAILED");
    return ok;
}

int main(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "--dry-run") == 0) return run(true) ? 0 : 1;
    if (argc == 2 && std::strcmp(argv[1], "--self-test") == 0) return run(true) ? 0 : 1;
    std::puts("Usage: plugin_cost_test --dry-run | --self-test");
    return 2;
}
