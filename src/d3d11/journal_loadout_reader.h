// F140: the journal worker's loadout reader. Header only (journal_watch.cpp includes it, so the three builds that link journal_watch.cpp need nothing new). The parsing is
// journal_loadout.h's, which native_frame_test runs.
//
// How it reads: journal_watch.cpp tails the journal slice by slice, from the top when the file is provably this session's and from the END when it is not, and keeps only
// event names. The loadout is state, not an event: EDVR may start mid-session, with the last SuitLoadout or LoadoutEquipModule long written. So this reader opens the CURRENT
// journal file (the one journal_watch.cpp tails) on its own handle and reads it from its first byte whatever the provenance, up to kMaxPerPoll a poll until it has caught up
// (a long session's journal is a few MB; the worker is its own thread), then follows it. It keeps the three weapon slots (module name, scope or not, the mods) and nothing else
// of what it reads. A new journal file is read from its top and updates the loadout it finds: the loadout is the commander's own, it carries across a relaunch.
// F147: the worker polls it only in the VR profile and only after native on foot has first been active (journalWatchSetLoadoutWanted), so a flat session and a VR session that
// never goes on foot never open this file; the first poll after that is the one whole read, every later one only the appended bytes. The log line names the loadout by its
// id and the weapons with their scope state, not by the commander's name for it.
#pragma once

#include <windows.h>

#include <memory>
#include <new>
#include <string>

#include "../common/log.h"
#include "journal_loadout.h"

namespace edvr {
namespace loadout {

class Reader {
  public:
    static constexpr DWORD kChunk = 256 * 1024;
    static constexpr uint64_t kMaxPerPoll = 8ull * 1024 * 1024;
    // One poll (the worker's, every 500 ms): bring the loadout up to date from the journal file dir\file.
    void poll(const std::wstring& dir, const std::wstring& file) noexcept {
        if (file.empty()) return;
        if (file != file_) {
            closeHandle();
            file_ = file;
            offset_ = 0;
            feeder_.reset();
            failures_ = 0;
            retryAt_ = 0;
            filesRead_ += 1;
        }
        if (h_ == INVALID_HANDLE_VALUE) {
            const uint64_t now = GetTickCount64();
            if (failures_ && now < retryAt_) return;
            h_ = CreateFileW((dir + L"\\" + file_).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                             nullptr);
            if (h_ == INVALID_HANDLE_VALUE) {
                ++failures_;
                retryAt_ = now + 4000;
                if (failures_ == 1) Log::get().note("journal loadout (F140): could not open %S (error %lu); trying again every 4 s", file_.c_str(), GetLastError());
                return;
            }
            failures_ = 0;
        }
        if (!chunk_) chunk_.reset(new (std::nothrow) char[kChunk]);
        if (!chunk_) return;
        uint64_t total = 0;
        bool changed = false;
        while (total < kMaxPerPoll) {
            LARGE_INTEGER pos;
            pos.QuadPart = static_cast<LONGLONG>(offset_);
            DWORD got = 0;
            if (!SetFilePointerEx(h_, pos, nullptr, FILE_BEGIN) || !ReadFile(h_, chunk_.get(), kChunk, &got, nullptr)) {
                closeHandle();   // the next poll opens it again at the same offset
                ++failures_;
                retryAt_ = GetTickCount64() + 4000;
                break;
            }
            if (!got) break;
            offset_ += got;
            total += got;
            feeder_.feed(chunk_.get(), got, [&](const char* line, size_t n) {
                const LineResult r = applyLine(state_, line, n);
                if (r.changed) changed = true;
            });
            if (got < kChunk) break;
        }
        if (changed) {
            g_published.publish(state_);
            char text[640];
            describe(state_, text, sizeof(text));
            Log::get().note("journal loadout (F140): %s (read from %S, %llu bytes in)", text, file_.c_str(), static_cast<unsigned long long>(offset_));
        }
    }
    // The worker's exit, or the journal file being dropped by the watcher: the handle goes; the next poll starts the file again from its top.
    void close() noexcept {
        closeHandle();
        file_.clear();
        offset_ = 0;
        feeder_.reset();
    }

  private:
    void closeHandle() noexcept {
        if (h_ != INVALID_HANDLE_VALUE) {
            CloseHandle(h_);
            h_ = INVALID_HANDLE_VALUE;
        }
    }
    std::wstring file_;
    HANDLE h_ = INVALID_HANDLE_VALUE;
    uint64_t offset_ = 0, retryAt_ = 0;
    uint32_t failures_ = 0, filesRead_ = 0;
    Loadout state_;
    LineFeeder feeder_;
    std::unique_ptr<char[]> chunk_;
};

}  // namespace loadout
}  // namespace edvr
