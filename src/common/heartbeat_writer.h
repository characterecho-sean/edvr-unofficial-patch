// The breadcrumb heartbeat's file write, off the render thread (docs/freeze-diagnostics-2026-10-01.md, commit 3).
//
// breadcrumbHeartbeat (proxy.cpp) writes one line to edvr_breadcrumbs.txt every log.breadcrumb_heartbeat_seconds, so
// that a session that dies leaves a trail that says how long it lived and whether frames were still arriving. It
// did that with CreateFile / WriteFile / CloseHandle ON THE RENDER THREAD. A file create in the game folder goes
// through whatever filter driver is watching it; on a slow or filtered disk that is a hitch in a frame, every thirty
// seconds, from a feature whose whole job is to be harmless. In the issue 63 flight a worker thread's file read
// stalled for 2.3 s at the same moment as the worst freeze. This moves the write to a small thread of its own.
//
// WHAT THE RENDER THREAD DOES NOW: post(). Three stores into a record, one event set, nothing else. It never writes
// a file, never waits, never takes a lock another thread can hold: it is the record's only writer, and the writer
// thread reads it through a version counter (a seqlock), so a reader that is descheduled in the middle cannot make
// the poster wait.
//
// WHAT THE BREADCRUMB FILE MEANS, AND HOW THAT IS KEPT. The last line before a crash or a kill must still be
// meaningful, and these are the properties that make it so:
//   1. The writer thread writes ONLY what the render thread posted. It has no clock of its own and never writes a
//      heartbeat by itself. If the render thread hangs it posts nothing and the heartbeat stops, which is what a
//      hang looks like in the trail; a writer on a timer would have kept saying "alive" through the hang.
//   2. A line carries the frame number and uptime of the moment it was POSTED, not of the moment it was written.
//   3. Posts are coalesced, newest wins: a writer held up by a slow disk writes the latest post when it comes back,
//      never an old one after a newer one, so the frame numbers in the file never go backwards.
//   4. After a crash has been noticed (the unhandled-exception filter), at process exit, or at a FreeLibrary
//      unload, NO heartbeat is written after the closing line: close() drops whatever is pending and a heartbeat
//      that has not started never starts. closeAndDrain also waits (a bounded time) for one that already has, so
//      the crash filter's lines are the last in the file and not a heartbeat that landed behind them.
//
// Header only and parameterised on the sink, so tools\heartbeat_writer_test can hold every property above with a sink
// that records which thread called it and a sink that blocks for a second.
#pragma once

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <thread>

namespace edvr {

class HeartbeatWriter {
public:
    // Writes one heartbeat line. Called on the writer thread only.
    using Sink = void (*)(uint64_t frameNo, uint64_t uptimeSeconds);

    // Start the writer thread. Returns false if the thread could not be made (the heartbeat is then simply absent: the
    // trail loses its steady-state crumbs, never a frame). Detached, as the journal worker is: the graphics DLL is
    // pinned before its first Present (module_pin.h), and the thread needs nothing joined.
    bool start(Sink sink) {
        bool expected = false;
        if (!started_.compare_exchange_strong(expected, true)) return true;
        sink_ = sink;
        wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!wake_) return false;
        try {
            std::thread(&HeartbeatWriter::run, this).detach();
        } catch (...) {
            return false;
        }
        return true;
    }

    // The render thread: this frame, this uptime, to be written. No file, no wait, no lock. A post after close() is
    // dropped. Single poster: the render thread is the only caller.
    void post(uint64_t frameNo, uint64_t uptimeSeconds) noexcept {
        if (closed_.load(std::memory_order_acquire) || !started_.load(std::memory_order_acquire)) return;
        const uint32_t v = version_.load(std::memory_order_relaxed);
        version_.store(v + 1, std::memory_order_relaxed);   // odd: a write is in progress
        std::atomic_thread_fence(std::memory_order_release);
        frame_.store(frameNo, std::memory_order_relaxed);
        uptime_.store(uptimeSeconds, std::memory_order_relaxed);
        version_.store(v + 2, std::memory_order_release);   // even: the record is whole
        posts_.fetch_add(1, std::memory_order_release);
        if (wake_) SetEvent(wake_);
    }

    // No heartbeat will start from now on, and a pending one is dropped. For the places that cannot wait: a
    // FreeLibrary unload, process exit (the other threads are already dead there; nothing below blocks).
    //
    // Sequentially consistent on purpose: this and run() are a store-then-load handshake (the closer stores
    // `closed_` and loads `writing_`; the writer stores `writing_` and loads `closed_`), and on x86 a weaker
    // order lets each side miss the other's store, which is the one outcome that writes a heartbeat after the
    // crash lines.
    void close() noexcept {
        closed_.store(true, std::memory_order_seq_cst);
        if (wake_) SetEvent(wake_);
    }

    // As close(), and then waits up to `maxWaitMs` for a heartbeat that has already started writing to finish, so
    // the lines written after this are the last in the file. Returns true when nothing is being written any more.
    // For the unhandled-exception filter: the process is dying, so a short bounded wait costs nothing, and a
    // filter must never wait without a bound.
    bool closeAndDrain(unsigned maxWaitMs) noexcept {
        close();
        const ULONGLONG until = GetTickCount64() + maxWaitMs;
        while (writing_.load(std::memory_order_seq_cst)) {
            if (GetTickCount64() >= until) return false;
            Sleep(1);
        }
        return true;
    }

    // The writer thread's own counters, for the rig.
    uint64_t posts() const noexcept { return posts_.load(std::memory_order_acquire); }
    uint64_t written() const noexcept { return written_.load(std::memory_order_acquire); }
    bool closed() const noexcept { return closed_.load(std::memory_order_acquire); }
    bool writing() const noexcept { return writing_.load(std::memory_order_acquire); }
    bool exited() const noexcept { return exited_.load(std::memory_order_acquire); }
    // The rig lets the thread go: it leaves its loop (a process never needs this; the thread dies with it).
    void stop() noexcept {
        stop_.store(true, std::memory_order_release);
        if (wake_) SetEvent(wake_);
    }
    // Wake the writer without a post: what close() and stop() do, and the rig's spurious wake. It must write nothing
    // (no new post, no line) and go back to sleep without spinning.
    void wake() noexcept {
        if (wake_) SetEvent(wake_);
    }

    // The newest post, whole: re-read until the version is even and unchanged around the reads. False when
    // nothing has been posted yet. This is the reader the writer thread uses; the rig also calls it from a second
    // thread against a poster at full speed.
    bool latest(uint64_t& frameNo, uint64_t& uptimeSeconds) const noexcept {
        for (;;) {
            const uint32_t before = version_.load(std::memory_order_acquire);
            if (before & 1u) {
                YieldProcessor();
                continue;
            }
            frameNo = frame_.load(std::memory_order_relaxed);
            uptimeSeconds = uptime_.load(std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_acquire);
            if (version_.load(std::memory_order_relaxed) == before) return before != 0;
        }
    }

    // A seam for the rig, and nothing else: called on the writer thread between its last look at `closed_` and the
    // moment it marks itself writing, which is the one place a close() can land that the first look missed. The rig
    // closes the writer from the hook and requires that nothing is written.
    using Hook = void (*)(void*);
    void setBetweenChecksHook(Hook hook, void* arg) noexcept {
        hookArg_.store(arg, std::memory_order_relaxed);
        hook_.store(hook, std::memory_order_release);
    }

private:

    void run() noexcept {
        uint64_t lastPost = 0;
        while (!stop_.load(std::memory_order_acquire)) {
            WaitForSingleObject(wake_, INFINITE);
            if (stop_.load(std::memory_order_acquire)) break;
            if (closed_.load(std::memory_order_acquire)) continue;   // dropped: nothing is written after a close
            const uint64_t posted = posts_.load(std::memory_order_acquire);
            if (posted == lastPost) continue;                        // nothing new: never a heartbeat of its own
            uint64_t frameNo = 0, uptime = 0;
            latest(frameNo, uptime);
            lastPost = posted;
            if (Hook hook = hook_.load(std::memory_order_acquire)) hook(hookArg_.load(std::memory_order_relaxed));
            // `writing_` brackets the sink; closed_ is checked inside it too, so a close() that lands between the
            // check above and the flag below still stops the write (closeAndDrain waits only for a write that began).
            writing_.store(true, std::memory_order_seq_cst);
            if (!closed_.load(std::memory_order_seq_cst) && sink_) {
                sink_(frameNo, uptime);
                written_.fetch_add(1, std::memory_order_release);
            }
            writing_.store(false, std::memory_order_release);
        }
        exited_.store(true, std::memory_order_release);
    }

    std::atomic<bool> started_{false}, closed_{false}, stop_{false}, writing_{false}, exited_{false};
    HANDLE wake_ = nullptr;
    Sink sink_ = nullptr;
    // The record the render thread posts and the writer reads.
    std::atomic<uint32_t> version_{0};
    std::atomic<uint64_t> frame_{0}, uptime_{0};
    std::atomic<uint64_t> posts_{0}, written_{0};
    std::atomic<Hook> hook_{nullptr};
    std::atomic<void*> hookArg_{nullptr};
};

}  // namespace edvr
