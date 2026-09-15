#pragma once
#include <cstdint>

namespace edvr::openxr {
// Elite truncates its HMD quality multiplier to integer pixels. Even game
// recommendations keep a half-size input within NGX's render-size limits.
// This policy does not alter runtime swapchain sizes or projection geometry.
inline uint32_t gameFacingDimension(uint32_t value) noexcept {
  if (!value || value > 16384u) return 0;
  return (value + 1u) & ~1u;
}
}
