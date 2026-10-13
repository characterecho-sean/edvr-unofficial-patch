// runtime_profile_test -- the launch decision's pure rules (src\common\install_scope.h): the descriptor parse (schema 1 and 2 and their
// strictness), the frozen v1 parser an older DLL runs, Settings.xml's 3D mode, the command line's switch, the scope-by-mode profile
// table, the decision with and without each switch, the launch's log line, and the redaction of the logged command line. Built from
// the repo root by build.bat's :rig_runtime_profile_test. Windows-free: it includes no Windows header. --dry-run writes nothing;
// --self-test runs the checks.
#include "../../src/common/install_scope.h"
#include "../../src/common/stereo_mode.h"
#include "../../src/d3d11/vr_window_gate.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace {

using namespace edvr;

int g_checks = 0;
int g_fails = 0;

void check(bool ok, const char* name) {
    ++g_checks;
    if (!ok) ++g_fails;
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", name);
}

void checkText(const char* name, const std::string& got, const std::string& want) {
    if (got == want) {
        check(true, name);
        return;
    }
    check(false, name);
    std::printf("  got:  %s\n  want: %s\n", got.c_str(), want.c_str());
}

// The frozen copy of the v1 parser shipped up to this branch (runtime_profile.h's parseRuntimeProfile, before schema 2). An older DLL
// runs this body, so the rig reads schema 2 through it to show that an older build refuses the new descriptor. Do not change it.
RuntimeProfile legacyParseV1(std::string text) {
    if (text.size() > 4096 || text.find('\0') != std::string::npos)
        return RuntimeProfile::Invalid;
    if (text.compare(0, 3, "\xef\xbb\xbf") == 0) text.erase(0, 3);
    bool section = false, schema = false, profile = false;
    RuntimeProfile result = RuntimeProfile::Invalid;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        const std::string line = profileTrim(text.substr(pos, end - pos));
        pos = end + 1;
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        if (line == "[install]") {
            if (section) return RuntimeProfile::Invalid;
            section = true;
            continue;
        }
        if (!section) return RuntimeProfile::Invalid;
        const size_t eq = line.find('=');
        if (eq == std::string::npos) return RuntimeProfile::Invalid;
        const auto key = profileTrim(line.substr(0, eq));
        const auto value = profileTrim(line.substr(eq + 1));
        if (key == "schema" && !schema && value == "1") schema = true;
        else if (key == "profile" && !profile) {
            profile = true;
            if (value == "flat") result = RuntimeProfile::Flat;
            else if (value == "vr") result = RuntimeProfile::Vr;
            else return RuntimeProfile::Invalid;
        } else return RuntimeProfile::Invalid;
    }
    return section && schema && profile ? result : RuntimeProfile::Invalid;
}

void expectScope(const char* name, const std::string& text, InstallScope scope, int schema) {
    const ScopeRead r = parseInstallDescriptor(text);
    check(r.scope == scope && r.schema == schema && r.present, name);
}

void descriptorCases() {
    const ScopeRead absent = absentDescriptor();
    check(absent.scope == InstallScope::Both && absent.schema == 0 && !absent.present, "descriptor: absent is both, no schema");
    expectScope("descriptor: schema 1 vr is both", "[install]\r\nschema = 1\r\nprofile = vr\r\n", InstallScope::Both, 1);
    expectScope("descriptor: schema 1 flat is flat", "[install]\r\nschema = 1\r\nprofile = flat\r\n", InstallScope::Flat, 1);
    expectScope("descriptor: schema 2 both", "[install]\r\nschema = 2\r\nscope = both\r\n", InstallScope::Both, 2);
    expectScope("descriptor: schema 2 vr", "[install]\r\nschema = 2\r\nscope = vr\r\n", InstallScope::Vr, 2);
    expectScope("descriptor: schema 2 flat", "[install]\r\nschema = 2\r\nscope = flat\r\n", InstallScope::Flat, 2);
    expectScope("descriptor: schema 2 with a BOM and CRLF", "\xef\xbb\xbf[install]\r\nschema = 2\r\nscope = vr\r\n",
        InstallScope::Vr, 2);
    expectScope("descriptor: duplicate scope is invalid", "[install]\nschema=2\nscope=vr\nscope=both\n", InstallScope::Invalid, 0);
    expectScope("descriptor: duplicate schema is invalid", "[install]\nschema=2\nschema=2\nscope=both\n", InstallScope::Invalid, 0);
    expectScope("descriptor: duplicate profile under schema 1 is invalid", "[install]\nschema=1\nprofile=flat\nprofile=vr\n",
        InstallScope::Invalid, 0);
    expectScope("descriptor: unknown key is invalid", "[install]\nschema=2\nscope=both\nfoo=1\n", InstallScope::Invalid, 0);
    expectScope("descriptor: scope under schema 1 is invalid", "[install]\nschema=1\nprofile=vr\nscope=both\n", InstallScope::Invalid, 0);
    expectScope("descriptor: profile under schema 2 is invalid", "[install]\nschema=2\nscope=both\nprofile=vr\n", InstallScope::Invalid, 0);
    expectScope("descriptor: schema 3 is invalid", "[install]\nschema=3\nscope=both\n", InstallScope::Invalid, 0);
    expectScope("descriptor: missing section is invalid", "schema=2\nscope=both\n", InstallScope::Invalid, 0);
    expectScope("descriptor: empty text is invalid", "", InstallScope::Invalid, 0);

    std::string nul = "[install]\nschema=2\nscope=both\n";
    nul.push_back('\0');
    expectScope("descriptor: a NUL byte is invalid", nul, InstallScope::Invalid, 0);

    const std::string header = "[install]\nschema=2\nscope=both\n";
    expectScope("descriptor: 5000 bytes is invalid", header + std::string(5000 - header.size(), '#'), InstallScope::Invalid, 0);
    expectScope("descriptor: 4096 bytes is read", header + "#" + std::string(4096 - header.size() - 2, 'x') + "\n",
        InstallScope::Both, 2);
}

void legacyCases() {
    check(legacyParseV1("[install]\nschema=1\nprofile=vr\n") == RuntimeProfile::Vr, "legacy v1: schema 1 vr still reads Vr");
    check(legacyParseV1("[install]\r\nschema = 1\r\nprofile = flat\r\n") == RuntimeProfile::Flat,
        "legacy v1: schema 1 flat still reads Flat");
    check(legacyParseV1("[install]\nschema = 2\nscope = both\n") == RuntimeProfile::Invalid, "legacy v1: schema 2 both reads Invalid");
    check(legacyParseV1("[install]\nschema = 2\nscope = vr\n") == RuntimeProfile::Invalid, "legacy v1: schema 2 vr reads Invalid");
    check(legacyParseV1("[install]\nschema = 2\nscope = flat\n") == RuntimeProfile::Invalid, "legacy v1: schema 2 flat reads Invalid");
}

void settingsCases() {
    const ModeRead none = readGameMode(nullptr);
    check(!none.known && none.value == -1 && none.mode == GameMode::Vr && !none.fileRead,
        "settings: null text is unknown, Vr, file absent");
    for (int m = 0; m <= 6; ++m) {
        const std::string xml = "<Settings>\r\n<StereoscopicMode>" + std::to_string(m) + "</StereoscopicMode>\r\n</Settings>\r\n";
        const ModeRead r = readGameMode(xml.c_str());
        const GameMode want = m <= 1 ? GameMode::Flat : GameMode::Vr;
        char name[80];
        std::snprintf(name, sizeof name, "settings: mode %d reads %s", m, gameModeName(want));
        check(r.known && r.value == m && r.mode == want && r.fileRead, name);
    }
    const ModeRead bom = readGameMode("\xef\xbb\xbf<?xml version=\"1.0\"?>\r\n<Settings>\r\n<StereoscopicMode>3</StereoscopicMode>\r\n</Settings>");
    check(bom.known && bom.value == 3 && bom.mode == GameMode::Vr, "settings: a BOM before <?xml> reads mode 3");
    const ModeRead spaced = readGameMode("<Settings>\r\n<StereoscopicMode>\r\n  1 \t\r\n</StereoscopicMode>\r\n</Settings>");
    check(spaced.known && spaced.value == 1 && spaced.mode == GameMode::Flat, "settings: CRLF and spaces inside the tag read mode 1");
    const ModeRead absentTag = readGameMode("<Settings><PresetName>Custom</PresetName></Settings>");
    check(!absentTag.known && absentTag.mode == GameMode::Vr && absentTag.fileRead, "settings: tag absent is unknown, Vr, file read");
    const ModeRead seven = readGameMode("<StereoscopicMode>7</StereoscopicMode>");
    check(!seven.known && seven.mode == GameMode::Vr && seven.fileRead, "settings: mode 7 is unknown");
    const ModeRead minus = readGameMode("<StereoscopicMode>-1</StereoscopicMode>");
    check(!minus.known && minus.mode == GameMode::Vr && minus.fileRead, "settings: mode -1 is unknown");
    const ModeRead empty = readGameMode("");
    check(!empty.known && empty.mode == GameMode::Vr && empty.fileRead, "settings: empty text is unknown, file read");
}

// The closing tag must follow the number in full (stereo_mode.h). A cut-off or mismatched tag reads unknown, and an unknown mode runs Vr
// through the launch decision (Q2), so a half-written Settings.xml cannot pick the Flat profile.
void closingTagCases() {
    const ScopeRead both = parseInstallDescriptor("[install]\r\nschema = 2\r\nscope = both\r\n");
    for (int m = 0; m <= 6; ++m) {
        const std::string xml = "<Settings>\r\n  <StereoscopicMode>\r\n\t" + std::to_string(m) + " \r\n</StereoscopicMode>\r\n</Settings>";
        const ModeRead r = readGameMode(xml.c_str());
        const GameMode want = m <= 1 ? GameMode::Flat : GameMode::Vr;
        const RuntimeProfile wantActive = m <= 1 ? RuntimeProfile::Flat : RuntimeProfile::Vr;
        const LaunchDecision d = decideLaunch(both, r, VrSwitch::None, false, false);
        char name[120];
        std::snprintf(name, sizeof name, "closing tag: complete mode %d with CRLF and whitespace reads %s", m, gameModeName(want));
        check(r.known && r.value == m && r.mode == want && d.active == wantActive, name);
    }
    // The bare values, with no whitespace or wrapper: the closing tag directly after the digit.
    for (int m = 0; m <= 6; ++m) {
        const std::string xml = "<StereoscopicMode>" + std::to_string(m) + "</StereoscopicMode>";
        const ModeRead r = readGameMode(xml.c_str());
        check(r.known && r.value == m, ("closing tag: bare mode " + std::to_string(m) + " reads known").c_str());
    }
    const char* truncated[] = {
        "<StereoscopicMode>0<",
        "<StereoscopicMode>0</Stereo",
        "<StereoscopicMode>0</StereoscopicModeX>",
        "<StereoscopicMode>0</Other>",
        "<StereoscopicMode>0",
        "<StereoscopicMode>0\r\n</Settings>",
    };
    int index = 0;
    for (const char* xml : truncated) {
        ++index;
        const ModeRead r = readGameMode(xml);
        const LaunchDecision d = decideLaunch(both, r, VrSwitch::None, false, false);
        char name[120];
        std::snprintf(name, sizeof name, "closing tag: incomplete or mismatched case %d is unknown, Vr at launch", index);
        check(!r.known && r.mode == GameMode::Vr && r.fileRead && d.effective == GameMode::Vr && d.active == RuntimeProfile::Vr, name);
    }
}

void profileCases() {
    struct Row {
        InstallScope scope;
        GameMode mode;
        RuntimeProfile want;
        const char* name;
    };
    const Row rows[] = {
        {InstallScope::Both, GameMode::Flat, RuntimeProfile::Flat, "profile: both with flat mode is Flat"},
        {InstallScope::Both, GameMode::Vr, RuntimeProfile::Vr, "profile: both with vr mode is Vr"},
        {InstallScope::Vr, GameMode::Flat, RuntimeProfile::Passthrough, "profile: vr scope with flat mode is Passthrough"},
        {InstallScope::Vr, GameMode::Vr, RuntimeProfile::Vr, "profile: vr scope with vr mode is Vr"},
        {InstallScope::Flat, GameMode::Flat, RuntimeProfile::Flat, "profile: flat scope with flat mode is Flat"},
        {InstallScope::Flat, GameMode::Vr, RuntimeProfile::Passthrough, "profile: flat scope with vr mode is Passthrough"},
        {InstallScope::Invalid, GameMode::Flat, RuntimeProfile::Invalid, "profile: invalid scope with flat mode is Invalid"},
        {InstallScope::Invalid, GameMode::Vr, RuntimeProfile::Invalid, "profile: invalid scope with vr mode is Invalid"},
    };
    for (const Row& r : rows) check(activeProfileFor(r.scope, r.mode) == r.want, r.name);
}

void switchCases() {
    check(parseVrSwitch("") == VrSwitch::None, "switch: an empty line is none");
    check(parseVrSwitch(R"("C:\game\EliteDangerous64.exe")") == VrSwitch::None, "switch: the program path alone is none");
    check(parseVrSwitch(R"(C:\game\EliteDangerous64.exe /novr)") == VrSwitch::NoVr, "switch: a bare /novr");
    check(parseVrSwitch("x.exe /VR") == VrSwitch::Vr, "switch: /VR is case-insensitive");
    check(parseVrSwitch("x.exe\t/vr") == VrSwitch::Vr, "switch: a tab separates tokens");
    check(parseVrSwitch(R"("C:\a b\x.exe" /novr)") == VrSwitch::NoVr, "switch: a quoted path with a space, then /novr");
    check(parseVrSwitch(R"("C:\a b\x.exe" /vr /novr)") == VrSwitch::NoVr, "switch: /vr then /novr: the last wins, NoVr");
    check(parseVrSwitch(R"(x.exe /novr /vr)") == VrSwitch::Vr, "switch: /novr then /vr: the last wins, Vr");
    check(parseVrSwitch("x.exe /novrx") == VrSwitch::None, "switch: /novrx is not /novr");
    check(parseVrSwitch(R"(x.exe "/novr")") == VrSwitch::None, "switch: a quoted /novr is none");
    check(parseVrSwitch(R"("x.exe /novr")") == VrSwitch::None, "switch: /novr inside a quoted span is none");
}

void decisionCases() {
    const ScopeRead scope = parseInstallDescriptor("[install]\r\nschema = 2\r\nscope = both\r\n");
    const ModeRead mode4 = readGameMode("<StereoscopicMode>4</StereoscopicMode>");
    const ModeRead mode0 = readGameMode("<StereoscopicMode>0</StereoscopicMode>");
    const LaunchDecision vrOver0 = decideLaunch(scope, mode0, VrSwitch::Vr, true, false);
    check(vrOver0.effective == GameMode::Vr && vrOver0.cmdApplied && vrOver0.active == RuntimeProfile::Vr,
        "decision: /vr over mode 0 is Vr when applyVr is true");
    const LaunchDecision vrOff = decideLaunch(scope, mode0, VrSwitch::Vr, false, true);
    check(vrOff.effective == GameMode::Flat && !vrOff.cmdApplied && vrOff.active == RuntimeProfile::Flat,
        "decision: /vr is not applied by the applyNoVr flag alone");
    const LaunchDecision novrOff = decideLaunch(scope, mode4, VrSwitch::NoVr, true, false);
    check(novrOff.effective == GameMode::Vr && !novrOff.cmdApplied && novrOff.active == RuntimeProfile::Vr,
        "decision: /novr over mode 4 is not applied when applyNoVr is false");
    const LaunchDecision novrOn = decideLaunch(scope, mode4, VrSwitch::NoVr, true, true);
    check(novrOn.effective == GameMode::Flat && novrOn.cmdApplied && novrOn.active == RuntimeProfile::Flat,
        "decision: /novr over mode 4 is Flat when applyNoVr is true");
    // The production call passes both flags as true (runtime_profile.h): each switch overrides the file.
    const LaunchDecision production = decideLaunch(scope, mode4, VrSwitch::NoVr, true, true);
    check(production.effective == GameMode::Flat && production.cmdApplied && production.active == RuntimeProfile::Flat,
        "production shape (applyVr and applyNoVr both true): /novr over mode 4 is Flat, applied");
    const LaunchDecision productionVr = decideLaunch(scope, mode0, VrSwitch::Vr, true, true);
    check(productionVr.effective == GameMode::Vr && productionVr.cmdApplied && productionVr.active == RuntimeProfile::Vr,
        "production shape: /vr over mode 0 is Vr, applied");
    const LaunchDecision none = decideLaunch(scope, mode4, VrSwitch::None, true, true);
    check(none.effective == GameMode::Vr && !none.cmdApplied && none.active == RuntimeProfile::Vr,
        "decision: no switch leaves mode 4 as Vr");
    const LaunchDecision invalid = decideLaunch(parseInstallDescriptor("[install]\nschema=3\n"), mode4, VrSwitch::None, false, false);
    check(invalid.active == RuntimeProfile::Invalid, "decision: an invalid descriptor is Invalid whatever the mode");
}

void lineCases() {
    const char* path = R"(C:\Users\x\AppData\Local\Frontier Developments\Elite Dangerous\Options\Graphics\Settings.xml)";
    const std::string p = path;
    const ScopeRead schema2Flat = parseInstallDescriptor("[install]\nschema = 2\nscope = flat\n");
    const ScopeRead schema2Both = parseInstallDescriptor("[install]\nschema = 2\nscope = both\n");
    const ModeRead mode0 = readGameMode("<StereoscopicMode>0</StereoscopicMode>");
    const ModeRead mode4 = readGameMode("<StereoscopicMode>4</StereoscopicMode>");
    checkText("line: schema 2 flat with mode 0",
        formatProfileLine(decideLaunch(schema2Flat, mode0, VrSwitch::None, false, false), "flat", p),
        "profile: flat (scope flat; 3D mode 0 from " + p + "; command line none)");
    const ScopeRead schema1Vr = parseInstallDescriptor("[install]\nschema = 1\nprofile = vr\n");
    checkText("line: schema 1 vr with mode 4",
        formatProfileLine(decideLaunch(schema1Vr, mode4, VrSwitch::None, false, false), "vr", p),
        "profile: vr (scope both from schema-1 descriptor; 3D mode 4 from " + p + "; command line none)");
    checkText("line: no descriptor and the file absent",
        formatProfileLine(decideLaunch(absentDescriptor(), readGameMode(nullptr), VrSwitch::None, false, false), "vr", p),
        "profile: vr (scope both with no descriptor; 3D mode unknown from " + p + " (file absent); command line none)");
    checkText("line: the tag missing from a read file",
        formatProfileLine(decideLaunch(schema2Both, readGameMode("<Settings/>"), VrSwitch::None, false, false), "vr", p),
        "profile: vr (scope both; 3D mode unknown from " + p + "; command line none)");
    checkText("line: /vr applied over mode 0",
        formatProfileLine(decideLaunch(schema2Both, mode0, VrSwitch::Vr, true, false), "vr", p),
        "profile: vr (scope both; 3D mode 0 from " + p + "; command line /vr applied)");
    checkText("line: /vr not applied over mode 0",
        formatProfileLine(decideLaunch(schema2Both, mode0, VrSwitch::Vr, false, false), "flat", p),
        "profile: flat (scope both; 3D mode 0 from " + p + "; command line /vr not applied)");
    checkText("line: /novr not applied when its flag is off",
        formatProfileLine(decideLaunch(schema2Both, mode4, VrSwitch::NoVr, true, false), "vr", p),
        "profile: vr (scope both; 3D mode 4 from " + p + "; command line /novr not applied)");
    checkText("line: /novr applied (production: both flags on)",
        formatProfileLine(decideLaunch(schema2Both, mode4, VrSwitch::NoVr, true, true), "flat", p),
        "profile: flat (scope both; 3D mode 4 from " + p + "; command line /novr applied)");
    checkText("line: an invalid descriptor",
        formatProfileLine(decideLaunch(parseInstallDescriptor("[install]\nschema = 3\n"), readGameMode(nullptr), VrSwitch::None, false, false),
            "invalid", p),
        "profile: invalid (edvr_profile.ini malformed; fixes disabled, proxy chaining retained; repair the installation)");
}

bool leaksSecret(const std::string& s) {
    return s.find("ServerToken") != std::string::npos || s.find("AUTH_PASSWORD") != std::string::npos ||
        s.find("epicusername") != std::string::npos || s.find("abc123") != std::string::npos ||
        s.find("4d15") != std::string::npos || s.find("Sean M") != std::string::npos ||
        s.find("22702") != std::string::npos;
}

void redactCases() {
    const std::string steam = R"("C:\Steam\steamapps\common\Elite Dangerous\EliteDangerous64.exe" "wseed 22702"   "ServerToken 4d15 67b6" /vr)";
    checkText("redact: the Steam shape", redactCommandLine(steam), "EliteDangerous64.exe <redacted> <redacted> /vr");
    const std::string epic = R"("D:\Epic\x y\EliteDangerous64.exe" -AUTH_LOGIN=unused -AUTH_PASSWORD=abc123 -AUTH_TYPE=exchangecode -epicapp=Foo -epicenv=Prod -EpicPortal -epicusername="Sean M" -epicuserid=0123)";
    checkText("redact: the Epic shape", redactCommandLine(epic),
        "EliteDangerous64.exe <redacted> <redacted> <redacted> <redacted> <redacted> -EpicPortal <redacted> <redacted>");
    checkText("redact: an empty line", redactCommandLine(""), "");
    checkText("redact: a 40-character switch is redacted", redactCommandLine("x.exe /" + std::string(40, 'a')), "x.exe <redacted>");
    checkText("redact: a 32-character switch is kept", redactCommandLine("x.exe -" + std::string(32, 'a')),
        "x.exe -" + std::string(32, 'a'));
    checkText("redact: a switch with an equals sign is redacted", redactCommandLine("x.exe /a=b"), "x.exe <redacted>");
    check(!leaksSecret(redactCommandLine(steam)) && !leaksSecret(redactCommandLine(epic)),
        "redact: no output carries a server token, a password, an account name or a user id");
}

// The mode request's rewrite gate (vr_window_gate.h) over the four profiles, each with the install scope's VR flag and the runtime's live
// flag (16 rows). A VR profile always rewrites. Flat and Passthrough rewrite only with VR in scope and a live session. Invalid never does.
void windowGateCases() {
    struct Row { RuntimeProfile current; const char* name; bool scopeVr; bool live; bool want; };
    const Row rows[] = {
        {RuntimeProfile::Vr, "vr", true, true, true},
        {RuntimeProfile::Vr, "vr", true, false, true},
        {RuntimeProfile::Vr, "vr", false, true, true},
        {RuntimeProfile::Vr, "vr", false, false, true},
        {RuntimeProfile::Flat, "flat", true, true, true},
        {RuntimeProfile::Flat, "flat", true, false, false},
        {RuntimeProfile::Flat, "flat", false, true, false},
        {RuntimeProfile::Flat, "flat", false, false, false},
        {RuntimeProfile::Passthrough, "passthrough", true, true, true},
        {RuntimeProfile::Passthrough, "passthrough", true, false, false},
        {RuntimeProfile::Passthrough, "passthrough", false, true, false},
        {RuntimeProfile::Passthrough, "passthrough", false, false, false},
        {RuntimeProfile::Invalid, "invalid", true, true, false},
        {RuntimeProfile::Invalid, "invalid", true, false, false},
        {RuntimeProfile::Invalid, "invalid", false, true, false},
        {RuntimeProfile::Invalid, "invalid", false, false, false},
    };
    for (const Row& row : rows) {
        const std::string name = std::string("window gate: current ") + row.name + ", scope vr " + (row.scopeVr ? "yes" : "no") +
            ", vr live " + (row.live ? "yes" : "no");
        check(vrWindowRewriteAllowed(row.current, row.scopeVr, row.live) == row.want, name.c_str());
    }
}

void nameCases() {
    check(std::strcmp(scopeName(InstallScope::Both), "both") == 0 && std::strcmp(scopeName(InstallScope::Vr), "vr") == 0 &&
        std::strcmp(scopeName(InstallScope::Flat), "flat") == 0 && std::strcmp(scopeName(InstallScope::Invalid), "invalid") == 0,
        "names: scope names");
    check(std::strcmp(gameModeName(GameMode::Flat), "flat") == 0 && std::strcmp(gameModeName(GameMode::Vr), "vr") == 0,
        "names: mode names");
}

}  // namespace

int main(int argc, char** argv) {
    bool dryRun = false, selfTest = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--dry-run") == 0) dryRun = true;
        if (std::strcmp(argv[i], "--self-test") == 0) selfTest = true;
    }
    if (dryRun && !selfTest) {
        std::printf("runtime_profile_test: dry run, nothing written\n");
        return 0;
    }
    descriptorCases();
    legacyCases();
    settingsCases();
    closingTagCases();
    profileCases();
    switchCases();
    decisionCases();
    lineCases();
    redactCases();
    nameCases();
    windowGateCases();
    if (g_fails) {
        std::printf("FAIL: runtime_profile_test %d of %d checks failed\n", g_fails, g_checks);
        return 1;
    }
    std::printf("PASS: runtime_profile_test %d checks (descriptor schema 1 and 2, the v1 parser, Settings.xml mode, scope by mode, "
                "command line, decision, log line, redaction)\n", g_checks);
    return 0;
}
