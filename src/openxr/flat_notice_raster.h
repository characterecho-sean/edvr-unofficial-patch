#pragma once

#include <windows.h>
#include <dxgiformat.h>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <string>
#include <utility>
#include <vector>
#include "flat_notice_placement.h"

namespace edvr::openxr {

// The flat-mode notice, rasterised by GDI into a 32-bit top-down BGRA bitmap: black, two centred lines of orange text. Written
// here rather than taken from the graphics half's panel code, so the runtime does not link that code.
struct NoticeBitmap {
  int32_t width = 0, height = 0;
  std::vector<uint8_t> bgra;
  std::wstring face;          // the face GDI selected for both lines
  bool fallbackFace = false;  // Bahnschrift was not the face selected, so Segoe UI was used
};

// Elite's HUD orange (#FF7100) for the heading, and a dimmer orange (#C85800) for the detail, so the heading reads first.
struct NoticeRgb {
  uint8_t r, g, b;
};
constexpr NoticeRgb kNoticeHeading{255, 113, 0};
constexpr NoticeRgb kNoticeDetail{200, 88, 0};

// Bahnschrift (Windows 10 1709 and later) is DIN-like and squared, the nearest face we can ship to Elite's Eurostile.
constexpr wchar_t kNoticeFace[] = L"Bahnschrift";
constexpr wchar_t kNoticeFallbackFace[] = L"Segoe UI";

inline HFONT makeNoticeFont(int32_t cell, int32_t weight, const wchar_t* face) {
  LOGFONTW lf{};
  lf.lfHeight = -cell;  // a character cell height, not a point size
  lf.lfWeight = weight;
  lf.lfQuality = ANTIALIASED_QUALITY;
  wcscpy_s(lf.lfFaceName, face);
  return CreateFontIndirectW(&lf);
}

// GDI substitutes another face for a name it does not have, so the face is read back after it is selected. A family's named
// instances (Bahnschrift SemiBold and the like) count as Bahnschrift.
inline bool noticeFaceInstalled(HDC dc) {
  HFONT probe = makeNoticeFont(16, FW_SEMIBOLD, kNoticeFace);
  if (!probe) return false;
  HGDIOBJ previous = SelectObject(dc, probe);
  wchar_t selected[LF_FACESIZE]{};
  GetTextFaceW(dc, LF_FACESIZE, selected);
  SelectObject(dc, previous);
  DeleteObject(probe);
  return _wcsnicmp(selected, kNoticeFace, std::wcslen(kNoticeFace)) == 0;
}

// The heading's letter spacing: about 6% of its font cell, at least one pixel.
inline int32_t noticeHeadingSpacing(int32_t cell) { return std::max<int32_t>(1, (cell * 6 + 50) / 100); }

// panelWidth is the notice's width in pixels; imageHeight is the eye image's height, which sets the font sizes. The heading is set
// in uppercase with spacing and shrinks until it fits the panel's width on one line; the detail wraps as it always has.
inline bool rasterizeFlatNotice(int32_t panelWidth, uint32_t imageHeight, NoticeBitmap& out) {
  out = {};
  const int32_t headline = noticeHeadlineHeight(imageHeight), detail = noticeDetailHeight(imageHeight);
  if (panelWidth <= 0 || headline <= 0 || detail <= 0) return false;
  const int32_t margin = std::max<int32_t>(1, int32_t(imageHeight / 40));
  const wchar_t* line1 = L"ELITE IS IN FLAT MODE";
  const wchar_t* line2 = L"Turn 3D on in Elite's graphics options to return to VR";

  HDC dc = CreateCompatibleDC(nullptr);
  if (!dc) return false;
  const bool fallback = !noticeFaceInstalled(dc);
  const wchar_t* face = fallback ? kNoticeFallbackFace : kNoticeFace;
  HFONT font1 = nullptr, font2 = nullptr;
  HBITMAP bitmap = nullptr;
  HGDIOBJ previous = nullptr;
  bool ok = false;

  // Line 1: shrink by one cell at a time until its spaced width fits the panel.
  int32_t cell1 = headline, h1 = 0;
  SIZE s1{};
  for (;;) {
    if (font1) {
      SelectObject(dc, GetStockObject(SYSTEM_FONT));  // a font is never deleted while it is selected into the DC
      DeleteObject(font1);
    }
    font1 = makeNoticeFont(cell1, FW_SEMIBOLD, face);
    if (!font1) break;
    SelectObject(dc, font1);
    SetTextCharacterExtra(dc, noticeHeadingSpacing(cell1));
    GetTextExtentPoint32W(dc, line1, int32_t(std::wcslen(line1)), &s1);
    if (s1.cx <= panelWidth || cell1 <= 1) break;
    --cell1;
  }
  h1 = s1.cy;

  // Line 2: sentence case, no spacing, wrapping inside the panel.
  SetTextCharacterExtra(dc, 0);
  font2 = makeNoticeFont(detail, FW_NORMAL, face);
  RECT m2{0, 0, panelWidth, 0};
  if (font1 && font2) {
    SelectObject(dc, font2);
    DrawTextW(dc, line2, -1, &m2, DT_CENTER | DT_WORDBREAK | DT_CALCRECT);
  }
  const int32_t h2 = m2.bottom - m2.top;

  if (font1 && font2 && h1 > 0 && h2 > 0) {
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
      // White on black: GDI's grayscale antialiasing leaves each pixel's coverage in its channels, which the tint below uses.
      SetTextColor(dc, RGB(255, 255, 255));
      // Centred on its own extent, which includes the letter spacing. DrawText's centring leaves the spacing out, which pushed the
      // heading off the panel's right edge.
      SelectObject(dc, font1);
      SetTextCharacterExtra(dc, noticeHeadingSpacing(cell1));
      TextOutW(dc, (panelWidth - s1.cx) / 2, margin, line1, int32_t(std::wcslen(line1)));
      SetTextCharacterExtra(dc, 0);
      const int32_t split = margin + h1 + gap;
      RECT r2{0, split, panelWidth, split + h2};
      SelectObject(dc, font2);
      DrawTextW(dc, line2, -1, &r2, DT_CENTER | DT_WORDBREAK);
      // Each pixel's coverage times the line's colour, so the glyph edges fade into the black and never fringe white.
      auto* px = static_cast<uint8_t*>(bits);
      for (int32_t y = 0; y < height; ++y) {
        const NoticeRgb c = y < split ? kNoticeHeading : kNoticeDetail;
        uint8_t* row = px + size_t(y) * size_t(panelWidth) * 4;
        for (int32_t x = 0; x < panelWidth; ++x, row += 4) {
          const uint32_t cover = (uint32_t(row[0]) + row[1] + row[2]) / 3;
          row[0] = uint8_t((cover * c.b + 127) / 255);
          row[1] = uint8_t((cover * c.g + 127) / 255);
          row[2] = uint8_t((cover * c.r + 127) / 255);
          row[3] = 0xFF;  // GDI leaves alpha unset; the notice is opaque
        }
      }
      out.width = panelWidth;
      out.height = height;
      out.bgra.assign(px, px + bytes);
      out.face = face;
      out.fallbackFace = fallback;
      ok = true;
    }
  }
  if (previous) SelectObject(dc, previous);
  SelectObject(dc, GetStockObject(SYSTEM_FONT));
  if (bitmap) DeleteObject(bitmap);
  if (font1) DeleteObject(font1);
  if (font2) DeleteObject(font2);
  DeleteDC(dc);
  return ok;
}

// The bytes for one of the four formats the stereo renderer can choose. The colours are sRGB bytes already, so they go in
// unchanged; only the channel order follows the format. False for any other format.
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
