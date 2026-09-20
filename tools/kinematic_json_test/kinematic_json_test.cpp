// Build gate for the production KinematicEvalProbe JSON writer. Three
// serialization failures (a dropped closing quote after a hex value, a
// misplaced quote inside an array, a broken record termination) reached
// the flight journal before this gate existed; compilation cannot catch
// any of them, so this rig serializes a deterministic fixture through the
// real writer and hands the text to tools\kinematic_json_selftest.py,
// which strict-parses it and asserts every fixture value round-trips.
// The C++ side runs a quote/brace structural scan first so a broken
// stream is diagnosed here even before the python parse.
#include "../../src/d3d11/kinematic_eval_probe.h"
#include <cstdio>
#include <cwchar>
#include <sstream>
#include <string>

namespace edvr {
// Linker stubs for the hook-install paths (arm/finish/reset), which this
// rig never calls; the pattern is set by mesh_motion_test.
const char* attachKinematicEvalHooks(KinematicEvalProbe*) noexcept {return "identity_mismatch";}
void detachKinematicEvalHooks(KinematicEvalProbe*) noexcept {}
bool kinematicEvalHooksMatch(uintptr_t) noexcept {return false;}
}
using namespace edvr;

namespace {
unsigned checks=0,failures=0;
void check(bool ok,const char* name){++checks;if(!ok){++failures;std::fprintf(stderr,"FAIL: %s\n",name);}}

// Outside string/escape state, braces must balance and every string must
// terminate. A dropped quote leaves the scanner inside a string (the rest
// of the document reads as string content) or unbalances the braces, so
// this catches the missing-quote class without a JSON library.
bool structuralScan(const std::string& s) {
    bool inStr=false,esc=false;
    int depth=0;
    for(char c:s) {
        if(inStr) {
            if(esc)esc=false;
            else if(c=='\\')esc=true;
            else if(c=='"')inStr=false;
            continue;
        }
        if(c=='"')inStr=true;
        else if(c=='{')++depth;
        else if(c=='}'){if(--depth<0)return false;}
    }
    return !inStr&&depth==0;
}
}

int wmain(int argc,wchar_t** argv) {
    if(argc!=2)return 2;
    if(!std::wcscmp(argv[1],L"--dry-run")){std::puts("kinematic_json_test: dry-run (no runtime, device or files)");return 0;}
    if(std::wcscmp(argv[1],L"--self-test"))return 2;
    kinematicEvalProbe.selfTestPopulateForJson();
    std::ostringstream j;
    j<<'{';
    kinematicEvalProbe.writeJson(j);
    j<<'}';
    const std::string out=j.str();
    check(structuralScan(out),"quote/brace balance outside escapes");
    check(out.rfind("{\"kinematicEval\":{",0)==0,"document opens with the kinematicEval object");
    check(out.size()>2&&out[out.size()-1]=='}',"document closes");
    // JSON on stdout for the python gate; diagnostics on stderr.
    std::fwrite(out.data(),1,out.size(),stdout);
    std::fputc('\n',stdout);
    std::fprintf(stderr,"kinematic_json_test: %u checks, %u failures\n",checks,failures);
    return failures?1:0;
}
