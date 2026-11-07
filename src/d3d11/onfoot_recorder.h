// F213: the on-foot flight recorder, the graphics half (the format, the ring and the segment logic are src\common\onfoot_recorder_core.h's).
//
// ACTIVE is log.on_foot_detail on AND native stereo on foot: the frame thread decides it once a frame (frame()) and the producers read it with one relaxed load
// (active()). Off, a site costs that load and a branch; nothing is allocated until the first activation, which allocates the ring (about 3 MB) and starts the writer.
//
// THE WRITER is a detached std::thread that drains the ring every 100 ms into edvr_logs\onfoot\ and flushes, so a game crash leaves a readable file with at most one
// drain lost (the OS closes the handle). Its lifetime is the journal worker's (journal_watch.cpp startWorker): a stop flag and a wake event, a session that is never
// freed so a thread still winding down cannot touch freed memory, and the graphics DLL is pinned (module_pin.h) well before the first on-foot frame. shutdown() is the
// explicit stop (nativeOnFootShutdown): it sets the flag and the event and returns, so it is safe under the loader lock; the writer does its final drain and closes the
// file itself. Normal process exit skips shutdown() and loses nothing but the last drain.
#pragma once

#include <cstdint>
#include <string>

#include "../common/onfoot_recorder_core.h"

namespace edvr {
namespace onfootrec {

// The producers' gate. The frame thread stores it (frame()); any thread loads it.
inline std::atomic<bool> g_active{false};
inline bool active() noexcept { return g_active.load(std::memory_order_relaxed); }

// QPC ticks now, and the tick rate.
uint64_t qpcNow() noexcept;
uint64_t qpcFreq() noexcept;

// Any thread. Stamps the QPC and pushes; false when inactive or when the ring is full (counted). The caller built the record with initRecord.
bool pushRecord(unsigned id, void* rec, uint32_t size) noexcept;
template <class R> inline bool push(R& rec) noexcept { return pushRecord(Traits<R>::id, &rec, static_cast<uint32_t>(sizeof(R))); }

// The Animate thread: the QPC ticks one ik record cost its producer (the joint walks included).
void noteIkCost(uint64_t ticks) noexcept;

// The frame thread, once a frame: wantActive is log.on_foot_detail and native on foot; keysJson/keysGeneration are the on-foot keys in force for the segment header
// (a JSON object; copied when the generation moves). Opens the session on the first activation, and prints the window line.
void frame(bool wantActive, uint64_t nowMs, const std::string& keysJson, uint32_t keysGeneration);

// hotkey.on_foot_mark: counts, records when active, and always says so in the log. The device_hook frame thread.
void mark();

// nativeOnFootShutdown: stop the writer (see above). Safe to call twice and when nothing ever started.
void shutdown() noexcept;

}  // namespace onfootrec
}  // namespace edvr

// The runtime's way in (src\openxr): one fully built rt_eye or rt_recentre record, its QPC stamped by the caller. False when inactive, malformed or dropped. Optional in the
// runtime: a graphics half without this export simply records nothing.
extern "C" bool __stdcall edvrOnFootRecordPush(const void* record, uint32_t size);
