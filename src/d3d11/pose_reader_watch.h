#pragma once
// advanced.eye_origin_readers (docs/design-transition-flash-engine-fix-
// 2026-09-23.md, parts A2/B). Flight 195435 found that the eye origin is
// written by the SAME code path on bad and good frames -- what is missing
// is the cockpit eye point the head pose gets added to. This module hunts
// for the game code that adds it, two ways behind one key:
//
//   A2  a hardware data breakpoint (DR0) on the exact float the runtime's
//       WaitGetPoses/GetLastPoses handed back (the render-pose pointer
//       openvr_api.dll publishes every call, frame_flag.h), armed once
//       that address has held steady for 60 frames and is not on the
//       calling thread's own stack. Every hit inside EliteDangerous64.exe
//       is unwound and deduped into a small table: WHO reads the pose.
//   B   read-only CodeHooks on the game's per-frame camera-positioner Tick
//       and its swap-sync routine, build- and prologue-keyed like
//       transition_flash_prevent.cpp (each stands down on its own if
//       either has moved). A swap is the cached positioner (this+0x2A0)
//       changing across a call.
//
// Shares advanced.eye_origin_trace's dump (eye_origin_trace.h,
// glitch_frame.cpp) rather than writing its own: when this key is on, the
// trace's own stack capture runs too, so the two instruments' dumps
// describe one timeline. See pose_reader_watch_core.h for the pure logic
// (Dr7 composition, stack-range classification, the stability gate, the
// reader table's dedupe, the dump-trigger filter) that
// tools\transition_flash_prevent_test drives without the game.
#include <cstdint>

#include "pose_reader_watch_core.h"

namespace edvr {

class Config;

// Read on every config reload (vscreen.cpp, both call sites, beside
// transitionFlashPreventConfigure). Off: one log line, nothing installed,
// and the runtime's pose-trace request flag stays clear. The first call
// that sees "on" installs the two positioner CodeHooks and sets the
// runtime's request flag; later calls only move the live on/off bit -- the
// hardware breakpoint itself arms, re-arms and disarms on its own schedule
// from poseReaderWatchFrameBoundary, not from here.
void poseReaderWatchConfigure(Config& cfg);

// Once a frame, from vscreen.cpp beside glitchFrameBoundary()/
// transitionFlashPreventFrameBoundary(). Publishes the frame number the
// positioner hooks (which run on whatever thread calls Tick/swap-sync, not
// necessarily this one) read, runs the 60-frame stability gate against the
// runtime's published render-pose pointer, arms or sweeps for newly
// created threads, and services the 30s/5000-hit bound. Never call this
// from inside a game hook.
void poseReaderWatchFrameBoundary(uint32_t frameNo);

// Live on/off, for glitch_frame.cpp's eyeOriginTraceBoundary: while this
// instrument is on, its automatic dump narrows to the scene-judged
// eye-camera-reset verdict and a positioner swap (pose_reader_watch_
// core.h's dumpVerdictTrigger), not eye_origin_trace's own broader
// withheld-class trigger.
bool poseReaderWatchOn();

// A positioner swap (this+0x2A0 changing across a Tick or swap-sync call)
// not yet folded into a dump trigger. Takes it: a second call before the
// next swap reports false. `*frameOut` is set only when this returns true.
bool poseReaderTakeSwapTrigger(uint32_t* frameOut);

// This frame's reader mask and positioner-call counts, read once per ring-
// write site (glitch_frame.cpp's RingEntry) -- cullGuardStatePacked()'s
// pattern: a cheap snapshot of what happened during the frame that just
// closed, taken at the boundary and reset there for the next one.
struct PoseReaderFrameSnapshot {
    uint32_t readerMask = 0;      // bit N: unique-reader table id N fired this frame
    uint16_t tickCalls = 0;
    uint16_t swapSyncCalls = 0;
    bool     swapDetected = false;
};
PoseReaderFrameSnapshot poseReaderWatchFrameSnapshot();

// The unique-reader table, dump time only (glitch_frame.cpp's
// eyeOriginTracePerformDump). `index` is the table's own row number, 0..
// poseReaderWatchTableCount()-1 -- eot::StackTable's convention, the id IS
// the index. unwindRvas holds up to prw::kMaxUnwindFrames game-module
// RVAs, the immediate caller first; unwindCount says how many are valid.
// Capacity is pose_reader_watch_core.h's prw::kMaxReaders/kMaxUnwindFrames
// (the same constants the dedupe logic and its tests use), not a second
// copy of those numbers.
struct PoseReaderTableEntry {
    uint64_t rip = 0;          // game-module RVA of the instruction after the access
    uint32_t count = 0;
    uint32_t firstFrame = 0;
    uint32_t lastFrame = 0;
    uint32_t lastThreadId = 0;
    uint32_t unwindRvas[prw::kMaxUnwindFrames] = {};
    uint32_t unwindCount = 0;
};
uint32_t poseReaderWatchTableCount();
PoseReaderTableEntry poseReaderWatchTableEntry(uint32_t index);

// Final session summary. Mirrors transitionFlashPreventShutdown's call
// site (device_hook.cpp).
void poseReaderWatchShutdown();

}  // namespace edvr
