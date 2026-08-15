// GENERATED from src/common/frame_flag.h in the private edvr repo -- do not edit here.
// Edit there, then: python tools/sync_common.py --write   [body-sha256 7a73700fd8052ba0]
// A one-bit channel between the two proxies, for the frame that must not be
// shown.
//
// The bad frame is detected in d3d11.dll, part-way through rendering, by
// watching where the game is drawing from. The decision it feeds -- whether to
// hand that frame to the headset -- belongs to openvr_api.dll. Those are
// separate modules loaded from different directories, so they cannot share a
// global.
//
// A named shared mapping is used rather than an exported symbol because it does
// not care which module loads first, works if one of them is absent, and adds
// nothing to either DLL's export table. It is per-process and holds two
// integers.
//
// Nothing here reads or writes game state. It carries one flag between two parts
// of EDVR.
#pragma once

#include <cstdint>

namespace edvr {

// Mark the frame in progress as one that should not reach the headset. Cheap and
// safe to call from the draw path.
void markGlitchFrame();

// True if the frame in progress has been marked.
bool glitchFrameMarked();

// Withdraw a mark WITHIN the frame that made it.
//
// The detector re-decides several times per frame, as each new camera candidate
// arrives, and an early candidate can look like a jump while the frame's final
// verdict is no.
//
// Identical to clearGlitchFrame() today, and kept as its own name because the
// two mean different things to a reader: one is "I was wrong", the other is
// "this frame is over". They used to differ -- a `counted` field distinguished
// them, so a withdrawn mark would not be tallied twice -- but that total moved
// into glitch_frame.cpp and the field became write-only. It has been removed
// rather than left as a mechanism the header describes and nothing implements.
void unmarkGlitchFrame();

// Called once per frame, so a mark applies to exactly one frame. Without this a
// single detection would suppress every frame that followed.
void clearGlitchFrame();

// Announced by openvr_api.dll once its hook is validated, and read by d3d11.dll.
//
// The two halves install separately and the openvr one is optional, so d3d11 can
// detect a bad frame and have nothing on the other end to act on it. Without
// this the log would report "12 frames withheld" to somebody who skipped the
// second file and had none withheld at all -- which is exactly the kind of
// counter that gets believed and wastes a day.
void announceGlitchConsumer();
bool glitchConsumerPresent();

// The player is on foot in the external camera, having arrived there from the
// flat panel -- the one state where moving the head pose is wanted.
//
// Set by d3d11.dll, which is the only half that can tell the modes apart: it
// watches the panel composite, and the panel stopping while a full scene is
// drawn into the eyes is what the transition looks like. Read by
// openvr_api.dll, which is the only half that can act on it, because the head
// pose passes through there. Neither can do the other's job, which is why this
// is a channel rather than a local.
//
// UNLIKE the glitch flag, this one is a STATE and persists across frames. It is
// not cleared at the frame boundary; it is cleared when the panel comes back.
// Call this EVERY frame, with the current answer, not only when it changes. The
// repetition is the point: it is also the heartbeat that externalCameraOnFootLive
// reads, so "d3d11 says no" and "d3d11 has stopped saying anything" stay
// distinguishable.
void setExternalCameraOnFoot(bool on);

// The last value written, whenever it was written.
//
// Prefer externalCameraOnFootLive for anything that MOVES THE PLAYER. This one
// cannot tell a current "yes" from a "yes" left behind by a writer that has
// since stopped, and the header used to say as much without doing anything
// about it: "a wrong answer here does not expire on its own".
bool externalCameraOnFoot();

// True only if d3d11.dll says yes AND has said something within maxAgeFrames
// calls of this function.
//
// WHY A HEARTBEAT. The writer runs inside a fault-budgeted SEH guard on the
// Present path, and that guard stops running its body permanently after a few
// faults. The gate would then freeze at whatever it last published. Frozen ON
// means the head offset stays applied in every mode -- including the cockpit --
// for the rest of the session, with both logs still saying it is gated, because
// no line is printed for a decision that is never re-made. The same freeze
// follows from the d3d11 hook being lost to a recreated device or swapchain.
//
// This is the argument the openvr half already makes for keeping the offset
// itself outside its own budgeted guard: a fault budget is right for logging,
// where losing a line costs nothing, and wrong for anything that changes what
// the player sees. The writer here cannot move out of its guard -- it is
// derived from the render state the guard exists to inspect -- so the reader
// stops trusting it instead.
//
// Counted in READER frames rather than time. Both halves run once per rendered
// frame, so the units match without a clock, and a stall that freezes both
// halves together does not age the flag while nothing is being drawn anyway.
bool externalCameraOnFootLive(uint32_t maxAgeFrames);

// Has ANYTHING ever published the mode, whatever it said?
//
// "Nobody is publishing" and "the publisher says no" are different facts and a
// reader that cannot tell them apart will accuse a healthy install of being
// broken. That is not hypothetical: the openvr side warned "nothing has
// reported the player's mode" on every correctly-configured session about forty
// seconds in, because until the player first enters the camera the gate is
// publishing "no" continuously and the two look identical from here.
bool externalCameraEverPublished();

// Ask the openvr half to decline the next `frames` frames.
//
// NOT A DETECTION, and that is the whole point of it. Everything else across
// this channel is one half telling the other what it inferred; this is the
// player saying so. They pressed the external-camera key, so a transition is
// starting -- there is nothing to recognise and nothing to be wrong about.
//
// It exists because during that transition Elite draws several frames from
// somewhere the player is not -- confirmed with fix.transition_flash = 0, so it
// is the game's and not ours -- and detection cannot help: withholding shows the
// PREVIOUS frame, and by the time a detector has recognised a bad one, the
// previous frame is already bad too. Starting at the press means the frame being
// held is the last good one before any of it.
//
// The cost is unmeasured and is the reason this ships off by default. One
// withheld frame has measured 74-82 ms because the compositor waits for a submit
// that never comes; whether a RUN of them costs that each or settles into steady
// reprojection is not known, and the difference is between a smooth hold and a
// freeze. The ring's timing column is the readout.
void requestSubmitHold(uint32_t frames);

// One frame of that hold, consumed by the reader. True while the hold is live.
bool takeSubmitHoldFrame();

// ONE VERDICT PER FRAME, over a channel that carries no frame identity.
//
// The mark above is read once per eye, at each Submit, and it legitimately
// changes DURING a frame: the detector re-decides on every new furthest camera
// and can set, withdraw and re-raise it by design. So a transition landing
// between Submit(left) and Submit(right) shows one eye this frame and the other
// a reprojection of the last one -- a one-frame binocular mismatch, which is
// what a flash feels like. The fix for flashes, producing one.
//
// It cannot be solved by comparing frame numbers, because this channel does not
// carry one (EDVR-31). It is solved by sampling once and making the second eye
// follow, which is available without any protocol change at all.
//
// CONSISTENT-LATE BEATS SPLIT, in both directions. Two eyes showing the previous
// frame is a dropped frame, which runtimes reproject and people are used to. Two
// eyes disagreeing is not something either handles.
//
// Header-only and pure, so the property can be asserted without a compositor --
// and shared, so the two copies of the hook cannot drift. compositor_hook.cpp is
// FORKED and its "kept aligned by hand" regions have no mechanical check, which
// is the standing gap in the sync tooling; this is four lines fewer to keep
// aligned by remembering.
class SubmitPairLatch {
public:
    // The verdict for this frame. The first call after reset() decides it; every
    // later call in the same frame gets the same answer, whatever the flag has
    // done since.
    bool verdict(bool markedNow) {
        if (!m_latched) {
            m_latched = true;
            m_withhold = markedNow;
        }
        return m_withhold;
    }

    // Called at the frame boundary -- WaitGetPoses, which is where the mark
    // itself is cleared.
    void reset() {
        m_latched = false;
        m_withhold = false;
    }

    bool latched() const { return m_latched; }

private:
    bool m_latched = false;
    bool m_withhold = false;
};

}  // namespace edvr
