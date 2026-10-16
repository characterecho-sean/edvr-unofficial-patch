#pragma once
// The F8 menu's addon settings: the rows an addon registers, and the queue through which every change to them
// reaches the menu thread. Kept out of menu.cpp so the rig that drives the plugin manager can run the real thing.
//
// Threads. Rows are registered, and retired after a fault, on whatever thread the plugin manager runs (Elite's
// Present thread, the OpenXR owner thread). The menu thread reads rows while it draws and while it builds pages,
// holding Page and Entry values that index into rows() and borrow heading text. So the other threads never erase a
// row or rebuild a page. They mark it (retire(): its callbacks go null at once, under mutex(), so nothing calls into a
// DLL that is about to go) and post a request; the menu thread, at the top of its tick, drains the requests, erases
// the retired rows and rebuilds its pages. Row storage and headings therefore outlive every use of them.
//
// Lock order: PluginManager::m_mutex, then mutex() (this file), then the private queue mutex. The queue mutex is a
// leaf and is never held across a call out of this file.

#include "../../include/edvr_plugin_api.h"

#include <cstdint>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace edvr::plugins {

struct PluginSettingRow {
    std::string owner;   // the addon (its stem) whose DLL holds getter, setter and userData; "" for none
    std::string header;
    std::string key;
    std::string label;
    std::string hint;
    EdvrPluginSettingType type = EDVR_PLUGIN_SETTING_BOOL;
    int64_t minValue = 0;
    int64_t maxValue = 1;
    int64_t stepValue = 1;
    int64_t defaultValue = 0;
    std::vector<std::string> choiceOptions;
    EdvrPluginSettingGetter getter = nullptr;
    EdvrPluginSettingSetter setter = nullptr;
    void* userData = nullptr;
    int64_t cachedValue = 0;
    bool retired = false;   // its owner is gone or faulted: inert (no callbacks), awaiting erase on the menu thread
};

class PluginRowRegistry {
public:
    // Guards rows(). Taken by the menu thread while it walks or calls into a row, and by anything that changes one.
    std::recursive_mutex& mutex() { return m_rowsMutex; }
    // The caller holds mutex().
    std::vector<PluginSettingRow>& rows() { return m_rows; }

    // Adds a row for (owner, header, key), or refreshes the owner's own live row with that identity. A row of
    // another owner with the same header and key is a different row, as is its saved value (pluginSettingIniKey). The caller holds mutex().
    PluginSettingRow& upsertLocked(const std::string& owner, const std::string& header, const std::string& key) {
        for (auto& existing : m_rows) {
            if (!existing.retired && existing.owner == owner && existing.header == header && existing.key == key) return existing;
        }
        m_rows.emplace_back();
        PluginSettingRow& row = m_rows.back();
        row.owner = owner;
        row.header = header;
        row.key = key;
        return row;
    }

    // Marks every live row of the owner retired and inert, then asks the menu thread to erase them. True when
    // there was anything to retire. Safe from any thread; touches no Page or Entry.
    bool retire(const std::string& owner) {
        bool any = false;
        {
            std::lock_guard<std::recursive_mutex> lock(m_rowsMutex);
            for (auto& r : m_rows) {
                if (r.owner != owner || r.retired) continue;
                r.retired = true;
                r.getter = nullptr;
                r.setter = nullptr;
                r.userData = nullptr;
                any = true;
            }
        }
        if (any) post(Op{ Op::Erase, owner, false });
        return any;
    }

    // Rows were added: the menu thread rebuilds its pages at its next tick.
    void postChanged() { post(Op{ Op::Changed, std::string(), false }); }
    // Every addon's rows (and the pending-approval list) are hidden, or shown again. Nothing is deleted.
    void postHidden(bool hidden) { post(Op{ Op::Hidden, std::string(), hidden }); }

    // Menu thread only. Applies the posted requests: erases retired rows, flips hidden(). True when the pages must
    // be rebuilt, which the caller does AFTER this returns (the rebuild takes the plugin manager's lock first).
    bool drain() {
        std::vector<Op> ops;
        {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            ops.swap(m_ops);
        }
        bool rebuild = false;
        for (const Op& op : ops) {
            if (op.kind == Op::Erase) {
                std::lock_guard<std::recursive_mutex> lock(m_rowsMutex);
                for (size_t i = m_rows.size(); i-- > 0;) {
                    if (m_rows[i].retired && m_rows[i].owner == op.owner) m_rows.erase(m_rows.begin() + static_cast<ptrdiff_t>(i));
                }
                rebuild = true;
            } else if (op.kind == Op::Hidden) {
                if (m_hidden != op.hidden) rebuild = true;
                m_hidden = op.hidden;
            } else {
                rebuild = true;
            }
        }
        return rebuild;
    }

    // Menu thread only (drain() writes it).
    bool hidden() const { return m_hidden; }

    // Text for a page heading that stays valid for the process: rows move when the vector grows, and a heading that
    // pointed into one would dangle before the next rebuild. The caller holds mutex().
    const char* internHeading(const std::string& text) { return m_headings.insert(text).first->c_str(); }

private:
    struct Op {
        enum Kind { Erase, Changed, Hidden } kind;
        std::string owner;
        bool hidden;
    };
    void post(Op op) {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        m_ops.push_back(std::move(op));
    }

    std::recursive_mutex m_rowsMutex;
    std::vector<PluginSettingRow> m_rows;
    std::set<std::string> m_headings;
    std::mutex m_queueMutex;
    std::vector<Op> m_ops;
    bool m_hidden = false;
};

} // namespace edvr::plugins
