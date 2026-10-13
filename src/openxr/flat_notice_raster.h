#pragma once

#include <windows.h>
#include <dxgiformat.h>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <utility>
#include <vector>
#include "flat_notice_placement.h"

namespace edvr::openxr {

// The flat-mode notice, rasterised by GDI into a 32-bit top-down BGRA bitmap: black, white text, two centred lines. Written here
// rather than taken from the graphics half's panel code, so the runtime does not link that code.
struct NoticeBitmap {
  int32_t width = 0, height = 0;
  std::vector<uint8_t> bgra;
};

// panelWidth is the notice's width in pixels; imageHeight is the eye image's height, which sets the font sizes.
inline bool rasterizeFlatNotice(int32_t panelWidth, uint32_t imageHeight, NoticeBitmap& out) {
  out = {};
  const int32_t headline = noticeHeadlineHeight(imageHeight), detail = noticeDetailHeight(imageHeight);
  if (panelWidth <= 0 || headline <= 0 || detail <= 0) return false;
  const int32_t margin = std::max<int32_t>(1, int32_t(imageHeight / 40));
  const wchar_t* line1 = L"Elite is in flat mode";
  const wchar_t* line2 = L"Turn 3D on in Elite's graphics options to return to VR";

  HDC dc = CreateCompatibleDC(nullptr);
  if (!dc) return false;
  const auto makeFont = [](int32_t cell) {
    LOGFONTW lf{};
    lf.lfHeight = -cell;  // a character cell height, not a point size
    lf.lfWeight = FW_NORMAL;
    lf.lfQuality = ANTIALIASED_QUALITY;
    wcscpy_s(lf.lfFaceName, L"Segoe UI");
    return CreateFontIndirectW(&lf);
  };
  HFONT font1 = makeFont(headline), font2 = makeFont(detail);
  HBITMAP bitmap = nullptr;
  HGDIOBJ previous = nullptr;
  bool ok = false;
  if (font1 && font2) {
    // Measure each line's height when it wraps inside the panel, then draw both at those heights.
    RECT m1{0, 0, panelWidth, 0}, m2{0, 0, panelWidth, 0};
    SelectObject(dc, font1);
    DrawTextW(dc, line1, -1, &m1, DT_CENTER | DT_WORDBREAK | DT_CALCRECT);
    SelectObject(dc, font2);
    DrawTextW(dc, line2, -1, &m2, DT_CENTER | DT_WORDBREAK | DT_CALCRECT);
    const int32_t h1 = m1.bottom - m1.top, h2 = m2.bottom - m2.top;
    const int32_t gap = std::max<int32_t>(1, detail / 2);
    const int32_t height = margin + h1 + gap + h2 + margin;

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = panelWidth;
    info.bmiHeader.biHeight = -height;  // top-down
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (bitmap && bits) {
      previous = SelectObject(dc, bitmap);
      const size_t bytes = size_t(panelWidth) * size_t(height) * 4;
      std::memset(bits, 0, bytes);
      SetBkMode(dc, TRANSPARENT);
      SetTextColor(dc, RGB(255, 255, 255));
      RECT r1{0, margin, panelWidth, margin + h1};
      SelectObject(dc, font1);
      DrawTextW(dc, line1, -1, &r1, DT_CENTER | DT_WORDBREAK);
      RECT r2{0, margin + h1 + gap, panelWidth, margin + h1 + gap + h2};
      SelectObject(dc, font2);
      DrawTextW(dc, line2, -1, &r2, DT_CENTER | DT_WORDBREAK);
      auto* px = static_cast<uint8_t*>(bits);
      for (size_t i = 3; i < bytes; i += 4) px[i] = 0xFF;  // GDI leaves alpha unset; the notice is opaque
      out.width = panelWidth;
      out.height = height;
      out.bgra.assign(px, px + bytes);
      ok = true;
    }
  }
  if (previous) SelectObject(dc, previous);
  if (bitmap) DeleteObject(bitmap);
  if (font1) DeleteObject(font1);
  if (font2) DeleteObject(font2);
  DeleteDC(dc);
  return ok;
}

// The bytes for one of the four formats the stereo renderer can choose. Black stays black and white stays white in each, so the
// notice needs no tone mapping; only the channel order follows the format. False for any other format.
inline bool noticeBytesFor(DXGI_FORMAT format, const NoticeBitmap& bitmap, std::vector<uint8_t>& out) {
  const bool rgba = format == DXGI_FORMAT_R8G8B8A8_UNORM || format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
  const bool bgraOrder = format == DXGI_FORMAT_B8G8R8A8_UNORM || format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
  if (!rgba && !bgraOrder) return false;
  out = bitmap.bgra;
  if (rgba)
    for (size_t i = 0; i + 3 < out.size(); i += 4) std::swap(out[i], out[i + 2]);
  return true;
}

}  // namespace edvr::openxr
