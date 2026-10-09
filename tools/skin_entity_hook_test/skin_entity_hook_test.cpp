// skin_entity_hook_test: the read-only hook on the game's skinning-job assembly (src/d3d11/skin_entity_hook.cpp), driven on a synthetic game function.
//
//   --dry-run    the same run (the rig never writes a file), for the gate's --dry-run convention
//   --self-test  every check; the optional argument is the repository root (unused here)
//
// The production source carries the test (compiled in with EDVR_SKIN_HOOK_TEST, which also redirects the hook's target from the game's
// EliteDangerous64.exe+0x4C540E0 to a function built here: the real 28-byte prologue, a witness store, the epilogue). The real CodeHook patches
// it, the real relay runs, the real guarded reads walk a heap laid out the way the decompile says the game's list is. Cases:
//   H1  before arming nothing exists: no state, no snapshot, the game's function untouched
//   H2  a function whose prologue is not build 332841's stands the hook down with the reason, nothing patched
//   H3  the right prologue arms it (the line says READ ONLY and the byte counts); arming twice changes nothing
//   H4  a call runs the original FIRST (the end row is read after it), then the list is read whole: addresses, bases, counts, thread, sequence
//   H5  the first call is reported with its thread
//   H6  with the gate shut the original still runs and nothing is observed
//   H7  a pointer into unmapped memory is a counted fault, the entries before it kept, and the game is unharmed
//   H8  a pointer that cannot be an object is flagged implausible
//   H9  a list that points back into itself ends at the snapshot's capacity
//   H10 recovery, and the sequence numbers run on
//   H11 a call from another thread is reported
//   H12 a reader against the game's thread: every copy is one call's list, never a mix
//   H13 a second node stands the hook down: the stand-down is reported, no snapshot is offered, the original keeps running
//   H14 lists of no entries (a menu, a loading screen) judge nothing, and a dispatch with no jobs judges no list
//   H15 the bound stays for lists with something to read: 120 unusable ones (a list of no entries but an odd end row, or one that faulted, is one of
//       those) stand the hook down, empty lists between them do not count
//   H16 an empty list is judged by a chain dispatch that has jobs: 120 of them stand the hook down
//   H17 one usable list ends the judging for good
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "skin_entity_hook.h"

namespace edvr {
void breadcrumb(const char*) {}   // production guard.cpp's crash-channel dependency (proxy.cpp), as the other rigs stub it
}  // namespace edvr

// The case ids the production self-test labels its failures with (tools\skin_entity_hook_test\mutants.py reads this list to hold each case to a mutation).
static const char* const kCases[] = {"H1.unarmed",  "H2.prologue",   "H3.arm",         "H4.read",      "H5.first-call", "H6.gate",    "H7.fault",
                                     "H8.implausible", "H9.cycle", "H10.recovery", "H11.thread", "H12.tear",       "H13.second-node", "H14.empty-idle",
                                     "H15.bound",      "H16.empty-with-jobs", "H17.usable-ends-judging"};

int main(int argc, char** argv) {
    bool selfTest = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--self-test" || a == "--dry-run") selfTest = true;
        else if (i == 2 && argv[1] == std::string("--self-test")) continue;   // the repository root
        else {
            std::fprintf(stderr, "usage: skin_entity_hook_test --self-test [repository root] | --dry-run\n");
            return 2;
        }
    }
    if (!selfTest) {
        std::fprintf(stderr, "usage: skin_entity_hook_test --self-test [repository root] | --dry-run\n");
        return 2;
    }
    static char detail[8192];
    const unsigned failures = edvr::skinEntityHookSelfTest(detail, sizeof(detail));
    if (failures) {
        std::fputs(detail, stderr);
        std::fprintf(stderr, "FAIL: skin_entity_hook_test: %u check(s) failed\n", failures);
        return 1;
    }
    std::printf("PASS: skin_entity_hook_test (%zu cases:", sizeof(kCases) / sizeof(kCases[0]));
    for (const char* c : kCases) std::printf(" %s", c);
    std::printf(")\n");
    return 0;
}
