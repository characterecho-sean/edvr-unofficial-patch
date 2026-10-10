#pragma once

// The game's executable, as its own PE headers say, and a return address as an RVA in it. The head-pose answer (head_pose_time.h) asks
// "did Elite itself call this?" of it: a return address inside the mapped image.
#include <cstdint>

namespace edvr::openxr {

struct ExeModule {
  uintptr_t base = 0, size = 0;
  uint32_t stamp = 0, imageSize = 0;
};
// This process's executable (openvr_system.cpp: the Windows headers stay out of this one, whose `near` macro would break the rigs
// that include it).
ExeModule readExeModule() noexcept;

// A return address as an RVA in the game's executable. kFrameUnknown is "not captured", kFrameOutside "not in the image".
constexpr uint32_t kFrameOutside = 0xFFFFFFFFu, kFrameUnknown = 0xFFFFFFFEu;
inline uint32_t frameRva(const ExeModule& module, uintptr_t address) {
  if (!address) return kFrameUnknown;
  if (!module.base || address < module.base || address - module.base >= module.size) return kFrameOutside;
  return static_cast<uint32_t>(address - module.base);
}

}  // namespace edvr::openxr
