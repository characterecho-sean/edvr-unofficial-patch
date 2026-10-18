// The F8 writer's jobs and completion path, shared with the device-free menu rig.
#pragma once
#include <cstdint>
#include <string>

namespace edvr {

enum class WriteDestination : uint8_t { BuiltinRow, PluginSetting, AddonApproval };

struct WriteJob {
    WriteDestination destination = WriteDestination::BuiltinRow;
    int         def = -1;
    std::string dotted;
    std::string value;
    std::string before;
    // Per-headset rows only: the worn headset's key and the values either
    // side, so the log and the last-write line name what changed rather than
    // printing two lists; `dropped` names malformed tokens the merge left out
    // of the list.
    std::string headset;
    std::string fromValue;
    std::string toValue;
    std::string dropped;
    uint32_t    gen = 0;   // EditGeneration: rollback only if no newer row edit exists
};
struct WriteDone {
    WriteJob    job;
    bool        ok;
    std::string err;
};

// The worker supplies the guarded INI writer; tests supply successful or failed persistence.
// Every attempted write produces a completion, including a contained writer fault.
template<class Persist>
WriteDone runMenuWrite(const WriteJob& job, Persist persist) {
    WriteDone done{job, false, {}};
    done.ok = persist(job, &done.err);
    if (!done.ok && done.err.empty()) done.err = "the write faulted";
    return done;
}

// Array references are used only after the explicit destination and bounds checks.
// Dynamic effects have already been applied by their caller; failed persistence
// leaves them active for this session without assuming persistence succeeded.
template<class State, class Definitions, class Rows, class ValueOf, class PendingOf, class Note>
void completeMenuWrite(const WriteDone& w, State& s, const Definitions& kMenuRows,
                       Rows& g_rows, int rowCount, ValueOf rowValue, PendingOf rowPending,
                       Note note, const char* iniName) {
    if (w.job.destination != WriteDestination::BuiltinRow) {
        const bool approval = w.job.destination == WriteDestination::AddonApproval;
        if (w.ok) {
            s.pollRequest = true;
            s.lastWrite = w.job.dotted + " = " + w.job.value;
            note("menu: %s = %s (written to %s; active this session).",
                 w.job.dotted.c_str(), w.job.value.c_str(), iniName);
        } else {
            // Both dynamic callers already applied the effect. An approval may have run
            // arbitrary addon initialisation, so undoing it here is not a safe rollback.
            // Keep the session state; a contained fault after replacement may leave the
            // file updated even though the worker could not confirm completion.
            s.lastWrite = "FAILED: " + w.job.dotted + ": " + w.err +
                (approval ? "; active this session; save not confirmed; approval may be needed next launch"
                          : "; active this session; save not confirmed");
            note("menu: save of %s = %s failed: %s (%s).",
                 w.job.dotted.c_str(), w.job.value.c_str(), w.err.c_str(),
                 approval ? "active this session; save not confirmed; approval may be needed next launch"
                          : "active this session; save not confirmed");
        }
        s.contentDirty = true;
        return;
    }
    if (w.job.def < 0 || w.job.def >= rowCount) {
        s.lastWrite = "FAILED: invalid built-in setting row for " + w.job.dotted;
        note("menu: invalid built-in setting row %d for %s.", w.job.def, w.job.dotted.c_str());
        s.contentDirty = true;
        return;
    }
    const auto& d = kMenuRows[w.job.def];
    if (w.ok) {
        s.pollRequest = true;
        const char* when = d.applies == 2 ? "takes effect at the next launch"
                         : d.applies == 1 ? "live"
                                          : "when it applies is not documented";
        if (!w.job.headset.empty()) {
            // The per-headset row: the key and the values, then the
            // list the file now holds.
            s.lastWrite = w.job.headset + " = " + w.job.toValue;
            note("menu: %s %s %s -> %s (list now %s%s%s; written to %s; "
                            "%s).",
                            w.job.dotted.c_str(), w.job.headset.c_str(), w.job.fromValue.c_str(),
                            w.job.toValue.c_str(), w.job.value.empty() ? "empty" : w.job.value.c_str(),
                            w.job.dropped.empty() ? "" : "; dropped malformed ",
                            w.job.dropped.c_str(), iniName, when);
        } else {
            s.lastWrite = w.job.dotted + " = " + w.job.value;
            note("menu: %s %s -> %s (written to %s; %s).", w.job.dotted.c_str(),
                            w.job.before.empty() ? "(default)" : w.job.before.c_str(),
                            w.job.value.c_str(), iniName, when);
        }
    } else {
        s.lastWrite = "FAILED: " + w.err;
        note("menu: %s = %s could not be written: %s.", w.job.dotted.c_str(),
                        w.job.value.c_str(), w.err.c_str());
        // The row goes back to what the file holds only if this was its latest edit. A newer one (held, queued, or written already) is what the row shows now, and
        // its own result decides: putting the file's value back here would drop the steps made since (menu_edit_hold.h, EditGeneration).
        if (g_rows[w.job.def].edits.rollbackDue(w.job.gen)) {
            g_rows[w.job.def].value = rowValue(d);
            if (d.applies == 2) {
                g_rows[w.job.def].pending = rowPending(d, g_rows[w.job.def].value,
                                                       g_rows[w.job.def].snapshot);
            }
        }
    }
    s.contentDirty = true;
}

}  // namespace edvr
