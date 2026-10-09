#pragma once

// The mono camera hooks (mono_camera_core.h says what they are for; docs\terrain-culling.md, round 5). TEMPORARY with
// advanced.cull_probe. Build 332841 only.
#include <cstdint>

namespace edvr::monocam {

// One call per frame boundary (native_frame.cpp's beginFrame). `wanted` is advanced.cull_probe being cycle, measure or mono: on the
// first such frame the hooks go in (once, pass or fail, with one line saying how), and from then on the relay gate follows `wanted`, so
// with the key back at off every call runs the game's original bytes. Writes the observation lines recorded since the last call.
// Frame thread only.
void frame(bool wanted, void (*sink)(const char*));

// The same for an executable image mapped at `base`: production passes the running executable, the rig a synthetic one.
void frameFor(uintptr_t base, bool wanted, void (*sink)(const char*));

// Rig only: put every patched byte back, release the relays and forget the lazy install, so the next frameFor starts over.
void uninstallForTest();

}  // namespace edvr::monocam
