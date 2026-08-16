// vtable_test -- the object-wrapping collision, reproduced without ReShade.
//
// WHY THIS EXISTS
//
// EDVR crashed users' games when ReShade was installed as dxgi.dll (issue #6,
// confirmed by the reporter, and "random" for others because the crash
// sentinel disables the fixes every other launch). The mechanism was in
// VTableHook: it copied an object's vtable and pointed the OBJECT'S VPTR at
// the copy. In isolation that is fine. Against a mod that wraps D3D11 objects
// in proxies of its own, it re-points an object somebody else owns and
// dispatches through -- and the field fingerprint was visible in every log:
// `exposure fix installed on ... (149 methods)` with ReShade against 302
// without, EDVR attaching to a wrapper's vtable instead of the real one.
//
// A harness that cannot produce the failure proves nothing (the fakechain
// lesson). So these cells are written against the OLD mechanism first and
// must fail on it; they pass only once entries are patched in place.
//
// WHAT IT MODELS
//
// A ReShade-shaped wrapper: its own vtable, forwarding to a real object,
// and -- the part that matters -- bookkeeping that assumes its own identity
// is stable. ReShade resolves its own methods by address; anything that
// moves the object's vptr into another module's heap invalidates that.
//
// No D3D11, no GPU, no ReShade install: the collision is vtable mechanics,
// so it is tested as vtable mechanics and runs in every build.
#include <windows.h>

#include <cstdio>

#include "../../src/common/vtable_hook.h"

using namespace edvr;

static int g_fails = 0;

static void ok(const char* what) { printf("  ok    %s\n", what); }
static void fail(const char* what, const char* detail) {
    printf("  FAIL  %s -- %s\n", what, detail);
    ++g_fails;
}
static void check(bool cond, const char* what, const char* detail) {
    if (cond) ok(what); else fail(what, detail);
}

// Eight slots: probeVTableLength refuses anything under four as implausible,
// and a few spare slots let a cell prove the UNPATCHED ones are untouched.
struct IThing {
    virtual int one() = 0;
    virtual int two() = 0;
    virtual int three() = 0;
    virtual int four() = 0;
    virtual int five() = 0;
    virtual int six() = 0;
    virtual int seven() = 0;
    virtual int eight() = 0;
};

struct RealThing : IThing {
    int one() override { return 1; }
    int two() override { return 2; }
    int three() override { return 3; }
    int four() override { return 4; }
    int five() override { return 5; }
    int six() override { return 6; }
    int seven() override { return 7; }
    int eight() override { return 8; }
};

// The wrapper, shaped like ReShade's: it forwards to the real object, and it
// remembers what its own dispatch is supposed to look like so it can notice
// when somebody moves it.
struct WrapThing : IThing {
    IThing* inner = nullptr;
    void**  myVTableAtBirth = nullptr;
    void*   mySlotOneAtBirth = nullptr;

    explicit WrapThing(IThing* real) : inner(real) {
        myVTableAtBirth = *reinterpret_cast<void***>(this);
        mySlotOneAtBirth = myVTableAtBirth[0];
    }
    // Does this object still dispatch through the table its class defines?
    bool identityIntact() const {
        return *reinterpret_cast<void* const* const*>(this) == myVTableAtBirth;
    }
    int one() override { return inner->one() + 100; }
    int two() override { return inner->two() + 100; }
    int three() override { return inner->three() + 100; }
    int four() override { return inner->four() + 100; }
    int five() override { return inner->five() + 100; }
    int six() override { return inner->six() + 100; }
    int seven() override { return inner->seven() + 100; }
    int eight() override { return inner->eight() + 100; }
};

// EDVR's side: one thunk on slot 0, with the owner check that in-place
// patching makes mandatory -- the patch is on the CLASS, so every object
// sharing the vtable arrives here and only ours may be treated as ours.
typedef int (*PFN_One)(IThing*);
static PFN_One  g_realOne = nullptr;
static void*    g_owner = nullptr;
static int      g_ownerHits = 0;
static int      g_foreignForwarded = 0;

static int thunkOne(IThing* self) {
    if (self != g_owner) {
        ++g_foreignForwarded;
        return g_realOne(self);
    }
    ++g_ownerHits;
    return g_realOne(self) + 1000;
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    printf("edvr vtable / wrapper collision\n");

    RealThing real;
    WrapThing wrapper(&real);
    WrapThing sibling(&real);   // same class, same vtable, NOT ours

    IThing* w = &wrapper;
    IThing* s = &sibling;

    check(w->one() == 101, "the wrapper forwards before anything is hooked",
          "wrapper dispatch was already wrong");

    VTableHook hook;
    g_owner = &wrapper;
    if (!hook.attach(&wrapper)) {
        fail("attach to the wrapper", "attach refused a plausible vtable");
        printf("\nVTABLE TEST FAILED (%d)\n", g_fails);
        return 1;
    }
    hook.replace(0, reinterpret_cast<void*>(&thunkOne),
                 reinterpret_cast<void**>(&g_realOne));
    check(hook.commit(), "commit patches the table",
          "commit refused -- the vtable page could not be made writable");

    // THE CELL THAT FAILED THE FIELD. A wrapper mod dispatches through its
    // own objects and resolves its own methods by address. Moving the
    // object's vptr to a private copy breaks both, first or second, and no
    // ordering fixes it.
    check(wrapper.identityIntact(),
          "the wrapper still dispatches through its own vtable",
          "the object's vptr was re-pointed at somebody else's table -- this "
          "is the ReShade collision (issue #6)");

    check(w->one() == 1101, "our thunk fires for the object we attached to",
          "the hook did not take effect on the owner");
    check(g_ownerHits == 1, "and it counted exactly one owner call", "wrong hit count");

    // In-place patching hooks the CLASS. A second object of the same class
    // reaches our thunk too, and must leave with its own behaviour intact.
    const int before = g_foreignForwarded;
    check(s->one() == 101, "a sibling object sharing the vtable is unaffected",
          "the owner check did not forward a foreign object");
    check(g_foreignForwarded == before + 1,
          "...and the forward was counted rather than silently skipped",
          "the sibling never reached the thunk, so the check is untested");

    // Slots we never asked for stay exactly as the class defined them.
    check(w->five() == 105, "an unpatched slot is untouched",
          "a slot nobody replaced changed behaviour");

    // Polite unhook: restore only what still points at us.
    hook.uninstall();
    check(wrapper.identityIntact(), "identity is still intact after uninstall",
          "uninstall moved the vptr");
    check(w->one() == 101, "uninstall restored the original entry",
          "the thunk survived uninstall");

    // Somebody hooked after us and we must not clobber them: patch the slot
    // behind our back, then uninstall, and their entry has to survive.
    {
        VTableHook second;
        g_owner = &wrapper;
        g_ownerHits = 0;
        if (!second.attach(&wrapper)) {
            fail("re-attach for the clobber cell", "attach refused");
        } else {
            second.replace(0, reinterpret_cast<void*>(&thunkOne),
                           reinterpret_cast<void**>(&g_realOne));
            second.commit();
            // A third party patches the same slot after us.
            void** vt = *reinterpret_cast<void***>(&wrapper);
            DWORD prot = 0;
            VirtualProtect(&vt[0], sizeof(void*), PAGE_READWRITE, &prot);
            void* theirs = wrapper.mySlotOneAtBirth;   // pretend: their own thunk
            vt[0] = theirs;
            DWORD ignored = 0;
            VirtualProtect(&vt[0], sizeof(void*), prot, &ignored);

            second.uninstall();
            void** after = *reinterpret_cast<void***>(&wrapper);
            check(after[0] == theirs,
                  "uninstall leaves a later hooker's entry alone",
                  "we restored over somebody who hooked after us");
        }
    }

    if (g_fails) {
        printf("\nVTABLE TEST FAILED (%d)\n", g_fails);
        return 1;
    }
    printf("\nVTABLE TEST PASSED\n");
    return 0;
}
