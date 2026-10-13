// The mode watch (docs\design-unified-install-2026-10-12.md sections 3.2 and 3.3): Elite's Settings.xml is read when it is written, and
// a confirmed change to the 3D mode is posted to the render thread, which at the frame boundary switches EDVR's profile live. mode_watch_logic.h
// holds the rules; mode_watch.cpp holds the worker, the log lines and the commit.
#pragma once

#include <cstddef>
#include <cstdint>

namespace edvr {

// Hints ask the worker to re-read now. Each is a bit, so several can be pending at once.
enum class ModeHint : uint32_t { Setter = 1, Window = 2, LiveRise = 4 };

// Starts the worker once, after the launch's profile log lines. A no-op when the profile is Invalid.
void modeWatchStart();

// Any thread; cheap. Safe before modeWatchStart: the bit is kept and the worker reads it at its first wake.
void modeWatchHint(ModeHint h);

// The render thread, at the top of presentFrameBoundary after the frame counter moves. Commits a posted mode and samples the runtime's
// live flag once a frame.
void modeWatchFrameBoundary();

// The menu's toast poll: true once per commit, with the notice text copied to out.
bool modeWatchNoticeDue(char* out, size_t size);

}  // namespace edvr
