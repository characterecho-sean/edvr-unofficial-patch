// Held numeric edits in the F8 menu, coalesced before the settings file is written and re-read (the review of 2026-10-09, item 2; measured by the config refresh
// window line of F16: a five-second hold on a numeric row was 100 refreshes, 93 ms of the frame thread, 1.17 ms at the worst).
//
// Holding Left or Right on a Number row delivers a step every 83 ms after a 400 ms delay (menu_keys.h). Every step used to queue its own write, and every landed write
// asked for a config refresh: the file parsed again and every module reconfigured on the frame thread, about twelve times a second. What the player sees does not depend
// on that: the row's value is shown the moment the step is made. What depends on it is the live EFFECT of the setting and the file, and those only need the VALUE the
// burst of steps ended on, so a burst of steps is written together:
//
//   - a step is HELD: the caller shows its value at once, and the write waits here;
//   - the held edit is WRITTEN (handed to the sink) when
//       Release     no key that steps a row is down any more (a tap writes here, a frame after the key came up),
//       MaxWait     the burst has been going for kEditMaxWaitMs (250 ms, Sean's choice 2026-10-09): a hold writes and reloads about four times a second, a preview that moves
//                   in quarter-second steps, a third of the old cost. It is also what writes a key held with no step coming (a row at its limit). There is no separate
//                   still time: a burst is written within the bound of its first step whether or not steps keep coming, and a pause long enough to be told from the
//                   83 ms repeat (the first design's 500 ms) is longer than the bound, so it could never come first,
//       RowSwitch   another row is highlighted or stepped,
//       PageSwitch  another page is shown,
//       Other       any other change is about to be queued (it goes behind the held one, the order the player made them in),
//       Close       the menu closes (Escape, the summon key, focus lost, the tracking origin changed),
//       Shutdown    the DLL is shutting down.
//   - the sink is the writer thread's queue, so a failed write is rolled back exactly as it was: the row is put back to what the file holds -- unless the row has been
//     edited since (EditGeneration below): the newer edit is already shown, and it is held or queued or written, so its own result decides what the row shows.
//
// Pure: the job is whatever the caller writes (menu.cpp's WriteJob; the rig's own), and the clock and the key state are arguments.
#pragma once
#include <cstdint>

namespace edvr {

constexpr uint64_t kEditMaxWaitMs = 250;   // the longest a held edit waits for its write: a hold writes about every 250 ms, and once more on release

enum class EditFlush : uint8_t { None = 0, Release, MaxWait, RowSwitch, PageSwitch, Other, Close, Shutdown };

// The edit counter of one row (the 0.19.0 release review, "existing limitation"). Every edit of the row takes the next number and its job carries it. A write that fails
// puts the row back to what the file holds only when its job was the row's LATEST edit: with a newer edit held in the coalescer or queued behind it, the row already shows
// the newer value, and putting it back to the file's would drop the steps that were made since -- the next repeat of a held key would step from the file's value, not
// from the one on the screen (a hold of 1, 2, 3, 4 written, 5 held, the write of 4 failing: the row went to 0 and the next step to 1, not 6). The newer edit's own result
// then decides: written, the file and the row agree; failed, it is the latest and rolls the row back itself.
struct EditGeneration {
    uint32_t latest = 0;
    uint32_t next() { return ++latest; }                                              // a new edit of the row: the number its job carries
    bool rollbackDue(uint32_t jobGeneration) const { return jobGeneration == latest; }   // a failed write rolls the row back only if no newer edit was made
};

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
    }

    // Once per menu tick, before the keys are read: `held` is whether a key that steps a row is down, `highlightDef` the row highlighted now (-1: none), `page` the page
    // shown. Writes the held edit when it is due and says why (None: it was not due, or there was none).
    EditFlush tick(uint64_t now, bool held, int highlightDef, int page) {
        if (!m_pending) return EditFlush::None;
        EditFlush why = EditFlush::None;
        if (m_def != highlightDef) why = EditFlush::RowSwitch;
        else if (m_page != page) why = EditFlush::PageSwitch;
        else if (!held) why = EditFlush::Release;
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
    uint64_t m_firstMs = 0;
    uint32_t m_flushes = 0;
    bool m_pending = false;
};

}  // namespace edvr
