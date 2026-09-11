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

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "../../src/common/code_hook.h"
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

// THE CODE HOOK'S TARGET AND ITS REPLACEMENT.
//
// __declspec(noinline) so there is a real function to hook, and the body is
// deliberately ordinary: a compiler-generated prologue is exactly what the
// decoder has to cope with in system32\d3d11.dll.
// volatile, and for the same reason the vtable stores above are: the compiler
// can SEE codeTarget's body, sees that it never touches g_codeHookCalls, and is
// entitled to fold "did the hook run" to false without loading it. Measured --
// the cell reported the redirect had not happened while its own return value
// proved it had. An optimiser reasoning about code being changed behind its
// back is this file's recurring hazard.
static volatile int g_codeTargetCalls = 0;
static volatile int g_codeHookCalls = 0;
typedef int (*PFN_CodeTarget)(int);
static PFN_CodeTarget g_codeOriginal = nullptr;

__declspec(noinline) static int codeTarget(int x) {
    ++g_codeTargetCalls;
    return x + 7;
}

static int codeReplacement(int x) {
    ++g_codeHookCalls;
    return g_codeOriginal(x) + 100;
}

// The same write, but into a table named DIRECTLY rather than found through an
// object -- and through a volatile pointer, which is the whole point.
//
// The copy-mode cell below rewrites the vtable of a local object whose dynamic
// type the compiler knows exactly. A vtable is immutable as far as the language
// is concerned, so MSVC is entitled to treat a store into one as dead and drop
// it, and it does: measured 2026-09-06, the store vanished and the cell that
// claimed to prove "the runtime re-pointing its OWN table does not bypass a
// copy hook" had been re-pointing nothing at all since the day it was written.
// It passed for the same reason an empty test passes. writeSlot above survives
// only because its table arrives through an opaque void* the compiler cannot
// trace back to an object.
//
// volatile is what states the intent the optimiser must respect. Read the slot
// back with readTableSlot for the same reason.
static void writeTableSlot(void** table, size_t slot, void* value) {
    DWORD prot = 0;
    VirtualProtect(&table[slot], sizeof(void*), PAGE_READWRITE, &prot);
    *reinterpret_cast<void* volatile*>(&table[slot]) = value;
    DWORD ignored = 0;
    VirtualProtect(&table[slot], sizeof(void*), prot, &ignored);
}

static void* readTableSlot(void** table, size_t slot) {
    return *reinterpret_cast<void* volatile*>(&table[slot]);
}

// ONE STORE INSTRUCTION, at one address, for the flip timeline's cells.
//
// The timeline aggregates by (slot, old, new, WRITER), and the writer is the
// faulting instruction's address. A store written inline in a loop is one
// address or six depending on whether the optimiser unrolled it, so a cell
// asserting how the table collapses would be asserting something about the
// optimiser. Out of line, it is one address, always -- and that is also what
// the instrument sees in the field, where the writer is one routine in
// system32\d3d11.dll called over and over.
//
// volatile for the reason everything else in this file is: a store nobody reads
// back is a store MSVC may drop, and a cell whose write never happened passes
// the way an empty test passes.
__declspec(noinline) static void watchStore(void** table, size_t slot,
                                            void* value) {
    *reinterpret_cast<void* volatile*>(&table[slot]) = value;
}

// The same store, from a thread that is not the one draining the timeline.
struct WatchStoreJob {
    void** table;
    size_t slot;
    void*  value;
};

static DWORD WINAPI watchStoreThread(LPVOID param) {
    const WatchStoreJob* job = static_cast<const WatchStoreJob*>(param);
    watchStore(job->table, job->slot, job->value);
    return 0;
}

// A WRITER THAT DOES NOT STOP, for the two multithread cells.
//
// `a` and `b` are what it alternates between: two different pointers make every
// write a FLIP, the same pointer twice makes every write an idempotent restore
// -- which is what the runtime does hundreds of times a frame on a healthy rig,
// and the population the timeline has to drop without spending anything on.
//
// The store goes through watchStore, out of line and volatile, for the reason
// everything in this file does: a store nobody reads back is a store MSVC may
// drop, and a thread that never wrote anything cannot race anything.
struct StormJob {
    void**        table;
    size_t        slot;
    void*         a;
    void*         b;
    volatile LONG* stop;
    volatile LONG  writes;
};

static DWORD WINAPI stormThread(LPVOID param) {
    StormJob* job = static_cast<StormJob*>(param);
    LONG n = 0;
    while (!InterlockedCompareExchange(job->stop, 0, 0)) {
        watchStore(job->table, job->slot, (n & 1) ? job->b : job->a);
        ++n;
        // A pause, so four threads hammering one page for 300 ms do not spend
        // the whole run inside the handler -- the cell is about the RACE, and a
        // race needs both threads making progress.
        Sleep(0);
    }
    InterlockedExchange(&job->writes, n);
    return 0;
}

// Is the watched page read-only right now? The F2 cell's whole assertion: a
// watch that has gone blind is one whose page is writable with nothing pending,
// and no counter inside the watch can see that -- only the page can.
static bool pageIsReadOnly(const void* p) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(p, &mbi, sizeof(mbi)) != sizeof(mbi)) return false;
    return (mbi.Protect & 0xFF) == PAGE_READONLY ||
           (mbi.Protect & 0xFF) == PAGE_EXECUTE_READ;
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

    // THE IMPLEMENTATION MODULE -- the exemption that unblocked issue #21, and
    // the two ways it must stay narrow.
    //
    // The field case: Windows' d3d11.dll re-pointed ALL 29 of EDVR's patched
    // context slots 57 ms after install, before the first frame. Nothing of
    // ours ever ran, so the quiet vouch -- measured silence on a slot while
    // OTHER thunks on the object still fire -- could never be earned, reclaim
    // correctly refused every slot, and the DLL sat inert for fourteen minutes
    // and 102,210 frames explaining exactly why. An entry arriving from the
    // module that IMPLEMENTS the method cannot be a chainer, because a
    // chainer's thunk is the chainer's own code, so that case never needed the
    // vouch. setImplementationModule is how a caller says which module that is.
    //
    // It has to be TOLD, and this cell is why. The first cut inferred it -- "the
    // new entry and the one it replaced are in the same image" -- and the
    // chainer cell above failed instantly, because in this test the chainer and
    // the entries it displaces are all in one binary. Inference cannot separate
    // them; a caller's assertion can.
    {
        void* const selfModule = GetModuleHandleW(nullptr);
        // Some other loaded image, for the negative halves. Anything certainly
        // mapped and certainly not where this test's code lives.
        void* const otherModule = GetModuleHandleW(L"kernel32.dll");
        // Asserted, because a null module disables the exemption outright
        // (vtable_hook.cpp requires m_implModule non-null) -- so if either of
        // these came back null the negative cells below would pass by doing
        // nothing at all, which is the shape of a test that has quietly stopped
        // testing.
        check(selfModule != nullptr && otherModule != nullptr,
              "both module handles for the implementation-module cells resolved",
              "a null handle would make the negative cells vacuous");

        // (1) NAMED, AND THE RE-POINT COMES FROM IT: adopted with no vouch, and
        // both run afterwards. The bypasser shape, not the chainer shape --
        // adopting a chainer is a call loop no matter who vouches for it.
        {
            VTableHook owner;
            g_owner = &wrapper;
            g_ownerHits = 0;
            g_toolkitHits = 0;
            if (!owner.attach(&wrapper)) {
                fail("attach for the implementation-module cell", "attach refused");
            } else {
                owner.setImplementationModule(selfModule);
                owner.replace(0, reinterpret_cast<void*>(&thunkOne),
                              reinterpret_cast<void**>(&g_realOne));
                owner.commit();

                g_toolkitClean = reinterpret_cast<PFN_One>(wrapper.mySlotOneAtBirth);
                writeSlot(&wrapper, 0, reinterpret_cast<void*>(&toolkitOne));
                check(owner.reclaim("impl-module-cell") == 1,
                      "a re-point from the named implementation module is taken "
                      "back with no vouch at all",
                      "reclaim refused it -- issue #21's rig stays inert");

                g_ownerHits = 0;
                g_toolkitHits = 0;
                const int got = w->one();
                check(g_ownerHits == 1 && g_toolkitHits == 1,
                      "...and BOTH run afterwards, ours in front",
                      "the adopted entry was not chained to correctly");
                // 101 from the wrapper, +10000 as the adopted entry passes it
                // on, +1000 as our thunk wraps that -- the same arithmetic the
                // chainer cell's 31101 spells out, stacked the other way up.
                check(got == 11101,
                      "...with the forward pointing at the adopted entry",
                      "the composed return value is wrong");

                // The duty-cycle fact, which the totals line reports per frame.
                // The pass that found the slot taken must say so, and the next
                // pass -- with our thunk back on top -- must say the opposite,
                // or a caller sampling it once a frame is reading a number that
                // never changes.
                check(owner.lastPassDisplaced() == 1,
                      "the pass that found the slot taken reports it displaced",
                      "the displaced count did not see a slot it re-patched");
                check(owner.reclaim("impl-module-cell") == 0 &&
                          owner.lastPassDisplaced() == 0,
                      "...and the next pass, back on top, reports none",
                      "the displaced count is stale rather than per-pass");

                owner.uninstall();
                writeSlot(&wrapper, 0, wrapper.mySlotOneAtBirth);
            }
        }

        // (2) NAMED, BUT THE RE-POINT COMES FROM SOMEWHERE ELSE: still refused
        // without a vouch. The exemption is scoped to the module the caller
        // named; it is not a blanket "adopt anything". A rival tool must go on
        // earning the vouch exactly as before.
        {
            VTableHook owner;
            g_owner = &wrapper;
            g_ownerHits = 0;
            g_toolkitHits = 0;
            if (!owner.attach(&wrapper)) {
                fail("attach for the foreign-module cell", "attach refused");
            } else {
                owner.setImplementationModule(otherModule);
                owner.replace(0, reinterpret_cast<void*>(&thunkOne),
                              reinterpret_cast<void**>(&g_realOne));
                owner.commit();

                g_toolkitClean = reinterpret_cast<PFN_One>(wrapper.mySlotOneAtBirth);
                writeSlot(&wrapper, 0, reinterpret_cast<void*>(&toolkitOne));
                check(owner.reclaim("foreign-module-cell") == 0,
                      "a re-point from any OTHER module still needs the vouch",
                      "the exemption leaked past the module it was scoped to");
                check(readSlot(&wrapper, 0) == reinterpret_cast<void*>(&toolkitOne),
                      "...and the slot was left exactly as it was found",
                      "a refused reclaim wrote to the table anyway");
                check(owner.lastPassDisplaced() == 1,
                      "...and a REFUSED slot still counts as displaced",
                      "the duty-cycle count only sees slots it heals, so it "
                      "would report 100% on a rig where nothing is healing");

                owner.uninstall();
                writeSlot(&wrapper, 0, wrapper.mySlotOneAtBirth);
            }
        }

        // (3) A CHAINER FROM ELSEWHERE, WITH A MODULE NAMED: still refused.
        // Naming a module must not weaken the gate for everybody else.
        {
            VTableHook owner;
            g_owner = &wrapper;
            g_ownerHits = 0;
            g_chainHits = 0;
            if (!owner.attach(&wrapper)) {
                fail("attach for the chainer-with-module cell", "attach refused");
            } else {
                owner.setImplementationModule(otherModule);
                owner.replace(0, reinterpret_cast<void*>(&thunkOne),
                              reinterpret_cast<void**>(&g_realOne));
                owner.commit();

                g_chainSaved = reinterpret_cast<PFN_One>(readSlot(&wrapper, 0));
                writeSlot(&wrapper, 0, reinterpret_cast<void*>(&chainerOne));
                check(owner.reclaim("chainer-with-module-cell") == 0,
                      "a chainer outside the named module is still refused",
                      "the exemption leaked to a chainer -- the next call would "
                      "overflow the stack");

                g_ownerHits = 0;
                g_chainHits = 0;
                check(w->one() == 31101 && g_ownerHits == 1 && g_chainHits == 1,
                      "...and the chain still runs exactly once each",
                      "the refused pass disturbed the chain");

                owner.uninstall();
                writeSlot(&wrapper, 0, wrapper.mySlotOneAtBirth);
            }
        }

        // (3b) AN OWNER RE-POINT IS NEVER CAPPED, INCLUDING WHEN IT IS ALSO
        // VOUCHED. The cap ends a tug-of-war between two TOOLS; the module that
        // implements the method will not tire and conceding to it just switches
        // EDVR off. This was written as `ownerRepoint = !vouched && …`, so a
        // slot that happened to be vouched as well was booked against the cap
        // and retired after 64 exchanges -- the exact concession the exemption
        // exists to prevent, arriving on whichever slots the once-a-second
        // vouched pass reached before the per-frame one. 200 exchanges, all
        // vouched, all from the named module: none may be conceded.
        {
            VTableHook owner;
            g_owner = &wrapper;
            if (!owner.attach(&wrapper)) {
                fail("attach for the vouched-owner cell", "attach refused");
            } else {
                owner.setImplementationModule(selfModule);
                owner.replace(0, reinterpret_cast<void*>(&thunkOne),
                              reinterpret_cast<void**>(&g_realOne));
                owner.commit();

                const size_t quietSlot0[1] = {0};
                size_t healed = 0;
                for (int i = 0; i < 200; ++i) {
                    g_toolkitClean = reinterpret_cast<PFN_One>(readSlot(&wrapper, 0));
                    writeSlot(&wrapper, 0, reinterpret_cast<void*>(&toolkitOne));
                    healed += owner.reclaim("vouched-owner-cell", quietSlot0, 1);
                }
                check(healed == 200,
                      "200 vouched owner re-points are all taken back, none "
                      "conceded",
                      "the cap retired a slot the implementation module owns -- "
                      "on a rig whose runtime re-points every second, EDVR goes "
                      "inert about a minute in");

                owner.uninstall();
                writeSlot(&wrapper, 0, wrapper.mySlotOneAtBirth);
                g_realOne = reinterpret_cast<PFN_One>(wrapper.mySlotOneAtBirth);
            }
        }

        // (4) THE HAZARD ITSELF, PINNED DOWN. Name a module the CHAINER lives
        // in and the exemption adopts it -- forward pointing at the chainer,
        // chainer forwarding to our thunk, a two-node cycle that the next
        // dispatch would turn into a stack overflow.
        //
        // This cell asserts the broken outcome on purpose. It is not a bug
        // being tolerated: it is the exact reason setImplementationModule takes
        // an assertion from the caller instead of inferring the module from the
        // entries, and the reason its header says the named module must be one
        // that implements the methods and does not hook. An inferred rule was
        // written first and cell (3) above killed it in one run, because here
        // the chainer and the entries it displaces are all in one binary. If
        // somebody later "simplifies" the contract back to inference, this cell
        // stops failing -- and that silence is the signal to read the header.
        //
        // It never dispatches through the cycle, because a test that overflows
        // the stack reports nothing.
        {
            VTableHook owner;
            g_owner = &wrapper;
            g_ownerHits = 0;
            g_chainHits = 0;
            if (!owner.attach(&wrapper)) {
                fail("attach for the misnamed-module cell", "attach refused");
            } else {
                owner.setImplementationModule(selfModule);
                owner.replace(0, reinterpret_cast<void*>(&thunkOne),
                              reinterpret_cast<void**>(&g_realOne));
                owner.commit();

                g_chainSaved = reinterpret_cast<PFN_One>(readSlot(&wrapper, 0));
                writeSlot(&wrapper, 0, reinterpret_cast<void*>(&chainerOne));
                check(owner.reclaim("misnamed-module-cell") == 1,
                      "naming a module the chainer lives in DOES adopt it -- the "
                      "contract is the caller's to keep",
                      "the exemption no longer trusts the caller, so the header's "
                      "contract and this cell disagree");
                check(g_realOne == &chainerOne,
                      "...and that is a call cycle, which is why the module is "
                      "asserted and never inferred",
                      "the forward is not the chainer, so this cell is no longer "
                      "describing the hazard it names");

                // Break the cycle before anything can dispatch through it.
                writeSlot(&wrapper, 0, wrapper.mySlotOneAtBirth);
                g_realOne = reinterpret_cast<PFN_One>(wrapper.mySlotOneAtBirth);
                owner.uninstall();
                writeSlot(&wrapper, 0, wrapper.mySlotOneAtBirth);
            }
        }
    }

    // THE CODE HOOK -- owning the function instead of the slot.
    //
    // The mechanism issue #21 forced: on that rig Windows' d3d11.dll restores
    // the context's table every frame, so no patch to a slot survives a frame
    // and the fixes ran on 0% of them. Hooking the FUNCTION the slot names is
    // immune to that -- the table can be restored as often as its owner likes.
    //
    // What must hold: the redirect happens, the original is still reachable
    // through the trampoline, the composition is right, and uninstall puts the
    // function back byte for byte. And the decoder must REFUSE what it cannot
    // safely move, which is the half that keeps this from being a crash
    // somewhere else with EDVR nowhere on the stack.
    {
        CodeHook hook;
        g_codeTargetCalls = 0;
        g_codeHookCalls = 0;
        check(codeTarget(1) == 8, "the target behaves before it is hooked",
              "the test's own function is not what this cell assumes");

        const bool got = hook.install(reinterpret_cast<void*>(&codeTarget),
                                      reinterpret_cast<void*>(&codeReplacement),
                                      reinterpret_cast<void**>(&g_codeOriginal),
                                      "code-cell");
        check(got, "the code hook installs on an ordinary compiled function",
              "install refused a plain prologue -- the decoder is too strict to "
              "be useful, or the entry point was not aligned");
        if (got) {
            g_codeTargetCalls = 0;
            g_codeHookCalls = 0;
            const int result = codeTarget(1);
            check(g_codeHookCalls == 1,
                  "...calls to the function arrive at the replacement",
                  "the patch did not redirect the call");
            // ALSO the rip-relative relocation test, and the strongest one
            // available. codeTarget begins `FF 05 <disp32>` -- MSVC's way of
            // incrementing a global -- so this counter only moves if the
            // displacement was rewritten for the trampoline's address. Copy
            // the instruction unchanged and it increments some other four
            // bytes entirely, with nothing to show for it here.
            check(g_codeTargetCalls == 1,
                  "...and the trampoline still runs the original body, with its "
                  "rip-relative operand still addressing the right global",
                  "the original was not reachable, or its relocated "
                  "displacement now points somewhere else");
            check(result == 108,
                  "...composed in the right order",
                  "the return value says the composition is wrong");
            check(hook.stolenBytes() >= kCodeHookPatchBytes,
                  "...having relocated at least the patched bytes",
                  "fewer bytes were stolen than the patch overwrites, so the "
                  "trampoline resumes inside an instruction");

            hook.uninstall();
            g_codeTargetCalls = 0;
            g_codeHookCalls = 0;
            check(codeTarget(1) == 8 && g_codeHookCalls == 0 &&
                      g_codeTargetCalls == 1,
                  "uninstall puts the function back exactly as it was",
                  "the function did not survive being unhooked");
        }
    }

    // THE DECODER -- what it measures, where it says the displacement is, and
    // what it refuses. The refusals matter more than the successes: a length
    // this gets wrong is a crash in somebody else's code with EDVR nowhere on
    // the stack.
    {
        size_t disp = 123;   // poisoned, so "left alone" is distinguishable

        // sub rsp, 0x28 -- four bytes, the commonest prologue there is.
        const uint8_t frame[] = {0x48, 0x83, 0xEC, 0x28};
        check(codeInstructionLength(frame, sizeof(frame), &disp) == 4 && disp == 0,
              "the decoder measures `sub rsp, 0x28` and reports no displacement",
              "a prologue this common must be movable or nothing will hook");

        // mov [rsp+8], rcx -- five bytes, the parameter spill.
        const uint8_t spill[] = {0x48, 0x89, 0x4C, 0x24, 0x08};
        check(codeInstructionLength(spill, sizeof(spill), &disp) == 5,
              "...and `mov [rsp+8], rcx`",
              "the shadow-space spill was not understood");

        // push rbx.
        const uint8_t push[] = {0x53};
        check(codeInstructionLength(push, sizeof(push), &disp) == 1,
              "...and `push rbx`",
              "a one-byte push was not understood");

        // inc dword ptr [rip+disp32] -- six bytes, displacement at offset 2.
        // This is how a compiler increments a global, and it is exactly what
        // the first function this class was pointed at began with.
        const uint8_t incGlobal[] = {0xFF, 0x05, 0x11, 0x22, 0x33, 0x44};
        check(codeInstructionLength(incGlobal, sizeof(incGlobal), &disp) == 6 &&
                  disp == 2,
              "...and measures `inc [rip+disp32]`, saying where its "
              "displacement is",
              "a rip-relative global access was refused or mislocated, which "
              "would refuse most real prologues or relocate one wrongly");

        // lea rax, [rip+disp32] -- seven bytes, displacement at offset 3.
        const uint8_t ripLea[] = {0x48, 0x8D, 0x05, 0x00, 0x00, 0x00, 0x00};
        check(codeInstructionLength(ripLea, sizeof(ripLea), &disp) == 7 &&
                  disp == 3,
              "...and a REX-prefixed rip-relative lea, displacement included",
              "the prefix threw the displacement offset out, so the fixup would "
              "rewrite the wrong four bytes");

        // jmp rel32 -- a function that begins with a jump is a linker thunk or
        // somebody else's hook; following it would cut them out.
        const uint8_t jump[] = {0xE9, 0x00, 0x00, 0x00, 0x00};
        check(codeInstructionLength(jump, sizeof(jump), &disp) == 0,
              "the decoder REFUSES a relative jump",
              "it would follow a thunk or splice out another tool's hook");

        // call rel32.
        const uint8_t call[] = {0xE8, 0x00, 0x00, 0x00, 0x00};
        check(codeInstructionLength(call, sizeof(call), &disp) == 0,
              "...refuses a relative call",
              "a relocated call returns to the wrong address");

        // A truncated instruction: the buffer ends mid-operand.
        const uint8_t truncated[] = {0x48, 0x83};
        check(codeInstructionLength(truncated, sizeof(truncated), &disp) == 0,
              "...and refuses an instruction it cannot see the end of",
              "it read past the bytes it was given");
    }

    // THE WRITE WATCH -- does it catch a write, name it, and let it through?
    //
    // The instrument that answers the question every other line in this file
    // begs: not "what pointer is in the slot" but "who put it there". This is
    // the one probe in the recent run that CAN be tested in-process, because
    // the trigger is an ordinary store to a page we control, so it is tested.
    //
    // The write must still land. A watch that caught the writer and swallowed
    // the write would corrupt whatever it was watching, which on a real rig is
    // the D3D11 runtime's own dispatch table.
    {
        void** fake = static_cast<void**>(
            VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        if (!fake) {
            fail("VirtualAlloc for the write-watch cell", "allocation refused");
        } else {
            fake[3] = reinterpret_cast<void*>(&thunkOne);
            check(vtableWatchSlot(fake, 3, 8, "watch-cell"),
                  "the write watch arms on a writable page",
                  "arming refused on a plain read-write page");

            // The store that must be caught AND must succeed. volatile so the
            // compiler cannot decide a write nobody reads is not worth doing --
            // the same trap that left the copy-mode cell testing nothing.
            *reinterpret_cast<void* volatile*>(&fake[3]) =
                reinterpret_cast<void*>(&toolkitOne);

            check(vtableWatchCatches() == 1,
                  "...catches the write and names the instruction that did it",
                  "the write went through unseen, so the probe would report "
                  "nothing on the rig it was built for");
            check(*reinterpret_cast<void* volatile*>(&fake[3]) ==
                      reinterpret_cast<void*>(&toolkitOne),
                  "...and the write still landed",
                  "the watch swallowed the write it was only supposed to "
                  "observe -- on a real rig that corrupts the runtime's table");

            // Re-arm, and catch a second one, which is what the frame path does.
            vtableWatchRearm();
            *reinterpret_cast<void* volatile*>(&fake[3]) =
                reinterpret_cast<void*>(&thunkOne);
            check(vtableWatchCatches() == 2,
                  "...and re-arms to catch the next one",
                  "the watch fired once and went deaf");

            // Exhaust the budget so the disarm path runs, and require the
            // summary flag to end up set -- the field found that path printing
            // NOTHING, because the caller set the very flag the summary uses to
            // avoid printing twice, so its only call was guarded out.
            check(vtableWatchSummarised() == false,
                  "the watch has not summarised while it is still running",
                  "the summary fired early");
            vtableWatchStop();
            check(vtableWatchSummarised(),
                  "...and stopping it prints the summary rather than swallowing it",
                  "the disarm path produced no summary, which is the field bug: "
                  "thirty-two catches and then silence");
            *reinterpret_cast<void* volatile*>(&fake[3]) =
                reinterpret_cast<void*>(&toolkitOne);
            check(vtableWatchCatches() == 0,
                  "a stopped watch is silent and the page is writable again",
                  "stopping the watch did not disarm it");
            VirtualFree(fake, 0, MEM_RELEASE);
        }
    }

    // THE FLIP TIMELINE -- the same mechanism asked a different question: not
    // "who writes this table" but "when did a write CHANGE something, and what
    // did it change from".
    //
    // The distinction is the whole instrument. On the maintainer's rig the
    // runtime re-lays the context's table hundreds of times a frame with the
    // values it already held, which is why a frozen copy has never hurt him; on
    // the rig that hangs, twenty-four work-emitting slots were holding a SECOND
    // variant at the frame the GPU died, and nobody can say whether that
    // preceded the hang or followed it. A probe that reports every write cannot
    // separate those two populations. One that reports only the changes, with a
    // frame number and a microsecond on each, can.
    {
        void** fake = static_cast<void**>(
            VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        if (!fake) {
            fail("VirtualAlloc for the flip-timeline cell", "allocation refused");
        } else {
            fake[3] = reinterpret_cast<void*>(&thunkOne);
            check(vtableWatchSlot(fake, 3, 8, "timeline-cell", true),
                  "the flip timeline arms on a writable page",
                  "arming refused with the timeline asked for");
            // The render thread's identity and the frame the next events belong
            // to, exactly as the frame path publishes them.
            vtableWatchFrameTick(100);

            // A RESTORE IS NOT A FLIP. Twice, because once could be a counter
            // that has simply not been incremented yet.
            watchStore(fake, 3, reinterpret_cast<void*>(&thunkOne));
            watchStore(fake, 3, reinterpret_cast<void*>(&thunkOne));
            vtableWatchFrameTick(101);
            check(vtableWatchFlips() == 0,
                  "a write that puts the same value back is not a flip",
                  "the timeline recorded a change where nothing changed -- on a "
                  "rig with hundreds of those a frame it would record nothing "
                  "else");
            check(vtableWatchIdempotentWrites() == 2,
                  "...but it is counted, so the negative can be stated",
                  "same-value writes vanish without trace, and 'no flips' would "
                  "be indistinguishable from 'the probe never fired'");
            check(vtableWatchCatches() == 2,
                  "...and both were caught, so the watch really did see them",
                  "the writes went through unseen");

            // A CHANGE IS. One event, with both values, the slot, the frame it
            // happened in and the thread it happened on.
            watchStore(fake, 3, reinterpret_cast<void*>(&toolkitOne));
            vtableWatchFrameTick(102);
            check(vtableWatchFlips() == 1,
                  "a write that changes the value IS a flip",
                  "the change was not recorded");
            VTableFlip e{};
            check(vtableWatchFlipAt(0, &e),
                  "...and the recorded event can be read back",
                  "the ring did not publish the event");
            check(e.slot == 3 &&
                      e.before == reinterpret_cast<void*>(&thunkOne) &&
                      e.after == reinterpret_cast<void*>(&toolkitOne),
                  "...carrying the slot and BOTH values",
                  "old or new is wrong -- a changed entry only means something "
                  "next to the one it changed from");
            check(e.frame == 101,
                  "...stamped with the frame that was in progress",
                  "the frame number is not the one published before the write, "
                  "so the timeline cannot be lined up against a hang");
            check(e.thread == GetCurrentThreadId(),
                  "...and the thread that wrote it",
                  "the writing thread was not recorded");
            check(e.stackCount > 0 && e.stack[0] != nullptr,
                  "...with the writer's call stack unwound from the fault",
                  "no stack was captured, so the report names the store and not "
                  "who asked for it");
            check(vtableWatchFlipShapes() == 1,
                  "...and one distinct shape so far",
                  "the aggregate did not see the event");

            // FROM ANOTHER THREAD, which is the difference between the runtime
            // doing this inside the game's own submission and something doing
            // it from the side.
            WatchStoreJob job{fake, 3, reinterpret_cast<void*>(&chainerOne)};
            HANDLE t = CreateThread(nullptr, 0, watchStoreThread, &job, 0, nullptr);
            if (!t) {
                fail("CreateThread for the other-thread flip",
                     "the thread could not be started");
            } else {
                WaitForSingleObject(t, INFINITE);
                CloseHandle(t);
                vtableWatchFrameTick(103);
                check(vtableWatchFlips() == 2,
                      "a flip from another thread is recorded too",
                      "the watch only sees the thread that armed it");
                VTableFlip other{};
                check(vtableWatchFlipAt(1, &other) &&
                          other.thread != GetCurrentThreadId() &&
                          other.after == reinterpret_cast<void*>(&chainerOne),
                      "...and carries THAT thread's id, not the reader's",
                      "the thread id is the draining thread's, so every event "
                      "would read as the render thread's own");
            }

            // THE AGGREGATE. Six more changes through one store instruction,
            // alternating between two values: that is two shapes however many
            // times it happens, which is what keeps a runtime re-selecting
            // every frame from filling a log with the fact.
            const uint32_t shapesBefore = vtableWatchFlipShapes();
            const uint32_t flipsBefore = vtableWatchFlips();
            for (int i = 0; i < 3; ++i) {
                watchStore(fake, 3, reinterpret_cast<void*>(&toolkitOne));
                watchStore(fake, 3, reinterpret_cast<void*>(&thunkOne));
            }
            vtableWatchFrameTick(104);
            check(vtableWatchFlips() == flipsBefore + 6,
                  "six changing writes are six flips",
                  "the ring lost events between two frame ticks");
            check(vtableWatchFlipShapes() <= shapesBefore + 2,
                  "...collapsing into at most two (slot, from, to, writer) "
                  "shapes rather than six rows",
                  "the aggregate grows per event, so a runtime that re-selects "
                  "every frame overflows the table in a second");

            // And the closing report, which must actually print. The write
            // watch's own summary was found in the field never printing at all,
            // because the disarm path set the flag that guards it against
            // printing twice; this is the same assertion for the same reason.
            check(!vtableWatchFlipSummarised(),
                  "the timeline has not summarised while it is still running",
                  "the summary fired early");
            vtableWatchStop();
            check(vtableWatchFlipSummarised(),
                  "...and stopping it prints the cost and the shape table",
                  "the disarm path produced no timeline summary, so a session "
                  "ends with the events and never the tally");

            watchStore(fake, 3, reinterpret_cast<void*>(&toolkitOne));
            check(vtableWatchFlips() == flipsBefore + 6,
                  "a stopped timeline records nothing more",
                  "stopping the timeline did not disarm it");
            VirtualFree(fake, 0, MEM_RELEASE);
        }
    }

    // THE STORM -- four threads writing the watched page at once, which is the
    // shape that killed the process.
    //
    // The catch used to be claimed in three steps: read `armed` with a no-op
    // compare-exchange, spend a VirtualProtect syscall, then clear it. Two
    // threads faulting inside that window both passed, both set the trap flag,
    // and both wrote the ONE global that says whose single step is owed -- so
    // the loser's step failed the pair test, fell through to
    // EXCEPTION_CONTINUE_SEARCH, and nothing in the process handles
    // STATUS_SINGLE_STEP. Measured: four writer threads, all dead inside 50 ms,
    // three runs out of three. The instrument was killing the sessions it was
    // installed to record.
    //
    // So: the process must SURVIVE this, the chimera counter must read zero (no
    // step ever arrived on a thread that did not own the catch), writes must
    // still be caught, and every recorded event must be a real change -- a
    // thread restoring the same value must never appear as a flip, however many
    // other threads are faulting around it.
    {
        void** fake = static_cast<void**>(
            VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        if (!fake) {
            fail("VirtualAlloc for the four-writer cell", "allocation refused");
        } else {
            for (size_t i = 0; i < 8; ++i) fake[i] = reinterpret_cast<void*>(&thunkOne);
            // Slot 5's writer restores ONE value forever, so the slot must
            // already hold it: otherwise its very first write is a genuine
            // change and the "only slot 3 ever flips" assertion below would be
            // failing on a real event rather than on a mixed one.
            fake[5] = reinterpret_cast<void*>(&chainerOne);
            check(vtableWatchSlot(fake, 3, 8, "storm-cell", true),
                  "the flip timeline arms for the four-writer storm",
                  "arming refused");
            vtableWatchFrameTick(200);

            volatile LONG stop = 0;
            // Two inside the table -- one alternating (every write a flip), one
            // restoring (every write an idempotent) -- and two writing the rest
            // of the page, which is the traffic that shares a vtable's page in
            // the field and must be counted without being reported.
            StormJob jobs[4] = {
                {fake, 3, reinterpret_cast<void*>(&thunkOne),
                 reinterpret_cast<void*>(&toolkitOne), &stop, 0},
                {fake, 5, reinterpret_cast<void*>(&chainerOne),
                 reinterpret_cast<void*>(&chainerOne), &stop, 0},
                {fake, 200, reinterpret_cast<void*>(&thunkOne),
                 reinterpret_cast<void*>(&toolkitOne), &stop, 0},
                {fake, 400, reinterpret_cast<void*>(&chainerOne),
                 reinterpret_cast<void*>(&thunkOneB), &stop, 0},
            };
            HANDLE threads[4] = {};
            bool started = true;
            for (int i = 0; i < 4; ++i) {
                threads[i] = CreateThread(nullptr, 0, stormThread, &jobs[i], 0, nullptr);
                if (!threads[i]) started = false;
            }
            if (!started) {
                fail("CreateThread for the four-writer cell", "a thread refused");
                InterlockedExchange(&stop, 1);
            } else {
                // 300 ms of it, with the frame path doing exactly what the game's
                // does: re-arm what a catch left open, then drain.
                const DWORD start = GetTickCount();
                uint64_t frame = 200;
                while (GetTickCount() - start < 300) {
                    vtableWatchRearm();
                    vtableWatchFrameTick(++frame);
                    Sleep(1);
                }
                InterlockedExchange(&stop, 1);
            }
            for (int i = 0; i < 4; ++i) {
                if (!threads[i]) continue;
                WaitForSingleObject(threads[i], INFINITE);
                CloseHandle(threads[i]);
            }
            vtableWatchFrameTick(9000);

            check(true,
                  "four threads wrote the watched page for 300 ms and the "
                  "process is still alive",
                  "unreachable -- an orphaned single step kills the process "
                  "before this line");
            check(vtableWatchChimeras() == 0,
                  "...with no single step arriving on a thread that did not own "
                  "the catch",
                  "a step was handled by the wrong thread, which means two "
                  "threads believed they were mid-catch -- the state that "
                  "produced chimera events and, one instruction later, the "
                  "orphaned trap that killed the process");
            check(vtableWatchCatches() > 0,
                  "...and writes were still being caught",
                  "the watch saw nothing at all, so the cell proves nothing");

            // Every event must be a real change, and must come from one of the
            // two in-table slots. The restoring thread's writes are idempotent
            // by construction; if one of them is ever recorded as a flip, the
            // before/after pair has been mixed between two threads.
            bool allReal = true;
            bool allKnown = true;
            const uint32_t got = vtableWatchFlips();
            for (uint32_t i = 0; i < got; ++i) {
                VTableFlip e{};
                if (!vtableWatchFlipAt(i, &e)) continue;   // lapped the ring
                if (e.before == e.after) allReal = false;
                if (e.slot != 3) allKnown = false;
            }
            check(allReal,
                  "...and every recorded event is a write that really changed "
                  "the entry",
                  "an event carries the same value before and after, which is a "
                  "same-value write reported as a change");
            check(allKnown,
                  "...from the slot that was actually alternating, not the one "
                  "being restored",
                  "an event names a slot whose writer never changed its value -- "
                  "the pending fields were mixed between two threads");
            vtableWatchStop();
            VirtualFree(fake, 0, MEM_RELEASE);
        }
    }

    // THE WATCH THAT WENT BLIND, and it went blind SILENTLY, which is worse.
    //
    // The single-step handler re-protected the page and THEN set `armed`. A
    // second thread faulting between those two takes the concurrent branch,
    // which makes the page writable and returns -- and then the first thread
    // sets armed to 1 over a writable page with nothing pending. No write ever
    // faults again; vtableWatchRearm has nothing to put back because rearmWanted
    // is 0; the log shows a healthy instrument reporting nothing. Measured with
    // two writers: 282 catches in the first 250 ms and not one after.
    //
    // The cell runs two writers for half a second with the frame path re-arming
    // every simulated frame, and asserts the two things that separate a working
    // watch from a blind one: the page is READ-ONLY at the end (or one rearm
    // makes it so), and the catch count kept growing in the second half.
    {
        void** fake = static_cast<void**>(
            VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        if (!fake) {
            fail("VirtualAlloc for the blind-watch cell", "allocation refused");
        } else {
            for (size_t i = 0; i < 8; ++i) fake[i] = reinterpret_cast<void*>(&thunkOne);
            check(vtableWatchSlot(fake, 3, 8, "blind-cell", true),
                  "the flip timeline arms for the blindness cell",
                  "arming refused");
            vtableWatchFrameTick(300);

            volatile LONG stop = 0;
            StormJob jobs[2] = {
                {fake, 3, reinterpret_cast<void*>(&thunkOne),
                 reinterpret_cast<void*>(&toolkitOne), &stop, 0},
                {fake, 300, reinterpret_cast<void*>(&chainerOne),
                 reinterpret_cast<void*>(&thunkOneB), &stop, 0},
            };
            HANDLE threads[2] = {};
            threads[0] = CreateThread(nullptr, 0, stormThread, &jobs[0], 0, nullptr);
            threads[1] = CreateThread(nullptr, 0, stormThread, &jobs[1], 0, nullptr);
            uint32_t halfway = 0;
            if (!threads[0] || !threads[1]) {
                fail("CreateThread for the blind-watch cell", "a thread refused");
                InterlockedExchange(&stop, 1);
            } else {
                const DWORD start = GetTickCount();
                uint64_t frame = 300;
                while (GetTickCount() - start < 500) {
                    if (!halfway && GetTickCount() - start >= 250) {
                        halfway = vtableWatchCatches();
                    }
                    vtableWatchRearm();
                    vtableWatchFrameTick(++frame);
                    Sleep(1);
                }
                InterlockedExchange(&stop, 1);
            }
            for (int i = 0; i < 2; ++i) {
                if (!threads[i]) continue;
                WaitForSingleObject(threads[i], INFINITE);
                CloseHandle(threads[i]);
            }
            const uint32_t atEnd = vtableWatchCatches();
            check(halfway > 0 && atEnd > halfway,
                  "the watch was still catching writes in the second half of a "
                  "two-writer run",
                  "catches stopped partway through: the watch is armed over a "
                  "writable page and every write from here on is invisible");
            // The last catch on either thread has completed, so nothing is
            // mid-step: the page must be closed. One rearm is allowed, because a
            // VirtualProtect that failed inside the handler deliberately hands
            // the job to the frame path rather than lying about the page.
            if (!pageIsReadOnly(fake)) vtableWatchRearm();
            check(pageIsReadOnly(fake),
                  "...and the page is read-only again once the writers stop",
                  "the page is writable with nothing pending, which is what "
                  "being permanently blind looks like from outside");
            vtableWatchStop();
            VirtualFree(fake, 0, MEM_RELEASE);
        }
    }

    // THE WHOLE TABLE, NOT THE ANCHOR'S PAGE. A ~300-entry vtable is 2.4 KB and
    // lands wherever the heap puts it, so about a quarter of the time the page
    // holding the anchor slot does not reach the far end of the table -- and on
    // the reporting rig the far end is slot 137, whose neighbours are the whole
    // question. A write to a slot on an unprotected page raises nothing, prints
    // nothing and is mentioned nowhere, which reads exactly like a slot nobody
    // wrote.
    //
    // So: a table deliberately straddling a page boundary, and the first and
    // last slots both written. Both must be caught. Anchored on slot 0, which is
    // on the FIRST page, so the old one-page watch could not see slot 7 at all.
    {
        uint8_t* block = static_cast<uint8_t*>(
            VirtualAlloc(nullptr, 8192, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        if (!block) {
            fail("VirtualAlloc for the page-straddle cell", "allocation refused");
        } else {
            // Four slots on the first page, four on the second.
            void** table = reinterpret_cast<void**>(block + 4096 - 4 * sizeof(void*));
            for (size_t i = 0; i < 8; ++i) table[i] = reinterpret_cast<void*>(&thunkOne);
            check(vtableWatchSlot(table, 0, 8, "straddle-cell"),
                  "the write watch arms on a table that straddles a page "
                  "boundary",
                  "arming refused");
            watchStore(table, 0, reinterpret_cast<void*>(&toolkitOne));
            watchStore(table, 7, reinterpret_cast<void*>(&toolkitOne));
            check(vtableWatchCatches() == 2,
                  "...and catches writes to BOTH pages of it",
                  "one of the two writes did not fault, so the slots on that "
                  "page are not being watched at all -- and a slot nobody "
                  "watches reads in the report exactly like a slot nobody wrote");
            check(vtableWatchInTableWrites() == 2,
                  "...counting both as writes INSIDE the table",
                  "a write inside the table was booked as page traffic");
            check(readTableSlot(table, 0) == reinterpret_cast<void*>(&toolkitOne) &&
                      readTableSlot(table, 7) == reinterpret_cast<void*>(&toolkitOne),
                  "...and both writes still landed",
                  "the watch swallowed a write it was only supposed to observe");
            vtableWatchStop();
            VirtualFree(block, 0, MEM_RELEASE);
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
            // A concession is NOT displacement, and the duty-cycle figure the
            // totals line reports depends on the difference. Counted together,
            // one permanently conceded slot -- and on issue #21's rig ClearState
            // is exactly that, on the first pass -- pins the figure at "never
            // held" for the whole session and hides every slot that IS being
            // healed within a frame: the measurement reporting total failure
            // while the fix works.
            check(hookA.lastPassDisplaced() == 0 && hookA.lastPassConceded() == 1,
                  "a conceded shared slot counts as conceded, not as displaced",
                  "the duty-cycle figure will read 0% forever on any rig with "
                  "one conceded slot");

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

    // ===================================================================
    // CopyVptr mode -- the mechanism that survives the table's OWNER
    // rewriting its own entries, which is what the D3D11 runtime does and
    // what no in-place patch can hold (2026-08-18).
    // ===================================================================

    // A fresh object whose vtable we treat as "the runtime's own": the cell
    // will rewrite entries in THAT table directly, the way the runtime
    // re-points its static table between internal modes, and prove a copy
    // hook does not notice.
    {
        RealThing runtimeObj;
        IThing* r = &runtimeObj;
        void** realTable = *reinterpret_cast<void***>(&runtimeObj);
        void*  realSlot0 = realTable[0];

        VTableHook copy;
        g_owner = &runtimeObj;
        g_ownerHits = 0;
        if (!copy.attach(&runtimeObj)) {
            fail("attach for the copy-vptr cell", "attach refused");
        } else {
            check(copy.setMode(HookMode::CopyVptr),
                  "setMode(CopyVptr) is accepted before staging",
                  "the mode could not be selected");
            copy.replace(0, reinterpret_cast<void*>(&thunkOne),
                         reinterpret_cast<void**>(&g_realOne));
            check(copy.commit(), "copy-vptr commit swaps the object's vptr",
                  "commit failed");
            check(*reinterpret_cast<void***>(&runtimeObj) != realTable,
                  "the object now dispatches through our private copy",
                  "the vptr was not swapped");
            // RealThing::one() returns 1 (no wrapper +100 here), so the thunk
            // chain is 1 + 1000.
            check(r->one() == 1001, "our thunk fires through the copy",
                  "the copy hook did not take effect");

            // THE WHOLE POINT: the table OWNER re-points its own slot 0, the
            // way the runtime re-selects a variant. An in-place hook would be
            // silently bypassed here (that is the field bug). The copy never
            // saw the write, because the object stopped dispatching through
            // the real table the moment we swapped its vptr.
            writeTableSlot(realTable, 0, reinterpret_cast<void*>(&toolkitOne));
            // Proved, not assumed. This cell's whole claim rests on the table
            // really having changed, and for years it had not -- see
            // writeTableSlot.
            check(readTableSlot(realTable, 0) == reinterpret_cast<void*>(&toolkitOne),
                  "the owner's re-point actually landed in the shared table",
                  "the store was optimised away, so this cell proves nothing");

            g_ownerHits = 0;
            check(r->one() == 1001,
                  "the runtime re-pointing its OWN table does not bypass a copy hook",
                  "the copy hook was bypassed by a table-owner rewrite -- this is "
                  "the exact field failure CopyVptr exists to survive");
            check(g_ownerHits == 1, "...our thunk still ran",
                  "our thunk stopped running after the table rewrite");

            // Reclaim runs while the shared table is STILL re-pointed, which is
            // the state noteCopyDrift exists to describe. It must still change
            // nothing -- immunity is the mode's whole point -- but it must also
            // have looked: this call is the only coverage the drift walk has,
            // and it was added because the first cut of this cell restored the
            // table on the line above and then measured a drift of zero.
            check(copy.reclaim("copy-cell") == 0,
                  "reclaim is a no-op in copy mode even while the shared table "
                  "is re-pointed",
                  "copy mode tried to reclaim a table it does not share");
            check(readTableSlot(realTable, 0) == reinterpret_cast<void*>(&toolkitOne),
                  "...and the drift walk left the shared table exactly as it "
                  "found it",
                  "measuring drift wrote to the table it was measuring");
            g_ownerHits = 0;
            check(r->one() == 1001 && g_ownerHits == 1,
                  "...and dispatch through the copy is untouched by the walk",
                  "the drift walk disturbed the private copy");

            // Restore the borrowed table before uninstall compares against it.
            writeTableSlot(realTable, 0, realSlot0);

            copy.uninstall();
            check(*reinterpret_cast<void***>(&runtimeObj) == realTable,
                  "uninstall restores the object's original vptr",
                  "the vptr was not restored");
            check(r->one() == 1, "...and dispatch is stock again",
                  "the thunk survived uninstall");
        }
    }

    // THE SWAP WITH NOTHING IN IT -- the probe that separates "the private copy
    // is fatal" from "a thunk is". It must move the object onto a copy, the
    // copy must be byte-identical (every call runs the original, no thunk
    // anywhere), and uninstall must put the birth vptr back. A probe that
    // secretly patched something, or dispatched differently, would answer the
    // wrong question on the one rig it exists for.
    {
        RealThing obj;
        void** birth = *reinterpret_cast<void***>(&obj);
        IThing* r = &obj;
        VTableHook bare;
        g_owner = &obj;
        g_ownerHits = 0;
        if (!bare.attach(&obj)) {
            fail("attach for the swap-only cell", "attach refused");
        } else {
            check(bare.setMode(HookMode::CopyVptr) && bare.commitUnpatched(),
                  "an unpatched copy commits in copy mode",
                  "commitUnpatched refused a plain copy");
            check(*reinterpret_cast<void***>(&obj) != birth,
                  "...and the object now dispatches through the copy",
                  "the vptr was not moved, so the probe would test nothing");
            check(r->one() == 1 && g_ownerHits == 0,
                  "...running exactly the original code -- no thunk anywhere",
                  "something other than the original ran through a copy that "
                  "was supposed to be empty");
            check(!bare.commit(),
                  "...and commit() still refuses with nothing staged",
                  "commit accepted an empty patch list, which is a second way "
                  "to reach this state that the header does not describe");
            bare.uninstall();
            check(*reinterpret_cast<void***>(&obj) == birth && r->one() == 1,
                  "uninstall puts the birth vptr back",
                  "the object was left on a freed copy");
        }
    }

    // Copy-mode uninstall is POLITE: if a later tool swapped the object's
    // vptr on top of ours, uninstall must leave their vptr alone rather than
    // restore over them (and must not free our copy their chain still runs
    // through). Mirrors the in-place "leaves a later hooker's entry alone"
    // cell, and guards the shutdown-order independence finding-1 added.
    {
        RealThing obj;
        void** birthTable = *reinterpret_cast<void***>(&obj);

        VTableHook copy;
        g_owner = &obj;
        copy.attach(&obj);
        copy.setMode(HookMode::CopyVptr);
        copy.replace(0, reinterpret_cast<void*>(&thunkOne),
                     reinterpret_cast<void**>(&g_realOne));
        copy.commit();
        void** ourCopy = *reinterpret_cast<void***>(&obj);
        check(ourCopy != birthTable, "copy hook took the object",
              "the copy hook did not commit");

        // A third party swaps the object's vptr to a table of their own.
        static void* theirTable[8];
        for (size_t i = 0; i < 8; ++i) theirTable[i] = birthTable[i];
        theirTable[0] = reinterpret_cast<void*>(&toolkitOne);
        g_toolkitClean = reinterpret_cast<PFN_One>(birthTable[0]);
        *reinterpret_cast<void***>(&obj) = theirTable;

        copy.uninstall();
        check(*reinterpret_cast<void***>(&obj) == theirTable,
              "copy uninstall leaves a later vptr-swapper alone",
              "uninstall restored over a third party that swapped on top -- "
              "the dangling-vptr hazard finding 1 closed");
        // And dispatch through their table still works (our copy was leaked,
        // not freed, so nothing they reference was pulled out).
        IThing* o = &obj;
        check(o->one() == 10001, "...and their table still dispatches",
              "the third party's dispatch broke after our uninstall");
        *reinterpret_cast<void***>(&obj) = birthTable;  // cleanup for later cells
    }

    // Copy hooks STACK: a second copy hook on an object already copy-hooked
    // must chain through the first, and unwind in reverse. This is how the
    // exposure and vScreen hooks coexist on one runtime-owned context.
    {
        RealThing obj;
        IThing* o = &obj;
        void** birthTable = *reinterpret_cast<void***>(&obj);

        VTableHook lower, upper;
        g_owner = &obj;
        lower.attach(&obj);
        lower.setMode(HookMode::CopyVptr);
        lower.replace(0, reinterpret_cast<void*>(&thunkOne),
                      reinterpret_cast<void**>(&g_realOne));
        lower.commit();

        upper.attach(&obj);              // reads the vptr the lower hook installed
        upper.setMode(HookMode::CopyVptr);
        upper.replace(0, reinterpret_cast<void*>(&thunkOneB),
                      reinterpret_cast<void**>(&g_realOneB));
        upper.commit();

        g_ownerHits = 0;
        g_bHits = 0;
        // upper (thunkOneB, +2000) chains through lower (thunkOne, +1000)
        // chaining through RealThing::one (1): 1 + 1000 + 2000.
        check(o->one() == 3001, "two copy hooks stack, both run",
              "the stacked copy chain does not run both");
        check(g_ownerHits == 1 && g_bHits == 1, "...each exactly once",
              "stacked copy hooks did not each fire once");

        // Reverse-order unwind, the discipline the header promises.
        upper.uninstall();
        g_ownerHits = 0;
        check(o->one() == 1001, "peeling the upper copy hook leaves the lower",
              "uninstalling the upper copy hook broke the lower");
        lower.uninstall();
        check(*reinterpret_cast<void***>(&obj) == birthTable,
              "peeling both restores the birth vptr",
              "the copy stack did not unwind to the original");
        check(o->one() == 1, "...and dispatch is stock",
              "a copy hook survived the full unwind");
    }

    // ===================================================================
    // LiveCopy mode -- the vptr moves and NOTHING is frozen. Every entry of
    // the private table is a stub that reads the runtime's own slot at the
    // moment of the call, so the one property CopyVptr lacks -- following the
    // table owner -- is the property these cells are written to prove.
    // ===================================================================

    // (a) THE LIVE-ONLY COMMIT, and the property that separates it from the
    // swap-only probe above. The object moves onto the private table, every
    // call runs the original code with no thunk anywhere -- and then the
    // EMBEDDED table is re-pointed and the very next call goes to the NEW
    // function. The swap-only cell above proves the opposite for its mode; if
    // this cell ever starts behaving like that one, the stubs have stopped
    // reading and started copying.
    {
        RealThing obj;
        void** birth = *reinterpret_cast<void***>(&obj);
        void*  birthSlot0 = readTableSlot(birth, 0);
        IThing* r = &obj;
        VTableHook live;
        g_owner = &obj;
        g_ownerHits = 0;
        g_toolkitHits = 0;
        if (!live.attach(&obj)) {
            fail("attach for the live-only cell", "attach refused");
        } else {
            check(live.setMode(HookMode::LiveCopy),
                  "setMode(LiveCopy) builds the stub table",
                  "the stub page could not be allocated or made executable");
            check(live.commitLive(), "an unpatched live table commits",
                  "commitLive refused a plain live table");
            check(*reinterpret_cast<void***>(&obj) != birth,
                  "...and the object now dispatches through it",
                  "the vptr was not moved, so this cell would test nothing");
            check(r->one() == 1 && g_ownerHits == 0,
                  "...running exactly the original code -- no thunk anywhere",
                  "something other than the original ran through a live table "
                  "that has nothing patched in it");

            // THE PROPERTY. The table's owner re-selects its own entry, the way
            // Windows' d3d11.dll does to the context every frame.
            g_toolkitClean = reinterpret_cast<PFN_One>(birthSlot0);
            writeTableSlot(birth, 0, reinterpret_cast<void*>(&toolkitOne));
            check(readTableSlot(birth, 0) == reinterpret_cast<void*>(&toolkitOne),
                  "the owner's re-point actually landed in the embedded table",
                  "the store was optimised away, so this cell proves nothing");
            g_toolkitHits = 0;
            check(r->one() == 10001 && g_toolkitHits == 1,
                  "a live table FOLLOWS the owner re-pointing its own entry",
                  "the call went to the entry the table held at install -- the "
                  "stubs are freezing instead of reading, which is CopyVptr "
                  "with extra steps");

            writeTableSlot(birth, 0, birthSlot0);
            live.uninstall();
            check(*reinterpret_cast<void***>(&obj) == birth && r->one() == 1,
                  "uninstall puts the birth vptr back",
                  "the object was left on a private table");
        }
    }

    // (b) A PATCHED LIVE SLOT. The thunk fires, and its forward -- which
    // replace() handed back as the slot's STUB rather than as a function --
    // follows an embedded re-point. This is the whole reason a thunk body needs
    // no change between the modes.
    {
        RealThing obj;
        IThing* r = &obj;
        void** birth = *reinterpret_cast<void***>(&obj);
        void*  birthSlot0 = readTableSlot(birth, 0);

        VTableHook live;
        g_owner = &obj;
        g_ownerHits = 0;
        if (!live.attach(&obj) || !live.setMode(HookMode::LiveCopy)) {
            fail("attach/setMode for the patched-live cell", "refused");
        } else {
            live.replace(0, reinterpret_cast<void*>(&thunkOne),
                         reinterpret_cast<void**>(&g_realOne));
            check(live.commit(), "a patched live table commits", "commit failed");
            check(r->one() == 1001 && g_ownerHits == 1,
                  "our thunk fires through the live table",
                  "the live hook did not take effect");

            // The runtime re-selects. Our thunk must still run, and the thing it
            // forwards to must be the NEW entry -- proved by the counter inside
            // toolkitOne, not by arithmetic alone.
            g_toolkitClean = reinterpret_cast<PFN_One>(birthSlot0);
            writeTableSlot(birth, 0, reinterpret_cast<void*>(&toolkitOne));
            g_ownerHits = 0;
            g_toolkitHits = 0;
            const int got = r->one();
            check(g_ownerHits == 1 && g_toolkitHits == 1,
                  "after an embedded re-point BOTH run, our thunk in front",
                  "the thunk's forward did not follow the re-point");
            // 1 from RealThing::one, +10000 as the new entry wraps it, +1000 as
            // our thunk wraps that.
            check(got == 11001, "...composed in the right order",
                  "the return value says the composed chain is wrong");

            writeTableSlot(birth, 0, birthSlot0);
            g_ownerHits = 0;
            check(r->one() == 1001 && g_ownerHits == 1,
                  "...and it follows the re-point BACK again",
                  "the forward latched onto the new entry instead of reading "
                  "the slot at each call");

            live.uninstall();
            check(*reinterpret_cast<void***>(&obj) == birth && r->one() == 1,
                  "uninstall restores the birth vptr",
                  "a live hook survived uninstall");
        }
    }

    // (c) TWO STACKED LIVE HOOKS, in the shipped order: exposure underneath
    // (its table is the runtime's real one), vScreen on top (its table is the
    // lower hook's). The upper hook's stubs read the LOWER hook's cells, which
    // hold stubs or thunks -- two extra jumps and still nothing frozen, which
    // this cell proves by re-pointing the bottom table and requiring the call
    // to follow it through both layers.
    {
        RealThing obj;
        IThing* o = &obj;
        void** birth = *reinterpret_cast<void***>(&obj);
        void*  birthSlot0 = readTableSlot(birth, 0);

        VTableHook lower, upper;
        g_owner = &obj;
        lower.attach(&obj);
        check(lower.setMode(HookMode::LiveCopy), "the lower live hook builds",
              "setMode refused");
        lower.replace(0, reinterpret_cast<void*>(&thunkOne),
                      reinterpret_cast<void**>(&g_realOne));
        lower.commit();

        upper.attach(&obj);          // reads the vptr the lower hook installed
        check(upper.setMode(HookMode::LiveCopy), "the upper live hook builds",
              "setMode refused on a table of stubs");
        upper.replace(0, reinterpret_cast<void*>(&thunkOneB),
                      reinterpret_cast<void**>(&g_realOneB));
        upper.commit();

        g_ownerHits = 0;
        g_bHits = 0;
        check(o->one() == 3001 && g_ownerHits == 1 && g_bHits == 1,
              "two live hooks stack, both run, each once",
              "the stacked live chain does not run both exactly once");

        g_toolkitClean = reinterpret_cast<PFN_One>(birthSlot0);
        writeTableSlot(birth, 0, reinterpret_cast<void*>(&toolkitOne));
        g_ownerHits = 0;
        g_bHits = 0;
        g_toolkitHits = 0;
        check(o->one() == 13001 && g_ownerHits == 1 && g_bHits == 1 &&
                  g_toolkitHits == 1,
              "...and a re-point of the bottom table is followed through both",
              "a stacked live table froze somewhere in the middle");
        writeTableSlot(birth, 0, birthSlot0);

        upper.uninstall();
        g_ownerHits = 0;
        check(o->one() == 1001 && g_ownerHits == 1,
              "peeling the upper live hook leaves the lower",
              "uninstalling the upper live hook broke the lower");
        lower.uninstall();
        check(*reinterpret_cast<void***>(&obj) == birth && o->one() == 1,
              "peeling both restores the birth vptr",
              "the live stack did not unwind to the original");
    }

    // (d) ONE WAY TO EACH STATE. commit() is for a staged table and refuses an
    // empty list; commitLive() is for an empty one and refuses a staged list.
    // Two doors into the same room is how a log line stops meaning what it
    // says: "LIVE-ONLY PROBE" must imply no thunk exists, and the only thing
    // that can enforce that is the entry point refusing.
    {
        RealThing obj;
        void** birth = *reinterpret_cast<void***>(&obj);
        VTableHook live;
        g_owner = &obj;
        if (!live.attach(&obj) || !live.setMode(HookMode::LiveCopy)) {
            fail("attach/setMode for the live entry-point cell", "refused");
        } else {
            check(!live.commit(),
                  "commit() refuses a live table with nothing staged",
                  "commit accepted an empty patch list");
            live.replace(0, reinterpret_cast<void*>(&thunkOne),
                         reinterpret_cast<void**>(&g_realOne));
            check(!live.commitLive(),
                  "...and commitLive() refuses one with a patch staged",
                  "the live-only probe would have installed a thunk while the "
                  "log said no EDVR code was in the path");
            check(!live.commitUnpatched(),
                  "...and commitUnpatched() refuses the live mode outright",
                  "the copy-mode probe accepted a live table, so the two "
                  "probes would report each other's result");
            check(live.commit(), "...and commit() takes the staged one",
                  "commit refused a properly staged live table");
            live.uninstall();
            check(*reinterpret_cast<void***>(&obj) == birth,
                  "...cleanup restored the vptr",
                  "the entry-point cell left the object hooked");
        }
    }

    // (e) WHAT AN UNPATCHED LIVE ENTRY ACTUALLY IS, byte for byte. Everything
    // above tests behaviour, and behaviour can be produced by the wrong
    // mechanism -- a table of copied function pointers would pass cell (a) on a
    // run where nothing re-pointed. This asserts the twelve bytes:
    //
    //   48 B8 <&table[i]>   mov rax, the ADDRESS of the runtime's slot
    //   FF 20               jmp qword ptr [rax]
    //
    // If the immediate is ever the slot's CONTENTS rather than its address, the
    // stub freezes and every cell above starts passing for the wrong reason on
    // any machine where the runtime happens not to move.
    {
        RealThing obj;
        void** birth = *reinterpret_cast<void***>(&obj);
        VTableHook live;
        g_owner = &obj;
        if (!live.attach(&obj) || !live.setMode(HookMode::LiveCopy) ||
            !live.commitLive()) {
            fail("attach/commit for the stub-bytes cell", "refused");
        } else {
            void** table = *reinterpret_cast<void***>(&obj);
            bool shaped = true;
            for (size_t i = 0; i < 8 && shaped; ++i) {
                const uint8_t* stub = static_cast<const uint8_t*>(table[i]);
                void* want = static_cast<void*>(&birth[i]);
                void* got = nullptr;
                memcpy(&got, stub + 2, sizeof(got));
                shaped = stub[0] == 0x48 && stub[1] == 0xB8 && got == want &&
                         stub[10] == 0xFF && stub[11] == 0x20;
            }
            check(shaped,
                  "every unpatched live entry is `mov rax, &table[i]; jmp [rax]`",
                  "a stub does not have the shape that makes it read the live "
                  "entry -- if the immediate is the slot's value rather than "
                  "its address, this mode is a frozen copy wearing a disguise");
            check(static_cast<void*>(table[0]) !=
                      static_cast<void*>(table[1]),
                  "...and each slot has its own stub",
                  "two slots share a stub, so one of them dispatches the "
                  "other's method");
            live.uninstall();
        }
    }

    // The module probe that drives the whole decision: a vtable inside a
    // module's image reads true, one on the heap reads false. The real
    // callers probe the context against system32\d3d11.dll; here the test
    // binary's own module stands in for "the implementation".
    {
        HMODULE self = GetModuleHandleW(nullptr);
        RealThing stackObj;
        void** heapTable = *reinterpret_cast<void***>(&stackObj);  // the C++ vtable, in THIS image
        check(vtableInsideModule(heapTable, self),
              "a vtable in the module image reads as inside it",
              "the module probe missed a table in its own image");

        void* farTable[8] = {};
        for (auto& e : farTable) e = reinterpret_cast<void*>(&thunkOne);
        check(!vtableInsideModule(farTable, self),
              "a stack-allocated table reads as outside the module",
              "the module probe claimed a stack table was in the image");
        check(!vtableInsideModule(heapTable, nullptr),
              "a null module base fails safe (false -> in-place)",
              "the module probe did not fail safe on a null base");
    }

    if (g_fails) {
        printf("\nVTABLE TEST FAILED (%d)\n", g_fails);
        return 1;
    }
    printf("\nVTABLE TEST PASSED\n");
    return 0;
}
