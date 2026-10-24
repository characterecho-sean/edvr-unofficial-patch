#pragma once
// Flat-only bookkeeping the VR profile does not pay for: the setter-class issue-epoch bumps (src/d3d11/flat_issue_epoch.h) and
// the map-bounce window notes (flatRuntimeMapBounceNoteMap / NoteKind, called from vscreen.cpp hookedMap).
//
// Behaviour (driven through the real header and the real profile global): the four setter-class bumps do nothing in the VR
// profiles, bump in Flat, Passthrough and Invalid, and Revoke / NoteResource are never gated. A live switch is walked with the
// real certificate type: the clear that leaving Flat performs is what keeps a skipped VR bump from touching a reusable proof,
// and the control shows the proof would survive without it.
//
// Source (the call sites are in files this rig does not compile): every Map note is behind the VR test, the leases and the
// bounce install are not, nothing but the report reads the note counters, the epoch's generic bump has no caller of its own,
// the unconditional revokes stay unconditional, the certificate is stamped in one place that only a flat draw scope reaches,
// and mode_watch.cpp tears Flat down before it flips the profile.
#include "../../src/d3d11/flat_forward_light.h"
#include "../../src/d3d11/flat_issue_epoch.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <string>
#include <system_error>
#include <vector>

namespace issue_epoch_gate_test {
inline std::string slurp(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
    return text;
}
inline std::string compact(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) if (c != ' ' && c != '\t' && c != '\n') out.push_back(c);
    return out;
}
inline unsigned count(const std::string& hay, const std::string& needle) {
    unsigned n = 0;
    for (auto at = hay.find(needle); at != std::string::npos; at = hay.find(needle, at + needle.size())) ++n;
    return n;
}
// From the first `open` to the next closing brace at column 0.
inline std::string bodyOf(const std::string& text, const std::string& open) {
    const auto at = text.find(open);
    if (at == std::string::npos) return {};
    const auto end = text.find("\n}\n", at);
    return text.substr(at, end == std::string::npos ? std::string::npos : end - at);
}
inline std::string lineOf(const std::string& text, const std::string& open) {
    const auto at = text.find(open);
    if (at == std::string::npos) return {};
    const auto end = text.find('\n', at);
    return text.substr(at, end == std::string::npos ? std::string::npos : end - at);
}

// The blocks open at byte `at` of C++ text, outermost first, each as its header with the whitespace removed: the code between the
// previous `;`, `{` or `}` and the `{` ("if(runtimeFlatProfile())", "else", "voidhookedDraw(...)"). Comments, string and character
// literals and preprocessor lines are skipped, so a brace inside one neither opens nor closes a block. `code` is false when `at`
// itself lies inside a comment, a literal or a preprocessor line.
struct OpenBlocks {
    std::vector<std::string> headers;
    bool code = true;
};
inline OpenBlocks openBlocksAt(const std::string& text, size_t at) {
    OpenBlocks out;
    std::string pending;
    bool lineStart = true;
    const size_t limit = at < text.size() ? at : text.size();
    for (size_t i = 0; i < limit; ++i) {
        const char c = text[i];
        const char next = i + 1 < text.size() ? text[i + 1] : '\0';
        if (c == '\n') { lineStart = true; continue; }
        if (c == ' ' || c == '\t') continue;
        const bool atLineStart = lineStart;
        lineStart = false;
        if (c == '#' && atLineStart) {   // a preprocessor line, with its backslash continuations
            size_t end = i;
            while (end < text.size() && (text[end] != '\n' || (end > 0 && text[end - 1] == '\\'))) ++end;
            if (end >= limit) { out.code = false; return out; }
            i = end - 1;
            continue;
        }
        if (c == '/' && next == '/') {
            const size_t end = text.find('\n', i);
            if (end == std::string::npos || end >= limit) { out.code = false; return out; }
            i = end - 1;
            continue;
        }
        if (c == '/' && next == '*') {
            const size_t end = text.find("*/", i + 2);
            if (end == std::string::npos || end + 2 > limit) { out.code = false; return out; }
            i = end + 1;
            continue;
        }
        // A quote after a letter or digit is a digit separator (1'000), not the start of a character literal.
        const bool charLiteral = c == '\'' && !(i > 0 && std::isalnum(static_cast<unsigned char>(text[i - 1])));
        if (c == '"' || charLiteral) {
            size_t j = i + 1;
            while (j < text.size() && text[j] != c && text[j] != '\n') j += text[j] == '\\' ? 2 : 1;
            if (j >= limit) { out.code = false; return out; }
            i = j;
            continue;
        }
        if (c == '{') { out.headers.push_back(pending); pending.clear(); continue; }
        if (c == '}') { if (!out.headers.empty()) out.headers.pop_back(); pending.clear(); continue; }
        if (c == ';') { pending.clear(); continue; }
        pending.push_back(c);
    }
    return out;
}
// Whether a block chain (openBlocksAt's headers) puts its innermost position in the true branch of `if (runtimeFlatProfile())`:
// walking outward from the innermost block, the first block whose header is exactly that test, with no `else` branch and no other
// profile test on the way. A position in `} else {` of that if, after its block closed, or under `if (!runtimeFlatProfile())` is not.
inline bool inFlatProfileBranch(const std::vector<std::string>& headers) {
    for (size_t i = headers.size(); i-- > 0;) {
        const std::string& h = headers[i];
        if (h == "if(runtimeFlatProfile())") return true;
        if (h.compare(0, 4, "else") == 0) return false;
        if (h.find("runtimeFlatProfile") != std::string::npos || h.find("runtimeVrProfile") != std::string::npos) return false;
    }
    return false;
}
// The FlatRuntimeDrawScope constructions in vscreen.cpp text (comments and literals excluded), and how many sit in the true branch of
// an `if (runtimeFlatProfile())`.
inline unsigned flatDrawScopes(const std::string& text, unsigned* guarded) {
    unsigned total = 0;
    *guarded = 0;
    const std::string scope = "FlatRuntimeDrawScope flatDraw(";
    for (auto at = text.find(scope); at != std::string::npos; at = text.find(scope, at + 1)) {
        const OpenBlocks open = openBlocksAt(text, at);
        if (!open.code) continue;
        ++total;
        if (inFlatProfileBranch(open.headers)) ++*guarded;
    }
    return total;
}

inline int run() {
    unsigned checks = 0;
    int failures = 0;
    const auto expect = [&](bool ok, const char* why) {
        ++checks;
        if (!ok) { ++failures; std::printf("FAIL: issue epoch gate %s\n", why); }
    };
    using edvr::RuntimeProfile;

    // ---- Behaviour ------------------------------------------------------------------------------------------------------------
    const RuntimeProfile saved = edvr::runtimeProfileValue();
    const auto bumpAll = [] {
        edvr::flatIssueEpochBumpDepth();
        edvr::flatIssueEpochBumpStage();
        edvr::flatIssueEpochBumpRt();
        edvr::flatIssueEpochBumpCb();
    };
    edvr::flatIssueEpochTakeWindow();   // the counters are process-wide; start this walk from empty
    for (const RuntimeProfile p : {RuntimeProfile::Vr, RuntimeProfile::LegacyVr}) {
        edvr::runtimeProfileSet(p);
        const uint64_t before = edvr::flatIssueEpochLoad();
        bumpAll();
        const edvr::FlatIssueEpochWindow w = edvr::flatIssueEpochTakeWindow();
        expect(edvr::flatIssueEpochLoad() == before && !w.depthSets && !w.stageSets && !w.rtSets && !w.cb1Sets,
               "the VR profiles skip every setter-class bump: the epoch and the four counters stay where they were");
    }
    for (const RuntimeProfile p : {RuntimeProfile::Flat, RuntimeProfile::Passthrough, RuntimeProfile::Invalid}) {
        edvr::runtimeProfileSet(p);
        const uint64_t before = edvr::flatIssueEpochLoad();
        bumpAll();
        const edvr::FlatIssueEpochWindow w = edvr::flatIssueEpochTakeWindow();
        expect(edvr::flatIssueEpochLoad() == before + 4 && w.depthSets == 1 && w.stageSets == 1 && w.rtSets == 1 && w.cb1Sets == 1,
               "Flat, Passthrough and Invalid still bump the epoch and their own counter once per setter-class call");
    }
    edvr::runtimeProfileSet(RuntimeProfile::Vr);
    {
        const uint64_t before = edvr::flatIssueEpochLoad();
        edvr::flatIssueEpochRevoke();
        const edvr::FlatIssueEpochWindow w = edvr::flatIssueEpochTakeWindow();
        expect(edvr::flatIssueEpochLoad() == before + 1 && w.revokes == 1,
               "Revoke is not gated: ClearState, command lists and the frame reset still move the epoch in VR");
        const void* const watched = reinterpret_cast<const void*>(static_cast<uintptr_t>(0x1000));
        const void* const other = reinterpret_cast<const void*>(static_cast<uintptr_t>(0x2000));
        edvr::flatIssueEpochPublishBuffer(reinterpret_cast<uintptr_t>(watched));
        const uint64_t beforeNote = edvr::flatIssueEpochLoad();
        edvr::flatIssueEpochNoteResource(other);
        expect(edvr::flatIssueEpochLoad() == beforeNote, "NoteResource leaves the epoch alone for a buffer no proof names");
        edvr::flatIssueEpochNoteResource(watched);
        const edvr::FlatIssueEpochWindow wn = edvr::flatIssueEpochTakeWindow();
        expect(edvr::flatIssueEpochLoad() == beforeNote + 1 && wn.cbWrites == 1,
               "NoteResource is not gated: a write of the watched buffer still moves the epoch in VR");
        edvr::flatIssueEpochRevoke();
        edvr::flatIssueEpochTakeWindow();
    }

    // A live Flat -> VR -> Flat switch, with the real certificate type and the real epoch.
    {
        namespace fl = edvr::forward_light;
        fl::Surface surface{};
        surface.resource = 11; surface.width = 1920; surface.height = 1080; surface.slices = 1; surface.samples = 1; surface.known = true;
        fl::ConstantIdentity id{};
        id.buffer = 0x1000; id.generation = 3; id.mutation = 4; id.writeGeneration = 5; id.bank = 6; id.writeFrame = 7;
        id.first = 0; id.count = 8; id.width = 256; id.writeQ = 9; id.bytes = 96; id.hash = 0xABCDu;
        fl::Issue issue{};
        issue.kind = fl::Issue::Geometry; issue.frame = 7; issue.vs = 11; issue.ps = 22; issue.depthWrite = true;
        issue.raw = surface; issue.constants[0] = id;
        const auto stamp = [&](fl::GeometryReuseProof& proof) { fl::geometryCertStamp(proof, issue, edvr::flatIssueEpochLoad(), 99); };
        const auto reusable = [&](const fl::GeometryReuseProof& proof, uint64_t frame) {
            return fl::geometryIssueReuse(proof, issue, edvr::flatIssueEpochLoad(), frame, 0.0f, 0.0f, edvr::flatIssueEpochFrozen());
        };

        edvr::runtimeProfileSet(RuntimeProfile::Flat);
        fl::GeometryReuseProof proof{};
        stamp(proof);
        expect(reusable(proof, 7), "a proof stamped in Flat is reusable at the epoch and frame it was stamped in");
        expect(!reusable(proof, 8), "reuse is bound to the frame the proof was stamped in");
        edvr::flatIssueEpochBumpStage();
        expect(!reusable(proof, 7), "a setter in Flat moves the epoch and retires the proof");

        // Leaving Flat, in commitPosted's order: the certificate is cleared and the epoch revoked while Flat is still the profile
        // (flatRuntimeResize -> forwardReset -> geometryCertClear), then the profile flips, then VR runs and skips every bump.
        stamp(proof);
        proof = {};
        edvr::flatIssueEpochRevoke();
        edvr::runtimeProfileSet(RuntimeProfile::Vr);
        bumpAll();
        edvr::runtimeProfileSet(RuntimeProfile::Flat);
        expect(!reusable(proof, 7), "after a VR stretch no proof is reusable: the teardown cleared it before the bumps were skipped");
        stamp(proof);
        expect(reusable(proof, 7), "a fresh stamp after the return to Flat is reusable again");
        edvr::flatIssueEpochBumpRt();
        expect(!reusable(proof, 7), "and Flat's bumps retire it again");

        // Control: without the teardown a proof survives the skipped bumps. That is why the clear has to precede the flip.
        fl::GeometryReuseProof held{};
        stamp(held);
        edvr::runtimeProfileSet(RuntimeProfile::Vr);
        bumpAll();
        edvr::runtimeProfileSet(RuntimeProfile::Flat);
        expect(reusable(held, 7), "control: a proof that was not cleared would survive the skipped VR bumps");
        edvr::flatIssueEpochRevoke();
        edvr::flatIssueEpochTakeWindow();
    }
    edvr::runtimeProfileSet(saved);

    // ---- Source ---------------------------------------------------------------------------------------------------------------
    const std::string epochH = slurp("src/d3d11/flat_issue_epoch.h");
    const std::string vscreenRaw = slurp("src/d3d11/vscreen.cpp");
    const std::string vscreen = compact(vscreenRaw);
    const std::string runtimeCpp = slurp("src/d3d11/flat_runtime.cpp");
    const std::string forwardInl = slurp("src/d3d11/flat_forward_light_runtime.inl");
    const std::string modeWatch = slurp("src/d3d11/mode_watch.cpp");
    const std::string deviceHook = slurp("src/d3d11/device_hook.cpp");
    expect(!epochH.empty() && !vscreenRaw.empty() && !runtimeCpp.empty() && !forwardInl.empty() && !modeWatch.empty() && !deviceHook.empty(),
           "the epoch header, vscreen, flat runtime, forward-light adapter, mode watch and device hook sources are readable from the repo root");

    // The four setter-class bumps are gated, once, by the VR test; the generic bump has no caller of its own.
    const std::string counted = lineOf(epochH, "inline bool flatIssueEpochSettersCounted()");
    const std::string setter = bodyOf(epochH, "inline void flatIssueEpochBumpSetter(");
    expect(counted.find("return !runtimeVrProfile();") != std::string::npos && counted.find("runtimeFlatProfile") == std::string::npos &&
               setter.find("if (flatIssueEpochSettersCounted()) flatIssueEpochBump(counter);") != std::string::npos,
           "the setter-class gate is !runtimeVrProfile() (not 'not flat'), and the setter helper is the only caller of the generic bump");
    const struct { const char* fn; const char* counter; } setters[] = {
        {"inline void flatIssueEpochBumpDepth()", "depthSets"}, {"inline void flatIssueEpochBumpStage()", "stageSets"},
        {"inline void flatIssueEpochBumpRt()", "rtSets"},       {"inline void flatIssueEpochBumpCb()", "cb1Sets"}};
    bool allGated = true;
    for (const auto& s : setters) {
        const std::string line = lineOf(epochH, s.fn);
        allGated = allGated && line.find(std::string("flatIssueEpochBumpSetter(&flatIssueEpochState().") + s.counter + ")") != std::string::npos;
    }
    expect(allGated && count(epochH, "flatIssueEpochBump(") == 2,
           "depth, stage, render-target and CB1 bumps each go through the gated helper; the generic bump is defined and called once");
    unsigned strays = 0, scanned = 0;
    std::error_code ec;
    for (std::filesystem::recursive_directory_iterator it("src", ec), end; !ec && it != end; it.increment(ec)) {
        const auto& path = it->path();
        const std::string ext = path.extension().string();
        if (!it->is_regular_file(ec) || (ext != ".cpp" && ext != ".h" && ext != ".inl")) continue;
        ++scanned;
        if (path.filename() == "flat_issue_epoch.h") continue;
        strays += count(slurp(path), "flatIssueEpochBump(");
    }
    expect(scanned > 50 && strays == 0, "no source file outside flat_issue_epoch.h calls the ungated generic bump");
    const std::string revoke = bodyOf(epochH, "inline void flatIssueEpochRevoke()");
    const std::string note = bodyOf(epochH, "inline void flatIssueEpochNoteResource(");
    expect(!revoke.empty() && !note.empty() && revoke.find("runtimeVrProfile") == std::string::npos &&
               revoke.find("SettersCounted") == std::string::npos && note.find("runtimeVrProfile") == std::string::npos &&
               note.find("SettersCounted") == std::string::npos,
           "Revoke and NoteResource carry no profile test");
    expect(count(vscreen, "if(!foreignContext(self))flatIssueEpochRevoke();") == 4 &&
               count(vscreen, "flatIssueEpochRevoke();") == 4,
           "the four owner-context revokes in vscreen.cpp (discard view, swap state, ClearState, command list) are unconditional");
    expect(count(vscreen, "flatIssueEpochBumpStage();") >= 8 && count(vscreen, "flatIssueEpochBumpRt();") >= 3 &&
               count(vscreen, "flatIssueEpochBumpCb();") >= 2 && count(vscreen, "flatIssueEpochBumpDepth();") >= 1,
           "the setter hooks and EDVR's own Raw setters still reach the gated bumps");

    // Every Map note is behind the VR test; the leases and the install are not; only the report reads the counters.
    const std::string gate = "if(!runtimeVrProfile())";
    unsigned sites = 0, ungated = 0;
    for (const char* name : {"flatRuntimeMapBounceNoteMap(", "flatRuntimeMapBounceNoteKind("}) {
        for (auto at = vscreen.find(name); at != std::string::npos; at = vscreen.find(name, at + 1)) {
            ++sites;
            if (at < gate.size() || vscreen.compare(at - gate.size(), gate.size(), gate) != 0) ++ungated;
        }
    }
    expect(sites >= 5 && ungated == 0, "each of the five Map-bounce note calls in hookedMap sits directly behind !runtimeVrProfile()");
    unsigned leaseSites = 0, leaseGated = 0;
    for (const char* name : {"flatRuntimeMapBouncePreMap(", "flatRuntimeMapBounceInstall(", "flatRuntimeMapBounceBeginUnmap("}) {
        for (auto at = vscreen.find(name); at != std::string::npos; at = vscreen.find(name, at + 1)) {
            ++leaseSites;
            const size_t from = at > 40 ? at - 40 : 0;
            if (vscreen.substr(from, at - from).find("runtimeVrProfile") != std::string::npos) ++leaseGated;
        }
    }
    expect(leaseSites >= 3 && leaseGated == 0, "the Map-bounce pre-map drop, install and unmap lease are not behind any profile test");
    const std::string report = bodyOf(runtimeCpp, "void reportMapBounce(");
    expect(!report.empty() && count(runtimeCpp, "w.maps.load()") == 1 && count(runtimeCpp, "w.tracked.load()") == 1 &&
               count(runtimeCpp, "w.otherBuffer.load()") == 1 && count(runtimeCpp, "w.texture.load()") == 1 &&
               report.find("w.maps.load()") != std::string::npos,
           "the note counters are read in one place, reportMapBounce");
    expect(count(runtimeCpp, "reportMapBounce(frame,now,") == 2 &&
               runtimeCpp.find("void flatRuntimePresent(") < runtimeCpp.find("reportMapBounce(frame,now,true)") &&
               runtimeCpp.find("reportMapBounce(frame,now,false)") < runtimeCpp.find("void flatRuntimeSubstitution("),
           "reportMapBounce is called only from flatRuntimePresent");
    expect(count(deviceHook, "flatRuntimePresent(") == 1 &&
               deviceHook.find("if (runtimeFlatProfile()) flatRuntimePresent(self, g_state->frameCounter, hr, flags);") != std::string::npos,
           "flatRuntimePresent, and so the map-bounce report, runs only in the flat profile");

    // What makes the epoch gate safe: the certificate is stamped in one place a flat draw scope reaches, and leaving Flat clears it.
    const std::string valid = bodyOf(forwardInl, "bool forwardIssueValid(State& s,");
    expect(count(forwardInl, "geometryCertStamp(") == 1 && valid.find("geometryCertStamp(") != std::string::npos,
           "the Geometry certificate is stamped once, inside forwardIssueValid");
    unsigned scopesGuarded = 0;
    const unsigned scopes = flatDrawScopes(vscreenRaw, &scopesGuarded);
    expect(scopes >= 6 && scopes == scopesGuarded,
           "every flat draw scope in vscreen.cpp is constructed inside the true branch of an `if (runtimeFlatProfile())` block (brace-matched, no else on the way), so VR never stamps");
    // The scan must bite. Mutated copies of vscreen.cpp, in memory only (the source is never written), put the hookedDrawAuto scope where a
    // search for the first closing brace after the nearest `if (runtimeFlatProfile())` would still pass it, and where it must not pass.
    {
        const std::string anchor = "FlatRuntimeDrawScope flatDraw(self, 0, 'A');";
        const std::string opener = "    if (runtimeFlatProfile()) {\n";
        const size_t anchorAt = vscreenRaw.find(anchor);
        const size_t openerAt = anchorAt == std::string::npos ? std::string::npos : vscreenRaw.rfind(opener, anchorAt);
        const bool anchored = anchorAt != std::string::npos && count(vscreenRaw, anchor) == 1 && openerAt != std::string::npos &&
                              anchorAt - openerAt < 400 && openBlocksAt(vscreenRaw, anchorAt).headers.size() >= 1 &&
                              openBlocksAt(vscreenRaw, anchorAt).headers.back() == "if(runtimeFlatProfile())";
        expect(anchored, "the scan's controls find hookedDrawAuto's scope directly inside its flat block (an anchor of the controls moved)");
        if (anchored) {
            const auto mutated = [&](size_t at, size_t len, const std::string& with) {
                std::string text = vscreenRaw;
                text.replace(at, len, with);
                return text;
            };
            const auto guardedIn = [&](const std::string& text, unsigned* total) {
                unsigned g = 0;
                *total = flatDrawScopes(text, &g);
                return g;
            };
            unsigned total = 0, g = 0;
            g = guardedIn(mutated(anchorAt, anchor.size(), "{ " + anchor + " }"), &total);
            expect(total == scopes && g == total, "scan control: a scope in a bare block inside the flat block is still guarded");
            g = guardedIn(mutated(anchorAt, anchor.size(), "if (self == nullptr) {} else { " + anchor + " }"), &total);
            expect(total == scopes && g + 1 == total, "scan control: a scope in an else branch nested inside the flat block is not guarded");
            g = guardedIn(mutated(openerAt, opener.size(), opener + "    } else {\n"), &total);
            expect(total == scopes && g + 1 == total,
                   "scan control: a scope in the else branch of the flat test itself is not guarded (a search for the first `\\n    }\\n` after the opener passes it)");
            g = guardedIn(mutated(openerAt, opener.size(), opener + "    }\n    {\n"), &total);
            expect(total == scopes && g + 1 == total, "scan control: a scope after the flat block closed, in a bare block of its own, is not guarded");
            g = guardedIn(mutated(openerAt, opener.size(), "    if (!runtimeFlatProfile()) {\n"), &total);
            expect(total == scopes && g + 1 == total, "scan control: a scope under the negated test is not guarded");
            g = guardedIn(mutated(openerAt, opener.size(), "    // if (runtimeFlatProfile()) {\n    {\n"), &total);
            expect(total == scopes && g + 1 == total, "scan control: a flat test that is only a comment guards nothing");
            g = guardedIn(mutated(anchorAt, anchor.size(), "// " + anchor), &total);
            expect(total + 1 == scopes && g == total, "scan control: a scope inside a comment is not a construction");
        }
    }
    const std::string forgetCert = bodyOf(forwardInl, "void geometryCertClear() {");
    const std::string reset = bodyOf(forwardInl, "void forwardReset(State& s,");
    const std::string resize = bodyOf(runtimeCpp, "void flatRuntimeResize() {");
    expect(forgetCert.find("geometryCert()={};") != std::string::npos &&
               forgetCert.find("geometryCert()={};") < forgetCert.find("flatIssueEpochRevoke();") &&
               reset.find("geometryCertClear();") != std::string::npos && resize.find("forwardReset(s);") != std::string::npos,
           "flatRuntimeResize resets the forward-light state, which clears the certificate and revokes the epoch");
    const std::string commit = bodyOf(modeWatch, "void commitPosted(uint64_t packed) {");
    const auto leaveFlat = commit.find("if (cur == RuntimeProfile::Flat) {");
    const auto resizeAt = commit.find("flatRuntimeResize();", leaveFlat);
    const auto flipAt = commit.find("runtimeProfileSet(target);");
    expect(!commit.empty() && leaveFlat != std::string::npos && resizeAt != std::string::npos && flipAt != std::string::npos &&
               leaveFlat < resizeAt && resizeAt < flipAt && count(modeWatch, "runtimeProfileSet(") == 1,
           "the mode watch tears Flat down (flatRuntimeResize) before it flips the profile, and nothing else sets the profile");

    std::printf("issue epoch gate: %u checks, %d failed\n", checks, failures);
    return failures;
}
}  // namespace issue_epoch_gate_test
