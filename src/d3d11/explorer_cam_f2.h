#pragma once
// The F2 instruments of the Explorer Cam redesign (advanced.explorer_cam_probe, temporary, LOG ONLY; explorer_cam_f2_core.h says what
// they read and why). The probe (explorer_cam_probe.cpp) owns the key and the cadence and calls these; nothing here writes the game's
// memory except the one 8-byte slot of the eye interface's vtable, which is swapped for a function that returns exactly what the
// original returns.
//   I3 pressed  the free-camera activity's action objects' pressed ints, on change      (free-camera observer)
//   I4          the camera controller's mode byte, pressed ints and call rate            (controller observer)
//   N           the commander's eye: the view-point interface's matrix accessor's readers (a vtable slot swap)
// The next instrument attaches the same way: an observer on a hook of explorer_cam.h, and a tick.
#include <cstddef>
#include <cstdint>

#include "explorer_cam_core.h"

namespace edvr {

// The probe's boundary, on the frame thread: attach the controller observer, swap the neck's slot, say what happened once.
void explorerCamF2Arm(const ecm::Sink& sink);
// Detach the observers. The slot swap stays in place (it is a faithful getter) until the game exits.
void explorerCamF2Disarm();
// The free-camera hook's observers (after the original returns, after every press is restored): pressed ints, and the neck's paired
// sample. Hook thread; takes no lock, allocates nothing, logs nothing.
void explorerCamF2FreeCamera(void* activity) noexcept;
void explorerCamF2Controller(void* controller) noexcept;
// The avatar fade counter's observer (after the dither-fade update): hook threads, many components a frame. Takes no lock, logs nothing.
void explorerCamF2Fade(void* component) noexcept;
// Once a frame, on the frame thread: the change lines, the 5 s heartbeats and the 1 Hz neck lines.
void explorerCamF2Tick(uint32_t frame, uint64_t nowMs, const ecm::Sink& sink);

#ifdef EDVR_EXPLORER_CAM_TEST
namespace explorercamf2test {
struct NeckSeam {
    uintptr_t vtable = 0, slot = 0, getter = 0, localSite = 0;
};
void setNeckTargets(const NeckSeam& targets);   // the synthetic vtable, its accessor and the return address that counts as local
struct HeadSeam {
    uintptr_t base = 0;       // a synthetic "game image": the head instrument's addresses are this + the build's RVAs
    size_t imageSize = 0;
};
void setHeadTargets(const HeadSeam& targets);
void setHeadInterval(uint32_t ms);              // the rig evaluates every free-camera call (0), not once a second
uint64_t headSteps();
uint64_t headCalls();
uint64_t headFaults();
uint32_t headState();                           // 0 not tried, 1 armed, 2 stood down
uint64_t fadeCalls();
uint64_t fadeEnabledCalls();
uint64_t fadeEnabledWhileZero();
size_t fadeDistinct();
uint64_t neckCalls();
uint64_t neckLocalSiteCalls();
uintptr_t neckLocalEye();
size_t neckDistinct();
bool neckInstalled();
uintptr_t neckReplacement();                    // what the slot holds when the swap is in
void reset();                                   // the swap undone, every latch and counter cleared
}  // namespace explorercamf2test
#endif

}  // namespace edvr
