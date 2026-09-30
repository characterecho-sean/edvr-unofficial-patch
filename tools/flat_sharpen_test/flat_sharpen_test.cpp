// flat_sharpen_test -- the flat profile's sharpening, without the game.
//
// Two halves, both gated by build.bat:
//
//  1. THE CONTRACT of src/d3d11/flat_sharpen.cpp against a stubbed pass (edvrSharpen is
//     defined here) on a WARP device: at strength 0 the resolve's own view comes back
//     and nothing is called; on, the view is over the pass's texture in the input
//     view's own format; the setting is read every frame and clamped; the two resolve
//     textures keep their own slots; one refusal stands the sharpening down; a view
//     that cannot be made, a deferred context and a non-2D view pass the frame
//     through. What each of those says in the log is read back from a real log file.
//
//  2. THE PANEL. Every key on the flat panel page (menu_flat_rows.h) must pass the
//     flat profile's gate (runtime_profile.h), or its getter reads 0 and the row does
//     nothing without a word. That is checked here, and so are the two things that
//     keep the check honest: menu.cpp builds the page from that table and from
//     nothing else, and every row in it has a row in the generated schema.
//
// Every check that guards a failure is run against a broken twin as well, and the rig
// fails if the twin passes: a check that cannot fail proves nothing.
//   flat_sharpen_test --dry-run
//   flat_sharpen_test --self-test <repo root> <generated folder>
#include <cstdlib>
#include <cstring>
#include <string>

#include "flat_sharpen_rig.h"
#include "../../src/common/runtime_profile.h"
#include "../../src/d3d11/flat_sharpen.h"
#include "../../src/d3d11/menu_flat_rows.h"

using namespace rig;

// ---------------------------------------------------------------------------
// The pass, stubbed: records the call, answers one of two textures the rig owns
// (the pass's per-slot output) or null (a refusal).
namespace {
struct Stub {
    int calls = 0;
    void* src = nullptr;
    int slot = -1;
    float strength = -1.0f;
    bool boundsNull = false;
    bool refuse = false;
    ID3D11Texture2D* result[2] = {nullptr, nullptr};
} stub;
}  // namespace

extern "C" void* edvrSharpen(void* srcTex, int slot, const float* bounds, float strength) {
    ++stub.calls;
    stub.src = srcTex;
    stub.slot = slot;
    stub.strength = strength;
    stub.boundsNull = bounds == nullptr;
    if (stub.refuse || slot < 0 || slot > 1) return nullptr;
    return stub.result[slot];
}

namespace {

struct Frame {
    ComPtr<ID3D11Texture2D> tex;
    ComPtr<ID3D11ShaderResourceView> srgb, plain;
};

Frame makeFrame(ID3D11Device* dev, UINT w, UINT h) {
    Frame f;
    f.tex = makeTexture(dev, w, h, DXGI_FORMAT_R8G8B8A8_TYPELESS, D3D11_BIND_SHADER_RESOURCE);
    if (f.tex) {
        f.srgb = makeSrv(dev, f.tex.Get(), DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
        f.plain = makeSrv(dev, f.tex.Get(), DXGI_FORMAT_R8G8B8A8_UNORM);
    }
    return f;
}

DXGI_FORMAT viewFormat(ID3D11ShaderResourceView* v) {
    D3D11_SHADER_RESOURCE_VIEW_DESC d{};
    v->GetDesc(&d);
    return d.Format;
}

ID3D11Resource* viewResource(ID3D11ShaderResourceView* v, ComPtr<ID3D11Resource>& keep) {
    v->GetResource(&keep);
    return keep.Get();
}

// A view's identity, not its address. The wrapper releases a view before it makes the next,
// so an allocator can hand the new one the old one's address, and "the same pointer came
// back" cannot tell a cached view from one remade every frame (a mutation run found exactly
// that: two survivors). A tag on the object can: a remade view does not carry it.
const GUID kViewTag = {0x6f0c2a17, 0x91d4, 0x4c7e, {0xa3, 0x58, 0x1b, 0xe2, 0x0d, 0x74, 0xc9, 0x3a}};
constexpr UINT32 kViewTagValue = 0xED5F1A7;

void tagView(ID3D11ShaderResourceView* v) {
    if (v) v->SetPrivateData(kViewTag, sizeof(kViewTagValue), &kViewTagValue);
}

bool viewIsTagged(ID3D11ShaderResourceView* v) {
    UINT32 got = 0;
    UINT size = sizeof(got);
    return v && SUCCEEDED(v->GetPrivateData(kViewTag, &size, &got)) && got == kViewTagValue;
}

void printLines(const char* what, const std::string& text, const char* needle) {
    for (const std::string& line : linesWith(text, needle)) {
        std::printf("  log (%s): %s\n", what, line.c_str());
    }
}

// ---------------------------------------------------------------------------
// The panel table.

bool sourceBuildsFlatPageFromTable(const std::string& menuCpp) {
    const size_t at = menuCpp.find("p.name = \"Flat graphics\";");
    if (at == std::string::npos) return false;
    const size_t end = menuCpp.find("firstSelectable(p);", at);
    if (end == std::string::npos) return false;
    const std::string region = menuCpp.substr(at, end - at);
    return region.find("flatPageHasRow(") != std::string::npos &&
           region.find("strcmp(d.key,") == std::string::npos;
}

// The Sharpening row's dimming is decided in one place (flatSharpenRowDim, whose truth table
// is checked below) and reached from three: the row's drawing, a step and a typed edit. menu.cpp
// is 4000 lines of UI the rig cannot draw, so what it can hold is that each site still asks
// that one question, in the profile's own terms.
bool sourceGatesSharpenRow(const std::string& menuCpp) {
    const size_t def = menuCpp.find("bool sharpenRowDisabled() {");
    if (def == std::string::npos) return false;
    const size_t defEnd = menuCpp.find("\n}", def);
    const std::string body = defEnd == std::string::npos ? std::string() : menuCpp.substr(def, defEnd - def);
    return body.find("flatSharpenRowDim(runtimeFlatProfile(),") != std::string::npos &&
           body.find("temporalModeEnabled(Config::get().requestedTemporalMode())") != std::string::npos &&
           menuCpp.find("(sharpenRow && sharpenRowDisabled())") != std::string::npos &&
           menuCpp.find("if (isSharpenRow(d) && sharpenRowDisabled()) {") != std::string::npos &&
           menuCpp.find("if (isSharpenRow(kMenuRows[defIndex]) && sharpenRowDisabled()) {") != std::string::npos;
}

// The one line that hands the resolve's output to the sharpening: flat_runtime.cpp, where the
// game's output copy's first input is swapped for it. The rig cannot run the flat runtime, and
// without this line the wrapper is never reached (a flat log would say so after 30 s, in the
// never-ran note, but a merge that drops the line is cheaper to catch here). What the call
// returns must be what the copy is bound to.
bool sourceHandsResolveToSharpen(const std::string& runtimeCpp) {
    const size_t inc = runtimeCpp.find("#include \"flat_sharpen.h\"");
    const size_t call = runtimeCpp.find("replacement = flatSharpenView(ctx, outputView.Get());");
    if (inc == std::string::npos || call == std::string::npos || inc > call) return false;
    const size_t bind = runtimeCpp.find("PSSetShaderResources(0, 1, &replacement)", call);
    return bind != std::string::npos && bind - call < 200;
}

bool schemaHasRow(const std::string& schema, const char* section, const char* key) {
    const std::string needle = std::string("{\"") + section + "\", \"" + key + "\",";
    return schema.find(needle) != std::string::npos;
}

void panelTable(const std::string& root, const std::string& gen) {
    using namespace edvr;
    check(kFlatPageRowCount >= 3 && flatPageHasRow("fix", "temporal_aa") &&
              flatPageHasRow("fix", "temporal_aa_model") && flatPageHasRow("fix", "render_sharpness"),
          "the flat page has the mode, the DLSS preset and the sharpening");
    check(!flatPageHasRow("fix", "black_void") && !flatPageHasRow("fix", "panel_distance"),
          "no unrelated fix is a row of the flat page");

    const RuntimeProfile saved = g_runtimeProfile;
    g_runtimeProfile = RuntimeProfile::Flat;
    const auto real = [](const char* dotted) { return runtimeProfileAllowsKey(dotted); };
    const FlatPageRow* refused = flatPageFirstRefused(real);
    if (refused) std::printf("  the flat gate refuses the panel's %s.%s\n", refused->section, refused->key);
    check(refused == nullptr, "every key on the flat panel page passes the flat profile's gate");

    // The one row that bypasses the gate is the mode itself, read through
    // requestedTemporalMode(); nothing else may.
    int bypass = 0;
    bool modeIsIt = false;
    for (const FlatPageRow& r : kFlatPageRows) {
        if (r.read != FlatRowRead::RequestedMode) continue;
        ++bypass;
        modeIsIt = std::strcmp(r.section, "fix") == 0 && std::strcmp(r.key, "temporal_aa") == 0;
    }
    check(bypass == 1 && modeIsIt, "fix.temporal_aa is the one row read around the gate");

    // NEGATIVE CONTROLS. The same check, run against a gate that lacks a key the
    // panel shows, must name it; against one that refuses everything, the first row.
    const auto lacksSharpness = [](const char* dotted) {
        return runtimeProfileAllowsKey(dotted) && std::strcmp(dotted, "fix.render_sharpness") != 0;
    };
    const FlatPageRow* m1 = flatPageFirstRefused(lacksSharpness);
    check(m1 && std::strcmp(m1->key, "render_sharpness") == 0,
          "control: a gate without fix.render_sharpness is caught, by name");
    const auto lacksModel = [](const char* dotted) {
        return runtimeProfileAllowsKey(dotted) && std::strcmp(dotted, "fix.temporal_aa_model") != 0;
    };
    const FlatPageRow* m2 = flatPageFirstRefused(lacksModel);
    check(m2 && std::strcmp(m2->key, "temporal_aa_model") == 0,
          "control: a gate without fix.temporal_aa_model is caught, by name");
    check(flatPageFirstRefused([](const char*) { return false; }) != nullptr,
          "control: a gate that refuses everything is caught");
    // And the gate as shipped really does refuse what it should, so the check above
    // is not passing on a gate that admits everything.
    check(!runtimeProfileAllowsKey("fix.black_void") && !runtimeProfileAllowsKey("fix.panel_distance"),
          "the flat gate refuses unrelated fixes");
    g_runtimeProfile = saved;

    // menu.cpp builds the page from the table and from nothing else.
    const std::string menuCpp = readWholeFile(std::wstring(root.begin(), root.end()) +
                                              L"\\src\\d3d11\\menu.cpp");
    check(!menuCpp.empty(), "menu.cpp is readable from the repo root");
    check(sourceBuildsFlatPageFromTable(menuCpp), "menu.cpp builds the flat page from flatPageHasRow");
    std::string hardCoded = menuCpp;
    const size_t at = hardCoded.find("if (!flatPageHasRow(d.section, d.key)) continue;");
    if (at != std::string::npos) {
        hardCoded.replace(at, std::strlen("if (!flatPageHasRow(d.section, d.key)) continue;"),
                          "if (strcmp(d.key, \"temporal_aa\") != 0) continue;");
    }
    check(at != std::string::npos && !sourceBuildsFlatPageFromTable(hardCoded),
          "control: a page built from a key list in menu.cpp is caught");

    // The flat runtime hands its resolve to the sharpening, and binds what comes back.
    const std::string runtimeCpp = readWholeFile(std::wstring(root.begin(), root.end()) +
                                                 L"\\src\\d3d11\\flat_runtime.cpp");
    check(!runtimeCpp.empty(), "flat_runtime.cpp is readable from the repo root");
    check(sourceHandsResolveToSharpen(runtimeCpp),
          "flat_runtime.cpp binds the game's output copy to what flatSharpenView returns for the resolve");
    {
        std::string dropped = runtimeCpp;
        const char* from = "replacement = flatSharpenView(ctx, outputView.Get());";
        const size_t i = dropped.find(from);
        if (i != std::string::npos) dropped.replace(i, std::strlen(from), "replacement = outputView.Get();");
        check(i != std::string::npos && !sourceHandsResolveToSharpen(dropped),
              "control: a runtime that binds the resolve's own view again is caught");
        std::string noInclude = runtimeCpp;
        const char* inc = "#include \"flat_sharpen.h\"";
        const size_t j = noInclude.find(inc);
        if (j != std::string::npos) noInclude.replace(j, std::strlen(inc), "");
        check(j != std::string::npos && !sourceHandsResolveToSharpen(noInclude),
              "control: a runtime without the sharpening's header is caught");
    }

    // The Sharpening row's dimming is asked in all three places, in the profile's terms.
    check(sourceGatesSharpenRow(menuCpp), "menu.cpp dims, refuses steps and refuses typing on the Sharpening row through flatSharpenRowDim");
    auto mutated = [&](const char* from, const char* to) {
        std::string m = menuCpp;
        const size_t i = m.find(from);
        if (i == std::string::npos) return std::string();   // an anchor that moved fails the control below
        m.replace(i, std::strlen(from), to);
        return m;
    };
    const std::string noDraw = mutated("(sharpenRow && sharpenRowDisabled())", "false");
    const std::string noStep = mutated("if (isSharpenRow(d) && sharpenRowDisabled()) {", "if (false) {");
    const std::string noEdit = mutated("if (isSharpenRow(kMenuRows[defIndex]) && sharpenRowDisabled()) {", "if (false) {");
    const std::string vrDims = mutated("flatSharpenRowDim(runtimeFlatProfile(),", "flatSharpenRowDim(true,");
    check(!noDraw.empty() && !sourceGatesSharpenRow(noDraw), "control: a row that never dims when drawn is caught");
    check(!noStep.empty() && !sourceGatesSharpenRow(noStep), "control: a dimmed row that still takes a step is caught");
    check(!noEdit.empty() && !sourceGatesSharpenRow(noEdit), "control: a dimmed row that still takes typing is caught");
    check(!vrDims.empty() && !sourceGatesSharpenRow(vrDims), "control: a row asking about a profile other than the running one is caught");

    // Every row of the table has a row in the schema the page is built from.
    const std::string schema = readWholeFile(std::wstring(gen.begin(), gen.end()) + L"\\menu_schema.inc");
    check(!schema.empty(), "the generated menu schema is readable");
    for (const FlatPageRow& r : kFlatPageRows) {
        check(schemaHasRow(schema, r.section, r.key),
              (std::string("the generated schema has the panel's ") + r.section + "." + r.key).c_str());
    }
    check(!schemaHasRow("", "fix", "render_sharpness"),
          "control: a schema without the row is caught");
}

int truthTableMisses(bool (*dim)(bool, bool)) {
    struct Case { bool flat, aaOn, want; };
    const Case cases[] = {{true, false, true}, {true, true, false}, {false, false, false}, {false, true, false}};
    int wrong = 0;
    for (const Case& c : cases) wrong += dim(c.flat, c.aaOn) != c.want;
    return wrong;
}

void sharpenRowDim() {
    check(truthTableMisses(edvr::flatSharpenRowDim) == 0,
          "the Sharpening row dims in the flat profile with anti-aliasing off, and nowhere else");
    check(truthTableMisses([](bool, bool aaOn) { return !aaOn; }) > 0,
          "control: a row that dims in VR too is caught");
    check(truthTableMisses([](bool flat, bool) { return flat; }) > 0,
          "control: a row that dims with anti-aliasing on is caught");
}

// ---------------------------------------------------------------------------
// The wrapper.

void wrapperContract() {
    using namespace edvr;
    Warp w = makeWarp();
    if (!w.ok) return;
    ID3D11Device* dev = w.device.Get();
    ID3D11DeviceContext* ctx = w.context.Get();
    auto& cfg = Config::get();
    const RuntimeProfile saved = g_runtimeProfile;
    g_runtimeProfile = RuntimeProfile::Flat;   // through the gate, as the game reads it
    const UINT W = 64, H = 48;
    const UINT passBind = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    Frame A = makeFrame(dev, W, H), B = makeFrame(dev, W, H);
    ComPtr<ID3D11Texture2D> r0 = makeTexture(dev, W, H, DXGI_FORMAT_R8G8B8A8_TYPELESS, passBind);
    ComPtr<ID3D11Texture2D> r1 = makeTexture(dev, W, H, DXGI_FORMAT_R8G8B8A8_TYPELESS, passBind);
    check(A.srgb && A.plain && B.srgb && r0 && r1, "the rig's textures and views");
    if (!(A.srgb && A.plain && B.srgb && r0 && r1)) { g_runtimeProfile = saved; return; }
    stub = Stub{};
    stub.result[0] = r0.Get();
    stub.result[1] = r1.Get();

    // 1. OFF: the resolve's own view, the pass never called, nothing allocated.
    const std::string offLog = withLog(L"flatsharpenoff", [&] {
        flatSharpenReset();
        int passes = 0;
        for (const char* v : {"0", "0.0", "-1", "nan", "inf", "", "abc"}) {
            cfg.set("fix.render_sharpness", v);
            ID3D11ShaderResourceView* out = flatSharpenView(ctx, A.srgb.Get());
            check(out == A.srgb.Get(), (std::string("off (") + v + "): the resolve's own view").c_str());
            ++passes;
        }
        check(stub.calls == 0, "off: the pass is never called");
        const FlatSharpenCounts c = flatSharpenCounts();
        check(c.passedOff == static_cast<uint64_t>(passes) && c.sharpened == 0 && !c.stoodDown,
              "off: every frame counted as passed through, none sharpened");
        check(flatSharpenView(ctx, nullptr) == nullptr, "no view in, no view out");
    });
    check(countLines(offLog, "flat sharpen: off (fix.render_sharpness is 0)") == 1 &&
              countLines(offLog, "turned off") == 0,
          "off: said once, and only once");
    printLines("off", offLog, "flat sharpen:");

    // The setting the game's copy would read is the gate's to refuse: under a
    // profile that does not list the key it reads 0 whatever the file says. This is
    // the silent zero the panel table exists to keep out of the shipped profile.
    {
        g_runtimeProfile = RuntimeProfile::Invalid;
        cfg.set("fix.render_sharpness", "0.7");
        flatSharpenReset();
        stub.calls = 0;
        ID3D11ShaderResourceView* out = flatSharpenView(ctx, A.srgb.Get());
        check(out == A.srgb.Get() && stub.calls == 0,
              "a profile that does not list the key reads it as 0: no sharpening, no word");
        g_runtimeProfile = RuntimeProfile::Flat;
    }

    // 2. ON, and the format parity, the live setting, the slots.
    const std::string onLog = withLog(L"flatsharpenon", [&] {
        flatSharpenReset();
        stub.calls = 0;
        cfg.set("fix.render_sharpness", "0.7");
        ID3D11ShaderResourceView* v1 = flatSharpenView(ctx, A.srgb.Get());
        check(v1 && v1 != A.srgb.Get(), "on: a different view comes back");
        check(stub.calls == 1 && stub.src == A.tex.Get() && stub.slot == 0 && stub.boundsNull &&
                  stub.strength == 0.7f,
              "on: the pass gets the resolve's texture, slot 0, the whole texture, strength 0.7");
        ComPtr<ID3D11Resource> keep;
        check(v1 && viewResource(v1, keep) == r0.Get(), "on: the view is over the pass's texture");
        check(v1 && viewFormat(v1) == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
              "an sRGB resolve view stays sRGB: the game's copy decodes it as it would have");
        ID3D11ShaderResourceView* v2 = flatSharpenView(ctx, A.plain.Get());
        check(v2 && viewFormat(v2) == DXGI_FORMAT_R8G8B8A8_UNORM,
              "a plain resolve view stays plain");
        tagView(v2);
        ID3D11ShaderResourceView* v2again = flatSharpenView(ctx, A.plain.Get());
        check(v2again == v2 && viewIsTagged(v2again),
              "the same texture and format reuse the cached view (the same object, not one remade)");

        // Live: read every frame, clamped, anything unusable is nothing to do.
        cfg.set("fix.render_sharpness", "0.1");
        flatSharpenView(ctx, A.plain.Get());
        check(stub.strength == 0.1f, "live: 0.1 on the next frame");
        cfg.set("fix.render_sharpness", "2");
        flatSharpenView(ctx, A.plain.Get());
        check(stub.strength == 1.0f, "live: 2 clamps to 1");
        const int before = stub.calls;
        cfg.set("fix.render_sharpness", "-3");
        check(flatSharpenView(ctx, A.plain.Get()) == A.plain.Get() && stub.calls == before,
              "live: -3 is nothing to do");
        cfg.set("fix.render_sharpness", "0.5");
        flatSharpenView(ctx, A.plain.Get());
        check(stub.strength == 0.5f && stub.calls == before + 1, "live: on again at 0.5");

        // The two resolve textures alternate; each keeps its slot and its view.
        ID3D11ShaderResourceView* seenA = nullptr;
        ID3D11ShaderResourceView* seenB = nullptr;
        bool slotsRight = true, stable = true, resultsRight = true;
        for (int i = 0; i < 8; ++i) {
            const bool isB = (i & 1) != 0;
            ID3D11ShaderResourceView* out =
                flatSharpenView(ctx, isB ? B.srgb.Get() : A.srgb.Get());
            slotsRight = slotsRight && stub.slot == (isB ? 1 : 0);
            ComPtr<ID3D11Resource> res;
            resultsRight = resultsRight && out && viewResource(out, res) == (isB ? r1.Get() : r0.Get());
            ID3D11ShaderResourceView*& seen = isB ? seenB : seenA;
            if (i >= 2) stable = stable && out == seen && viewIsTagged(out);
            else tagView(out);
            seen = out;
        }
        check(slotsRight, "alternating textures: A is always slot 0, B always slot 1");
        check(resultsRight, "alternating textures: each view is over its own slot's result");
        check(stable, "alternating textures: each slot's view is reused, not remade");
    });
    // The one off-and-on in that sequence (-3, then 0.5) is said once each way; the
    // first frame's engaging is the pass's own line, not this one's.
    check(countLines(onLog, "flat sharpen: turned off") == 1 &&
              countLines(onLog, "flat sharpen: on again at strength 0.50") == 1,
          "on: turning it off and on again is said once each way");
    printLines("on", onLog, "flat sharpen:");

    // 3. Toggling live says so (and does not repeat itself for ever).
    const std::string toggleLog = withLog(L"flatsharpentoggle", [&] {
        flatSharpenReset();
        cfg.set("fix.render_sharpness", "0.4");
        flatSharpenView(ctx, A.srgb.Get());
        cfg.set("fix.render_sharpness", "0");
        flatSharpenView(ctx, A.srgb.Get());
        cfg.set("fix.render_sharpness", "0.4");
        flatSharpenView(ctx, A.srgb.Get());
        for (int i = 0; i < 30; ++i) {
            cfg.set("fix.render_sharpness", (i & 1) ? "0.4" : "0");
            flatSharpenView(ctx, A.srgb.Get());
        }
    });
    check(countLines(toggleLog, "flat sharpen: turned off") >= 1 &&
              countLines(toggleLog, "flat sharpen: on again") >= 1,
          "toggling: turning off and on again are said");
    check(countLines(toggleLog, "flat sharpen: turned off") + countLines(toggleLog, "flat sharpen: on again") <= 6,
          "toggling: a slider walking the setting does not fill the log");
    printLines("toggle", toggleLog, "flat sharpen:");

    // 4. A refusal stands the sharpening down for the session, once, and a new
    //    device starts it afresh.
    const std::string refuseLog = withLog(L"flatsharpenrefuse", [&] {
        flatSharpenReset();
        cfg.set("fix.render_sharpness", "0.6");
        stub.calls = 0;
        stub.refuse = true;
        ID3D11ShaderResourceView* out = flatSharpenView(ctx, A.srgb.Get());
        check(out == A.srgb.Get() && stub.calls == 1, "refusal: the frame goes through, the pass was asked once");
        stub.refuse = false;
        for (int i = 0; i < 5; ++i) check(flatSharpenView(ctx, A.srgb.Get()) == A.srgb.Get(), "stood down: frames pass through");
        check(stub.calls == 1, "stood down: the pass is not asked again, even once it would answer");
        const FlatSharpenCounts c = flatSharpenCounts();
        check(c.stoodDown && c.refusals == 1 && c.passedStoodDown == 5 && c.sharpened == 0,
              "stood down: counted");
        // A new device is a new session. This is the wrapper's half only, against a stubbed
        // pass that hands back whatever texture the rig gives it: what the REAL pass does
        // with a frame from another device (it used to hand back the old device's result
        // texture, and this wrapper stood down) is flat_sharpen_pass_test's two-device cases.
        Warp w2 = makeWarp();
        if (w2.ok) {
            Frame F2 = makeFrame(w2.device.Get(), W, H);
            ComPtr<ID3D11Texture2D> q0 = makeTexture(w2.device.Get(), W, H, DXGI_FORMAT_R8G8B8A8_TYPELESS, passBind);
            ID3D11Texture2D* keepR0 = stub.result[0];
            stub.result[0] = q0.Get();
            ID3D11ShaderResourceView* second = flatSharpenView(w2.context.Get(), F2.srgb.Get());
            check(second && second != F2.srgb.Get() && !flatSharpenCounts().stoodDown,
                  "a new device starts the sharpening afresh");
            stub.result[0] = keepR0;
            flatSharpenReset();   // release the views that belong to the second device
        }
    });
    check(countLines(refuseLog, "the sharpening pass refused a 64x48 frame") == 1,
          "refusal: said once");
    printLines("refusal", refuseLog, "flat sharpen:");

    // 5. Things the pass cannot use pass the frame through, each said once.
    const std::string oddLog = withLog(L"flatsharpenodd", [&] {
        flatSharpenReset();
        cfg.set("fix.render_sharpness", "0.6");
        stub.calls = 0;
        // A result the game's copy cannot sample (no shader-resource bind): no view
        // can be made over it.
        ComPtr<ID3D11Texture2D> noSrv = makeTexture(dev, W, H, DXGI_FORMAT_R8G8B8A8_TYPELESS,
                                                    D3D11_BIND_UNORDERED_ACCESS);
        ID3D11Texture2D* keep0 = stub.result[0];
        stub.result[0] = noSrv.Get();
        check(flatSharpenView(ctx, A.srgb.Get()) == A.srgb.Get() && flatSharpenCounts().stoodDown,
              "no view over the result: the frame goes through and the sharpening stands down");
        stub.result[0] = keep0;
        flatSharpenReset();
        // A deferred context.
        ComPtr<ID3D11DeviceContext> deferred;
        if (SUCCEEDED(dev->CreateDeferredContext(0, &deferred))) {
            stub.calls = 0;
            check(flatSharpenView(deferred.Get(), A.srgb.Get()) == A.srgb.Get() && stub.calls == 0,
                  "a deferred context: the frame goes through, the pass is not asked");
            flatSharpenView(deferred.Get(), A.srgb.Get());
        }
        // A view that is not a plain 2D view.
        ComPtr<ID3D11Texture2D> array = makeTexture(dev, W, H, DXGI_FORMAT_R8G8B8A8_TYPELESS,
                                                    D3D11_BIND_SHADER_RESOURCE, nullptr, 2);
        D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
        vd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
        vd.Texture2DArray.MipLevels = 1;
        vd.Texture2DArray.ArraySize = 2;
        ComPtr<ID3D11ShaderResourceView> arrayView;
        if (array && SUCCEEDED(dev->CreateShaderResourceView(array.Get(), &vd, &arrayView))) {
            stub.calls = 0;
            check(flatSharpenView(ctx, arrayView.Get()) == arrayView.Get() && stub.calls == 0,
                  "an array view: the frame goes through, the pass is not asked");
            flatSharpenView(ctx, arrayView.Get());
        }
    });
    check(countLines(oddLog, "a view over the sharpened 64x48 frame could not be made") == 1,
          "no view over the result: said once");
    check(countLines(oddLog, "not the immediate one") == 1, "a deferred context: said once");
    check(countLines(oddLog, "not a plain 2D view") == 1, "an array view: said once");
    printLines("odd", oddLog, "flat sharpen:");

    flatSharpenReset();
    cfg.set("fix.render_sharpness", "");
    g_runtimeProfile = saved;
}

}  // namespace

int main(int argc, char** argv) {
    SetErrorMode(3);
    if (argc >= 2 && !std::strcmp(argv[1], "--dry-run")) {
        std::puts("flat_sharpen_test: dry-run (no device, no files)");
        return 0;
    }
    if (argc != 4 || std::strcmp(argv[1], "--self-test")) {
        std::puts("usage: flat_sharpen_test --dry-run | --self-test <repo root> <generated folder>");
        return 2;
    }
    panelTable(argv[2], argv[3]);
    sharpenRowDim();
    wrapperContract();
    std::printf("flat_sharpen_test: %u checks, %u failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
