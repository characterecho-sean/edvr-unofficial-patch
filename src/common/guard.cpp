#include "guard.h"

#include <windows.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>  // strncmp, to tell a probing site from a D3D one

#include "log.h"
#include "proxy.h"  // breadcrumb(), the last-resort channel

namespace edvr {

// ONCE PER SITE, which is what guard.h has always promised and what this did
// not do.
//
// Every fault got a formatted line. That is fine for the faults this was
// written for -- a handful, from a hook touching something unexpected -- and
// ruinous for the one that came later: a memory scan walking freed pages can
// take thousands in a single frame, and the log fills with the same line while
// the thing being diagnosed scrolls out of reach. The instrument destroys the
// evidence, which is the failure this project keeps meeting from new
// directions.
//
// Sites are string literals at the call, so pointer identity is the cheapest
// correct key: no hashing, no allocation, and nothing to get wrong in a
// context where the process may be part-way through faulting.
//
// The COUNT is kept and reported at the end, because "once" must not mean the
// total is lost -- a site that faulted twice and a site that faulted forty
// thousand times are different problems, and the first line looks identical.
namespace {

struct FaultSite {
    const char* site;
    uint64_t    count;
};

// More than the codebase has sites; a scan that somehow exceeded it would fall
// through to logging every fault, which is the old behaviour rather than a new
// failure.
constexpr unsigned kMaxSites = 32;
FaultSite g_sites[kMaxSites];
unsigned  g_siteCount = 0;

// WHERE A FAULT HAPPENED, as text, without allocating.
//
// A site is a string, and some sites are whole subsystems: one fault budget
// covers the entire frame-boundary block, so a note that named only the site
// could not say WHICH of dozens of ticks, or whose code, had faulted. The
// address answers that, and the module and offset turn it into something a
// map file or a disassembler can be pointed at.
//
// Everything here runs inside an exception filter, on whatever thread faulted
// and part-way through whatever it was doing: stack buffers only, no heap, and
// nothing taken on trust from EDVR's own state -- Windows is asked instead.
constexpr size_t kWhereCap = 384;   // the whole location text
constexpr size_t kPlaceCap = 128;   // one address: module+offset, or what it is
constexpr size_t kLeafCap = 64;     // a module's file name, clipped

// Appends printf-formatted text to a NUL-terminated buffer, clipping silently.
void appendf(char* out, size_t cap, const char* fmt, ...) {
    const size_t used = strlen(out);
    if (used + 1 >= cap) return;
    va_list args;
    va_start(args, fmt);
    _vsnprintf_s(out + used, cap - used, _TRUNCATE, fmt, args);
    va_end(args);
}

// "leaf.dll+0xOFFSET" when the address lies inside a loaded image, otherwise
// what it lies in.
//
// VirtualQuery -> AllocationBase -> GetModuleFileNameW: an image is mapped as
// one allocation, so the base of that allocation IS the module handle, and the
// distance from it is the RVA. Only the leaf of the path is kept -- a full path
// is long and says where a player keeps their games. Memory that is not an
// image (heap, stack, the trampolines EDVR allocates itself) has no module;
// its region base and offset are given instead, so an address can still be
// matched against a line that logged the allocation.
void describeAddress(const void* address, char* out, size_t cap) {
    MEMORY_BASIC_INFORMATION mbi = {};
    if (address == nullptr || VirtualQuery(address, &mbi, sizeof(mbi)) != sizeof(mbi) ||
        mbi.State == MEM_FREE) {
        _snprintf_s(out, cap, _TRUNCATE, "no module: not mapped");
        return;
    }
    const uintptr_t base = reinterpret_cast<uintptr_t>(mbi.AllocationBase);
    const uintptr_t offset = reinterpret_cast<uintptr_t>(address) - base;
    if (mbi.Type == MEM_IMAGE) {
        wchar_t path[MAX_PATH];
        const DWORD length =
            GetModuleFileNameW(reinterpret_cast<HMODULE>(mbi.AllocationBase), path, MAX_PATH);
        if (length > 0 && length < MAX_PATH) {
            const wchar_t* slash = wcsrchr(path, L'\\');
            const wchar_t* leaf = slash ? slash + 1 : path;
            // Narrowed by hand: a module's name is ASCII in practice, and a
            // conversion call is one more thing to go wrong in a filter.
            char narrow[kLeafCap];
            size_t i = 0;
            for (; leaf[i] != L'\0' && i + 1 < sizeof(narrow); ++i) {
                narrow[i] = leaf[i] < 0x80 ? static_cast<char>(leaf[i]) : '?';
            }
            narrow[i] = '\0';
            _snprintf_s(out, cap, _TRUNCATE, "%s+0x%llX", narrow,
                        static_cast<unsigned long long>(offset));
            return;
        }
    }
    const char* kind = mbi.Type == MEM_IMAGE    ? "image"
                       : mbi.Type == MEM_MAPPED ? "mapped"
                                                : "private";
    _snprintf_s(out, cap, _TRUNCATE, "no module: %s region 0x%016llX+0x%llX", kind,
                static_cast<unsigned long long>(base), static_cast<unsigned long long>(offset));
}

// The location half of a note, each part with its own leading space so it can
// be spliced after "site=%s": the faulting instruction's address with its
// module+offset, and for a memory fault the address it touched and how.
// Empty when the filter was reached without exception pointers -- the note is
// then what it always was.
void describeFault(unsigned long code, char* out, size_t cap) {
    if (cap == 0) return;
    out[0] = '\0';
    const detail::GuardFaultWhere& where = detail::guardFaultWhere();
    if (!where.valid) return;

    // A stack overflow is judged on the last of the stack. A path buffer and a
    // loader call are the wrong way to spend it, and a filter that overflows
    // again turns a caught fault into the crash this file exists to prevent.
    const bool stackGone = code == EXCEPTION_STACK_OVERFLOW;

    char place[kPlaceCap];
    if (stackGone) {
        _snprintf_s(place, _TRUNCATE, "stack overflow: module lookup skipped");
    } else {
        describeAddress(where.ip, place, sizeof(place));
    }
    appendf(out, cap, " at=0x%016llX (%s)",
            static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(where.ip)), place);

    if (where.hasData) {
        describeAddress(where.data, place, sizeof(place));
        const char* kind = where.access == 0   ? "read"
                           : where.access == 1 ? "write"
                           : where.access == 8 ? "execute"
                                               : nullptr;
        if (kind != nullptr) {
            appendf(out, cap, " access=%s", kind);
        } else {
            appendf(out, cap, " access=%u", where.access);
        }
        appendf(out, cap, " data=0x%016llX (%s)",
                static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(where.data)), place);
    }
}

}  // namespace

int guardFilter(unsigned long code, const char* site) {
    const char* key = site ? site : "?";
    for (unsigned i = 0; i < g_siteCount; ++i) {
        if (g_sites[i].site == key) {
            ++g_sites[i].count;
            // Restate the running total at every power of two. The shutdown
            // summary this used to defer to NEVER WRITES in the field -- the
            // game exits by TerminateProcess, so no log on the reporting rig
            // has ever ended with one -- and a count nobody can see is a
            // count that does not exist. Doubling keeps it to a handful of
            // lines however bad it gets, and makes runaway visible AS runaway.
            if ((g_sites[i].count & (g_sites[i].count - 1)) == 0) {
                // The LATEST fault's location rides on the restatement, and
                // only here: a shared budget's later faults may come from a
                // different module than the first, and the first note is the
                // only other place an address is written. Built only when the
                // line is, so a scan that faults thousands of times a frame
                // pays a counter increment and nothing more.
                char where[kWhereCap];
                describeFault(code, where, sizeof(where));
                char latest[kWhereCap + 16] = "";
                if (where[0] != '\0') _snprintf_s(latest, _TRUNCATE, " Latest fault%s.", where);
                Log::get().note("FAULT TOTAL site=%s: %llu absorbed so far this "
                                "session -- still caught, still not a crash.%s", key,
                                (unsigned long long)g_sites[i].count, latest);
            }
            return EXCEPTION_EXECUTE_HANDLER;   // already reported once
        }
    }
    if (g_siteCount < kMaxSites) {
        g_sites[g_siteCount++] = {key, 1};
    }
    // WHERE, spliced right after the site and before the verdict text, so it
    // survives if a long line is ever clipped and a reader meets it before the
    // prose. Built here and in the restated totals above and nowhere else: this
    // is the once-per-site path, so the VirtualQuery and the module lookup are
    // paid a handful of times a session, never once per fault.
    char where[kWhereCap];
    describeFault(code, where, sizeof(where));
    // SAY THE OUTCOME, NOT JUST THE EVENT.
    //
    // This line used to open with "FAULT exception=0xC0000005" and then talk
    // only about its own bookkeeping, so the one thing a reader wants to know
    // -- did this kill the game? -- was the one thing it did not say. Issue
    // #13 was filed as "CTD / Access Violation" on the strength of it: the
    // reporter pasted a run of these, every one absorbed exactly as designed,
    // and the scan they came from succeeded four lines later in the same log.
    // The real fault that session was a starved draw recogniser three modules
    // away, which this line's alarm drew attention away from rather than
    // towards.
    //
    // A reporter reads the first clause and stops, so the first clause is the
    // verdict now and the bookkeeping follows it. The word FAULT is kept
    // because logs are grepped for it and older reports quote it.
    //
    // TWO THINGS THIS LINE MUST NOT OVERCLAIM, both of which it did first time
    // and both of which are the same error it exists to correct -- a sentence
    // asserting something the code never checked.
    //
    // The first is WHOSE fault is routine. It was the nine camera_view sites,
    // which walked gigabytes of a live heap looking for an array and faulted
    // on pages the game had released, by design; that scanner was removed
    // 2026-09-29, so the line has no routine case left to describe. At
    // resubmit/copy, guardCrop/device or any of the D3D sites a fault means
    // something handed us a resource that was not what it claimed. Telling the
    // reader of one that the other is routine is how a real report gets
    // talked out of being filed.
    // The second is what a fault COSTS. For a read probe, abandoning it costs
    // the read. For a copy abandoned part-way, the destination holds whatever
    // got there first and is submitted anyway, so "the rest of that one
    // operation and nothing else" -- which this line used to promise -- is
    // more than the machinery guarantees. What is true everywhere is that the
    // operation was abandoned and the process carried on; the PR's own body
    // drew that line correctly and the log line now matches it.
    Log::get().note("FAULT ABSORBED exception=0x%08lX site=%s%s. THIS DID NOT CRASH "
                    "THE GAME. EDVR touched an address that was not there, its own "
                    "handler caught it, and the process carried on -- what the fault "
                    "cost is that one operation, abandoned. %sWhat would be worth "
                    "reporting is a total below that keeps doubling, or a "
                    "FEATURE-DISABLED line. Further faults at this site are counted "
                    "rather than logged; the running total is restated as it doubles, "
                    "so a hard exit cannot eat it.",
                    code, key, where,
                    "This site does NOT fault by design -- it is not one of the "
                    "memory probes -- so even a couple here are worth a report. ");
    return EXCEPTION_EXECUTE_HANDLER;
}

void reportFaultSites() {
    for (unsigned i = 0; i < g_siteCount; ++i) {
        if (g_sites[i].count > 1) {
            Log::get().note("FAULT TOTAL site=%s: %llu absorbed this session -- "
                            "all of them caught; none of them crashed the game.",
                            g_sites[i].site,
                            (unsigned long long)g_sites[i].count);
        }
    }
}

void FaultBudget::charge() {
    // fetch_sub returns the value BEFORE the decrement, so exactly one caller
    // sees 1 and logs the disable. The old non-atomic version could log twice,
    // or not at all, on overlapping faults.
    const int before = m_remaining.fetch_sub(1, std::memory_order_relaxed);
    if (before == 1) {
        Log::get().note("FEATURE-DISABLED %s exhausted its fault budget", m_name);
    } else if (before <= 0) {
        // Already exhausted; put it back so it cannot wrap after 2^31 faults.
        m_remaining.fetch_add(1, std::memory_order_relaxed);
    }
}

Sentinel::Sentinel(const wchar_t* dir, const wchar_t* name) {
    _snwprintf_s(m_path, _TRUNCATE, L"%s\\%s.armed", dir, name);
    m_tripped = GetFileAttributesW(m_path) != INVALID_FILE_ATTRIBUTES;
}

bool Sentinel::arm() {
    // The directory may not exist. It is created by Log::open(), which returns
    // early when log.enabled = 0 -- a documented setting -- so with logging off
    // the file was silently never written, trippedOnStartup() was false every
    // launch, and a genuinely crashing hook re-armed forever. The protection was
    // absent in exactly the configuration where there is no log to diagnose it
    // from.
    if (const wchar_t* slash = wcsrchr(m_path, L'\\')) {
        std::wstring dir(m_path, static_cast<size_t>(slash - m_path));
        CreateDirectoryW(dir.c_str(), nullptr);
    }

    HANDLE f = CreateFileW(m_path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        // m_armed stays false: nothing to delete later, and confirm() must not
        // report success for a file that was never created.
        breadcrumb("guard: sentinel could NOT be armed; crash protection is off");
        return false;
    }
    const char msg[] = "edvr: a hook was armed and never confirmed. "
                       "Delete this file to re-enable it.\r\n";
    DWORD written = 0;
    WriteFile(f, msg, sizeof(msg) - 1, &written, nullptr);
    FlushFileBuffers(f);
    CloseHandle(f);
    m_armed = true;
    return true;
}

void Sentinel::confirm() {
    if (!m_armed) return;
    DeleteFileW(m_path);
    m_armed = false;
}

void Sentinel::clearTrip() {
    // A trip costs ONE session, not every future one.
    //
    // confirm() runs on validation, on commit failure, and from the hook's
    // shutdown -- and that shutdown only happens on FreeLibrary, which a game
    // closing never does. So a session that ended cleanly before the hook could
    // validate (SteamVR restarting, the headset going to standby, the player
    // quitting from the menu) left the file behind, and every launch after it
    // refused to install and told the user it had very likely crashed. Nothing
    // had. The only way out was deleting a file nobody knew about.
    //
    // Deleting it on the refusing launch keeps the protection -- a hook that
    // really does crash still gets thrown out every other launch, and says so
    // -- while costing a false trip one session instead of all of them.
    if (!DeleteFileW(m_path) && GetFileAttributesW(m_path) != INVALID_FILE_ATTRIBUTES) {
        // Still there. Left unsaid, this is a permanent lockout wearing a
        // one-session message: the file cannot be removed (read-only, or held by
        // something), so every launch refuses and every launch promises to try
        // again next time.
        Log::get().note("the crash sentinel file could not be deleted, so this will "
                        "keep refusing every launch. Delete %S by hand, or set "
                        "ignore_sentinel = 1 under [advanced] to start anyway.",
                        m_path);
        return;
    }
    m_tripped = false;
}

}  // namespace edvr
