// Held numeric edits in the F8 menu, coalesced before the settings file is written and re-read (the review of 2026-10-09, item 2; measured by the config refresh
// window line of F16: a five-second hold on a numeric row was 100 refreshes, 93 ms of the frame thread, 1.17 ms at the worst).
//
// Holding Left or Right on a Number row delivers a step every 83 ms after a 400 ms delay (menu_keys.h). Every step used to queue its own write, and every landed write
// asked for a config refresh: the file parsed again and every module reconfigured on the frame thread, about twelve times a second. What the player sees does not depend
// on that: the row's value is shown the moment the step is made. What depends on it is the live EFFECT of the setting and the file, and those only need the VALUE the
// burst of steps ended on, so a burst is one write:
//
//   - a step is HELD: the caller shows its value at once, and the write waits here;
//   - the held edit is WRITTEN (handed to the sink) when
//       Release     no key that steps a row is down any more (a tap writes here, a frame after the key came up),
//       Still       the key is down but no step has come for kEditStillMs (a row at its limit),
//       MaxWait     the burst has been going for kEditMaxWaitMs: a long hold is not silent for ever, its live effect shows up about every two seconds,
//       RowSwitch   another row is highlighted or stepped,
//       PageSwitch  another page is shown,
//       Other       any other change is about to be queued (it goes behind the held one, the order the player made them in),
//       Close       the menu closes (Escape, the summon key, focus lost, the tracking origin changed),
//       Shutdown    the DLL is shutting down.
//   - the sink is the writer thread's queue, so a failed write is rolled back exactly as it was: the row is put back to what the file holds.
//
// Pure: the job is whatever the caller writes (menu.cpp's WriteJob; the rig's own), and the clock and the key state are arguments.
#pragma once
#include <cstdint>

namespace edvr {

constexpr uint64_t kEditStillMs = 500;      // longer than the first repeat (400 ms): the gap after the first step is not a pause
constexpr uint64_t kEditMaxWaitMs = 2000;   // the longest a held edit waits for its write

enum class EditFlush : uint8_t { None = 0, Release, Still, MaxWait, RowSwitch, PageSwitch, Other, Close, Shutdown };

template <class Job>
class EditCoalescer {
public:
    using Sink = void (*)(void* ctx, const Job& job, EditFlush why);
    using Merge = void (*)(Job& pending, const Job& latest);

    EditCoalescer(Sink sink, void* ctx, Merge merge) : m_sink(sink), m_ctx(ctx), m_merge(merge) {}

    // A step of row `def`, on page `page`, made at `now` (the caller has shown its value). A pending edit of another row is written first.
    void hold(const Job& job, int def, int page, uint64_t now) {
        if (m_pending && m_def != def) flush(EditFlush::RowSwitch);
        if (!m_pending) {
            m_job = job;
            m_def = def;
            m_page = page;
            m_firstMs = now;
            m_pending = true;
        } else {
            m_merge(m_job, job);
        }
        m_lastMs = now;
    }

    // Once per menu tick, before the keys are read: `held` is whether a key that steps a row is down, `highlightDef` the row highlighted now (-1: none), `page` the page
    // shown. Writes the held edit when it is due and says why (None: it was not due, or there was none).
    EditFlush tick(uint64_t now, bool held, int highlightDef, int page) {
        if (!m_pending) return EditFlush::None;
        EditFlush why = EditFlush::None;
        if (m_def != highlightDef) why = EditFlush::RowSwitch;
        else if (m_page != page) why = EditFlush::PageSwitch;
        else if (!held) why = EditFlush::Release;
        else if (now - m_lastMs >= kEditStillMs) why = EditFlush::Still;
        else if (now - m_firstMs >= kEditMaxWaitMs) why = EditFlush::MaxWait;
        if (why != EditFlush::None) flush(why);
        return why;
    }

    // Write the held edit now, whatever the clock says. True when there was one.
    bool flush(EditFlush why) {
        if (!m_pending) return false;
        const Job job = m_job;
        m_pending = false;   // before the sink: a sink that comes back here finds nothing to write twice
        ++m_flushes;
        m_sink(m_ctx, job, why);
        return true;
    }

    bool pending() const { return m_pending; }
    int def() const { return m_def; }
    uint32_t flushes() const { return m_flushes; }   // held edits written so far

private:
    Sink m_sink;
    void* m_ctx;
    Merge m_merge;
    Job m_job{};
    int m_def = -1, m_page = -1;
    uint64_t m_firstMs = 0, m_lastMs = 0;
    uint32_t m_flushes = 0;
    bool m_pending = false;
};

}  // namespace edvr
