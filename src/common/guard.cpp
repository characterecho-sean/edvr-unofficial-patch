// GENERATED from src/common/guard.cpp in the private edvr repo -- do not edit here.
// Edit there, then: python tools/sync_common.py --write   [body-sha256 f97fba872af534d2]
#include "guard.h"

#include <windows.h>

#include <cstdio>

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
                Log::get().note("FAULT TOTAL site=%s: %llu absorbed so far this "
                                "session.", key,
                                (unsigned long long)g_sites[i].count);
            }
            return EXCEPTION_EXECUTE_HANDLER;   // already reported once
        }
    }
    if (g_siteCount < kMaxSites) {
        g_sites[g_siteCount++] = {key, 1};
    }
    Log::get().note("FAULT exception=0x%08lX site=%s. Further faults at this "
                    "site are counted rather than logged; the running total is "
                    "restated as it doubles, so a hard exit cannot eat it.",
                    code, key);
    return EXCEPTION_EXECUTE_HANDLER;
}

void reportFaultSites() {
    for (unsigned i = 0; i < g_siteCount; ++i) {
        if (g_sites[i].count > 1) {
            Log::get().note("FAULT TOTAL site=%s: %llu absorbed this session.",
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
