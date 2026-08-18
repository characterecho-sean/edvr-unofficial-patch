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
//
// 2026-08-18 ADDED THE SECOND FIELD FAILURE: a tool that installs AFTER the
// in-place patches and resolves its "original" pointers from a clean table
// (OpenXR Toolkit under OpenComposite) writes over our entries and every fix
// on them starves silently. Those cells model the intruder exactly --
// clean-resolved forward, live-table write -- and hold reclaim() to its four
// promises: take a VOUCHED lone slot back so both tools run, refuse any slot
// nobody vouched as call-quiet (a chainer's slot never goes quiet, and
// re-patching over a chainer builds a call loop), refuse a slot two EDVR
// hooks share (re-patching one alone splices out the other, or loops the
// chain), and concede a tug-of-war at the cap instead of fighting forever.
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

// A second EDVR-side hook for the shared-slot cells, the shape of the real
// thing: exposure and vscreen both hook ClearState, each with its own forward.
static PFN_One  g_realOneB = nullptr;
static int      g_bHits = 0;

static int thunkOneB(IThing* self) {
    if (self != g_owner) return g_realOneB(self);
    ++g_bHits;
    return g_realOneB(self) + 2000;
}

// The intruder, the shape that broke the field: it does NOT chain through the
// slot it takes. It resolved a CLEAN entry from an object of its own (OpenXR
// Toolkit does this with a throwaway device) and forwards there, so whatever
// was in the slot before it -- us -- simply stops being called.
static PFN_One  g_toolkitClean = nullptr;
static int      g_toolkitHits = 0;

static int toolkitOne(IThing* self) {
    ++g_toolkitHits;
    return g_toolkitClean(self) + 10000;
}

// The polite neighbor, for the chainer cell: it captured what the slot held
// (our thunk) and forwards to it, the way overlays and other in-place hookers
// compose. Re-patching over THIS one is how a call loop is built, which is
// why reclaim must refuse a slot nobody vouched as quiet -- a chainer keeps
// our thunk running, so its slot never earns the vouch.
static PFN_One  g_chainSaved = nullptr;
static int      g_chainHits = 0;

static int chainerOne(IThing* self) {
    ++g_chainHits;
    return g_chainSaved(self) + 30000;
}

// A third party's write into the live table, without a VTableHook.
static void writeSlot(void* object, size_t slot, void* value) {
    void** vt = *reinterpret_cast<void***>(object);
    DWORD prot = 0;
    VirtualProtect(&vt[slot], sizeof(void*), PAGE_READWRITE, &prot);
    vt[slot] = value;
    DWORD ignored = 0;
    VirtualProtect(&vt[slot], sizeof(void*), prot, &ignored);
}

static void* readSlot(void* object, size_t slot) {
    return (*reinterpret_cast<void***>(object))[slot];
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
            // Cleanup for the cells below: the slot still holds `theirs`.
            writeSlot(&wrapper, 0, wrapper.mySlotOneAtBirth);
        }
    }

    // THE CELL THAT FAILED THE FIELD, SECOND EDITION (2026-08-18). A tool that
    // installs after us and resolves its "original" pointers from a CLEAN
    // table -- OpenXR Toolkit under OpenComposite -- writes over our entries
    // and we are simply gone: no crash, no log, the fixes starve. reclaim()
    // must notice, adopt their entry as the new forward, and take the slot
    // back so both run.
    {
        VTableHook third;
        g_owner = &wrapper;
        g_ownerHits = 0;
        g_toolkitHits = 0;
        if (!third.attach(&wrapper)) {
            fail("attach for the reclaim cell", "attach refused");
        } else {
            third.replace(0, reinterpret_cast<void*>(&thunkOne),
                          reinterpret_cast<void**>(&g_realOne));
            third.commit();
            check(w->one() == 1101, "our thunk fires before the intruder arrives",
                  "the baseline is already wrong");

            g_toolkitClean = reinterpret_cast<PFN_One>(wrapper.mySlotOneAtBirth);
            writeSlot(&wrapper, 0, reinterpret_cast<void*>(&toolkitOne));

            g_ownerHits = 0;
            check(w->one() == 10101,
                  "the clean-original intruder bypasses us entirely",
                  "the intruder did not take the slot -- the cell is not testing "
                  "the field failure");
            check(g_ownerHits == 0, "...and our thunk never ran",
                  "our thunk ran despite the clobber; the failure being tested "
                  "did not happen");

            // The owner's side of the contract: our thunk went quiet (it did
            // -- zero hits above), so slot 0 is vouched. Without the vouch,
            // reclaim must not touch it; that refusal is the chainer cell's
            // job below.
            const size_t quiet0[] = {0};
            check(third.reclaim("reclaim-cell") == 0,
                  "an unvouched slot is not taken back even from a bypasser",
                  "reclaim re-patched without call evidence");
            check(third.reclaim("reclaim-cell", quiet0, 1) == 1,
                  "reclaim re-patches the vouched re-pointed slot",
                  "reclaim did not take the slot back");
            g_ownerHits = 0;
            g_toolkitHits = 0;
            check(w->one() == 11101,
                  "after reclaim BOTH hooks run, ours first",
                  "the reclaimed chain does not run both hooks");
            check(g_ownerHits == 1 && g_toolkitHits == 1,
                  "...each exactly once -- no loop, no double-dispatch",
                  "hit counts say the chain is wrong");

            check(third.reclaim("reclaim-cell", quiet0, 1) == 0,
                  "a healthy pass reclaims nothing",
                  "reclaim rewrote a slot that already held our thunk");

            // Their liveness check takes it back; ours must take it back again
            // without re-wiring anything it already wired.
            writeSlot(&wrapper, 0, reinterpret_cast<void*>(&toolkitOne));
            check(third.reclaim("reclaim-cell", quiet0, 1) == 1,
                  "reclaim wins the second exchange too",
                  "the second reclaim did not re-patch");
            check(w->one() == 11101, "and the chain is unchanged",
                  "the second reclaim changed the chain's meaning");

            // Polite exit: the slot goes to the intruder we chained to, not to
            // the clean entry -- their hook survives our uninstall.
            third.uninstall();
            check(readSlot(&wrapper, 0) == reinterpret_cast<void*>(&toolkitOne),
                  "uninstall hands the slot to the intruder we chained to",
                  "uninstall restored the pre-intruder entry and cut them out");
            check(w->one() == 10101, "...and dispatch still works through them",
                  "dispatch broke after uninstall");
            writeSlot(&wrapper, 0, wrapper.mySlotOneAtBirth);
        }
    }

    // THE CHAINER -- the OTHER loop hazard, and the reason reclaim demands a
    // vouch. A tool that hooks the way EDVR does captures our thunk from the
    // slot and forwards to it: both run, exactly as the header promises. Its
    // slot still reads "not ours", and a reclaim that acted on that alone
    // would adopt an entry whose forward is US -- a two-node call cycle,
    // found by the next call as a stack overflow. The discriminator is call
    // traffic: a chainer keeps our thunk firing, so the owner never vouches
    // its slot, so reclaim never touches it. This cell holds that line.
    {
        VTableHook fourth;
        g_owner = &wrapper;
        g_ownerHits = 0;
        g_chainHits = 0;
        if (!fourth.attach(&wrapper)) {
            fail("attach for the chainer cell", "attach refused");
        } else {
            fourth.replace(0, reinterpret_cast<void*>(&thunkOne),
                           reinterpret_cast<void**>(&g_realOne));
            fourth.commit();

            // The chainer arrives after us and composes politely: it saves
            // OUR thunk as its forward and takes the slot.
            g_chainSaved = reinterpret_cast<PFN_One>(readSlot(&wrapper, 0));
            writeSlot(&wrapper, 0, reinterpret_cast<void*>(&chainerOne));

            check(w->one() == 31101, "the chainer runs both hooks, theirs first",
                  "the chainer composition is not what this cell assumes");
            check(g_ownerHits == 1 && g_chainHits == 1,
                  "...one hit each -- our thunk is demonstrably still called",
                  "hit counts contradict the chained composition");

            // Our thunk fires every call, so no honest owner vouches slot 0.
            // Reclaim must leave the chainer alone -- and keep leaving it
            // alone on every later pass, because this state is PERMANENT and
            // healthy, not a clobber awaiting repair.
            check(fourth.reclaim("chainer-cell") == 0,
                  "reclaim refuses the chained slot without a vouch",
                  "reclaim re-patched over a chainer -- this is the call-loop "
                  "bug, and the next dispatch would overflow the stack");
            check(fourth.reclaim("chainer-cell") == 0,
                  "...and stays refused on the next pass",
                  "the refusal did not hold across passes");
            g_ownerHits = 0;
            g_chainHits = 0;
            check(w->one() == 31101 && g_ownerHits == 1 && g_chainHits == 1,
                  "the chain is untouched after the refused passes",
                  "a refused reclaim still changed dispatch");

            // Polite exit under a chainer: the slot is theirs now, so
            // uninstall leaves it -- our thunk stays reachable through their
            // forward, which is the pre-existing uninstall contract.
            fourth.uninstall();
            check(readSlot(&wrapper, 0) == reinterpret_cast<void*>(&chainerOne),
                  "uninstall leaves the chainer in place",
                  "uninstall clobbered the chainer");
            writeSlot(&wrapper, 0, wrapper.mySlotOneAtBirth);
        }
    }

    // THE SHARED SLOT -- the loop hazard. ClearState is hooked by BOTH context
    // hooks in the real DLL, stacked. The lower one must read the upper as a
    // healthy chain, not a clobber: "reclaiming" it would splice the upper out,
    // and one more round of that points the two forwards at each other, which
    // is an infinite call loop. And when a REAL intruder takes a shared slot,
    // neither owner may re-patch alone -- the slot is reported and conceded.
    {
        VTableHook hookA, hookB;
        g_owner = &wrapper;
        if (!hookA.attach(&wrapper) || !hookB.attach(&wrapper)) {
            fail("attach for the shared-slot cell", "attach refused");
        } else {
            hookA.replace(0, reinterpret_cast<void*>(&thunkOne),
                          reinterpret_cast<void**>(&g_realOne));
            hookA.commit();
            hookB.replace(0, reinterpret_cast<void*>(&thunkOneB),
                          reinterpret_cast<void**>(&g_realOneB));
            hookB.commit();

            g_ownerHits = 0;
            g_bHits = 0;
            check(w->one() == 3101, "two EDVR hooks stack on one slot",
                  "the stacked chain does not run both");

            // Vouched on purpose in every call below: the shared-slot refusal
            // must hold BEFORE the vouch is consulted, or a co-owner whose
            // thunk is idle (the lower of a stack legitimately can be) gets
            // adopted the moment its counter reads quiet.
            const size_t quiet0[] = {0};
            check(hookA.reclaim("shared-A", quiet0, 1) == 0,
                  "the lower hook reads the upper as healthy, not as a clobber",
                  "the lower hook re-patched over the upper -- this is the "
                  "call-loop bug");
            check(w->one() == 3101, "...and touched nothing",
                  "a zero-reclaim pass still changed dispatch");

            g_toolkitClean = reinterpret_cast<PFN_One>(wrapper.mySlotOneAtBirth);
            writeSlot(&wrapper, 0, reinterpret_cast<void*>(&toolkitOne));

            check(hookA.reclaim("shared-A", quiet0, 1) == 0 &&
                      hookB.reclaim("shared-B", quiet0, 1) == 0,
                  "a shared slot taken by an intruder is conceded by both owners",
                  "an owner re-patched a shared slot alone");
            check(readSlot(&wrapper, 0) == reinterpret_cast<void*>(&toolkitOne),
                  "...and the intruder keeps it",
                  "somebody rewrote the shared slot after all");

            hookB.uninstall();
            hookA.uninstall();
            writeSlot(&wrapper, 0, wrapper.mySlotOneAtBirth);
            check(w->one() == 101, "the shared-slot cell cleaned up after itself",
                  "cleanup left the slot wrong for later cells");
        }
    }

    // THE CAP. Against an intruder that re-checks its hooks the way we do, the
    // slot would trade hands every second forever. After 64 exchanges the slot
    // is conceded -- loudly -- and stays theirs.
    {
        VTableHook fought;
        g_owner = &wrapper;
        if (!fought.attach(&wrapper)) {
            fail("attach for the cap cell", "attach refused");
        } else {
            fought.replace(0, reinterpret_cast<void*>(&thunkOne),
                           reinterpret_cast<void**>(&g_realOne));
            fought.commit();
            g_toolkitClean = reinterpret_cast<PFN_One>(wrapper.mySlotOneAtBirth);

            const size_t quiet0[] = {0};
            int exchanges = 0;
            for (int i = 0; i < 200; ++i) {
                writeSlot(&wrapper, 0, reinterpret_cast<void*>(&toolkitOne));
                if (fought.reclaim("cap-cell", quiet0, 1) == 0) break;
                ++exchanges;
            }
            check(exchanges == 64, "the tug-of-war is conceded after 64 exchanges",
                  "the cap did not hold at 64");
            check(readSlot(&wrapper, 0) == reinterpret_cast<void*>(&toolkitOne),
                  "...and the conceded slot stays with the intruder",
                  "the slot changed hands after the concession");
            check(w->one() == 10101, "...still dispatching correctly through them",
                  "dispatch broke after the concession");

            fought.uninstall();
            writeSlot(&wrapper, 0, wrapper.mySlotOneAtBirth);
        }
    }

    if (g_fails) {
        printf("\nVTABLE TEST FAILED (%d)\n", g_fails);
        return 1;
    }
    printf("\nVTABLE TEST PASSED\n");
    return 0;
}
