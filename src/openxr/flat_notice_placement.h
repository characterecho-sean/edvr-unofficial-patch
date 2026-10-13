#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace edvr::openxr {

// Where the flat-mode notice sits on one eye's image (docs/design-unified-install-2026-10-12.md section 12, N2). Pure: no Windows,
// so tools\flat_clear_test pins it.
//
// The panel is 40% of the image width. Its centre sits on this eye's optical centre, found from the view's own tangents. A canted
// display (Pimax) has asymmetric fov, so the centre is not the image centre, and the placement never assumes a symmetric fov. Image
// y grows downward; angleUp is the top edge.
struct NoticeRect {
  int32_t x = 0, y = 0, width = 0, height = 0;
};

inline int32_t noticePanelWidth(uint32_t imageWidth) {
  return int32_t(std::lround(0.40 * double(imageWidth)));
}

// The two lines' character cell heights: the eye image's height over 28 for the headline, over 36 for the line under it.
inline int32_t noticeHeadlineHeight(uint32_t imageHeight) { return int32_t(imageHeight / 28); }
inline int32_t noticeDetailHeight(uint32_t imageHeight) { return int32_t(imageHeight / 36); }

// The panel's rectangle on the image, centred on the optical centre, clamped inside the image. False for a degenerate fov (a
// non-positive span) or a tangent that is not finite.
inline bool flatNoticeRect(uint32_t imageWidth, uint32_t imageHeight, int32_t panelWidth, int32_t panelHeight,
                           double tanLeft, double tanRight, double tanUp, double tanDown, NoticeRect& out) {
  if (!imageWidth || !imageHeight || panelWidth <= 0 || panelHeight <= 0) return false;
  for (double t : {tanLeft, tanRight, tanUp, tanDown})
    if (!std::isfinite(t)) return false;
  const double spanX = tanRight - tanLeft, spanY = tanUp - tanDown;
  if (!(spanX > 0) || !(spanY > 0)) return false;
  // Fraction of the way from the left edge to the right, and from the top edge to the bottom, where the view's straight-ahead lies.
  // Clamped before lround so a near-zero span cannot overflow the conversion.
  const double cx = std::clamp(double(imageWidth) * (-tanLeft) / spanX, -double(imageWidth), 2.0 * double(imageWidth));
  const double cy = std::clamp(double(imageHeight) * tanUp / spanY, -double(imageHeight), 2.0 * double(imageHeight));
  const int32_t w = std::min<int32_t>(panelWidth, int32_t(imageWidth));
  const int32_t h = std::min<int32_t>(panelHeight, int32_t(imageHeight));
  const int32_t x = std::clamp(int32_t(std::lround(cx)) - w / 2, 0, int32_t(imageWidth) - w);
  const int32_t y = std::clamp(int32_t(std::lround(cy)) - h / 2, 0, int32_t(imageHeight) - h);
  out = NoticeRect{x, y, w, h};
  return true;
}

}  // namespace edvr::openxr
