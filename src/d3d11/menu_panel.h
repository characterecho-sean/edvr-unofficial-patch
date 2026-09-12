// The settings menu's pixels: rasterised on the CPU, composited at the door.
//
// RASTERISATION is GDI into a 32-bit DIB -- CreateFontW and DrawTextW, the
// installer's own text path, and the game already imports GDI32, so nothing
// new enters the process. It runs on a worker thread, on CHANGE only, and
// hands the frame thread a finished RGBA bitmap to upload; per frame the
// panel costs one compute dispatch per eye and nothing on the CPU.
//
// Two passes make one premultiplied bitmap: the same draw list rendered once
// in colour over the panel's premultiplied background, and once in white
// over the background's alpha, so the second image IS the first's coverage.
// GDI blends antialiased text exactly the way premultiplied compositing
// wants, which is what makes the two-pass trick exact rather than a guess.
//
// TYPE IN DEGREES. The bitmap is sized from the headset's own pixels per
// degree at the panel (eye size and tangents from the channel), so a Pimax
// and a Quest 3 get the same apparent size from different pixel counts, and
// the panel composites near 1:1 -- native-crisp whatever render scale says.
//
// THE COMPOSITE is a compute pass in the FSS theater's shape: per output
// pixel of the eye's region, build the view ray from the published
// tangents, rotate it into the anchor's frame, intersect the panel (flat or
// on a cylinder), and blend the sampled bitmap over the frame's pixel. The
// source is read through a view or a copy, never written; the result is an
// EDVR-owned region-sized texture the openvr half forwards, the sharpen
// pass's exact contract.
#pragma once

#include <cstdint>

struct ID3D11Device;
struct ID3D11DeviceContext;

namespace edvr {

// GPU ray tangents: physical left/right/up/down magnitudes. The shared
// vertical pair uses OpenVR's historical pfTop/pfBottom names instead.
void menuPanelFrustum(int eye, uint32_t w, uint32_t h, float tans[4]);

enum MenuLineStyle : uint8_t {
    kMenuRow = 0,       // label left, value right
    kMenuRowHi = 1,     // the highlighted row
    kMenuHeading = 2,   // a group heading
    kMenuInfo = 3,      // status text: label left, value right, no highlight
    kMenuDim = 4,       // a read-only row
    kMenuNote = 5,      // one full-width line of small text, `left` only
    kMenuRowEdit = 6,   // the row whose value is being typed: a field and a caret
};

enum MenuBadge : uint8_t {
    kBadgeNone = 0,
    kBadgeRestart = 1,   // "restart"
    kBadgePending = 2,   // "at next launch", the value beside it is the pending one
    kBadgeUnknown = 3,   // "?" -- when it applies is not documented
};

struct MenuLine {
    char    left[96];
    char    right[64];
    uint8_t style;
    uint8_t badge;
    uint8_t toggle;   // 0 none; 1 off, 2 on: a switch is drawn in place of `right`
};

// THE TOOLTIP'S STRIP, as fractions of the menu card's width: the gap
// between the two cards, and the tooltip card itself. The model reserves
// the bitmap from these and the raster draws from them, so they live here
// and not in either file.
constexpr float kTipGapFrac = 0.05f;
constexpr float kTipWidthFrac = 0.52f;
constexpr float kTipRatio = 1.0f + kTipGapFrac + kTipWidthFrac;

constexpr int kMenuMaxTabs = 8;
constexpr int kMenuMaxLines = 14;
constexpr int kMenuMaxTiles = 16;

// A gauge: a big number with a small caption above and one small line
// below -- the Monitor page's shape, four across. A sentence of numbers is
// unreadable in a headset; a tile is read at a glance.
struct MenuTile {
    char caption[24];
    char value[24];
    char sub[40];
};

// One frame-time strip: the samples, what they are, and the budget line.
struct MenuGraph {
    float samples[120];
    int   count = 0;        // how many samples; 0 draws nothing
    float budgetMs = 11.1f;
    char  label[64];
};
constexpr int kMenuMaxGraphs = 2;

// Everything the panel shows, as text. The model builds one of these on
// every change; the raster lays it out.
struct MenuContent {
    // The tabs the strip shows: a window of the pages that fits the panel,
    // `activeTab` indexing THIS array. An arrow at either end says there
    // are pages that way.
    char     tabs[kMenuMaxTabs][24];
    int      tabCount = 0;
    int      activeTab = 0;
    bool     tabMoreLeft = false;
    bool     tabMoreRight = false;
    MenuLine lines[kMenuMaxLines];
    int      lineCount = 0;
    // Tiles are laid out ABOVE the lines, `tileColumns` across (4 when 0).
    MenuTile tiles[kMenuMaxTiles];
    int      tileCount = 0;
    int      tileColumns = 0;
    char     hint[200];     // an explanation under the rows (information pages)
    char     footer[160];   // keys, pending-restart count, warnings
    // The tooltip: a title line and a wrapped body, drawn in the STRIP
    // beside the card (from `cardPx` to `widthPx`), level with line
    // `popupLine`; -1 for none. Its card is sized to the measured height of
    // its own text and capped by the panel's height, so it never changes
    // the bitmap. The body is what fits this buffer -- the facts the model
    // puts first always do; a long ini comment is what gets cut.
    char     popupTitle[96];
    char     popup[1024];
    int      popupLine = -1;
    int      popupScroll = 0;   // lines of the body scrolled past
    bool     toast = false; // a one-line panel instead of the menu
    bool     compact = false;   // info pages: a tighter row pitch
    // Sizing, decided by the model from the channel: the bitmap's width in
    // pixels, the MENU CARD's width within it, and the cap height of row
    // text. The bitmap is wider than the card so the tooltip has somewhere
    // to sit that is not on top of the values; the pixels between them are
    // transparent, and the anchor is offset so the card still lands where
    // the user was looking. cardPx 0 means the card is the whole bitmap.
    int      widthPx = 0;
    int      cardPx = 0;
    int      capPx = 0;
    // The Monitor page's strips: the last `count` frames in ms, oldest
    // first, drawn as bars against the display's budget. fpsVR draws the
    // GPU and the CPU as two strips, because a frame over budget on one is
    // a different problem from a frame over budget on the other.
    MenuGraph graphs[kMenuMaxGraphs];
    int       graphCount = 0;   // how many strips, not how many samples
};

// Where the panel sits, in the anchor's frame: metres to it, how much it
// wraps (0 flat .. 0.9), its half-width along the surface, and the fade.
//
// `halfW` is half of the WHOLE BITMAP, the tooltip's strip included --
// `halfH = halfW * menuPanelAspect()` is only correct under that reading,
// because the aspect is the bitmap's. `shift` slides the bitmap along its
// own surface so that the MENU CARD's middle, not the bitmap's, lands on
// the anchor's forward; it is metres, positive to the right, and the same
// value must reach the shader, the hit test and the culling box or the
// three will describe different panels.
struct MenuGeometry {
    float dist = 1.4f;
    float curve = 0.0f;
    float halfW = 0.3f;
    float shift = 0.0f;
    float alpha = 0.0f;
};

// Hand the raster a new content (copied; the worker wakes). Frame thread.
void menuPanelSubmit(const MenuContent& c);
#ifdef EDVR_MENU_TEST
bool menuPanelWorkerReadyForTest();
#endif

// Once per frame: upload a finished raster, create the texture as needed.
void menuPanelTick(ID3D11Device* dev);

void menuPanelSetGeometry(const MenuGeometry& g);

// The last built raster's height over its width (0 until one exists).
float menuPanelAspect();

// How many line-steps the tooltip's body could still be scrolled in the
// raster now showing: 0 when it all fits. The model clamps its own counter
// to this, so the scroll cannot run off the end of a text only the raster
// has measured.
int menuPanelPopupScrollMax();

// Which line a panel-relative point lands on: u 0..1 left to right, v 0..1
// TOP to bottom. -1 for none, a heading, no raster yet, or a point in the
// tooltip's strip rather than on the menu card.
int menuPanelLineAt(float u, float v);

// The panel's ray intersection, pure, shared by the CPU aim and (by
// transcription) the shader: `org` and `dir` in the anchor's frame,
// returns whether the ray hits the panel and where, su 0..1 left to right,
// sv 0..1 BOTTOM to top.
bool menuPanelHit(const float org[3], const float dir[3], float dist, float curve,
                  float halfW, float halfH, float shift, float* su, float* sv);

// For the Status page: the raster's size, how long the last one took on the
// CPU, and the composite's measured GPU price per eye (0 until measured).
bool menuPanelStats(int* w, int* h, double* lastMs, float* gpuMs);

// The footer's ruler: the single-line pixel width of `utf8` in the raster's
// own face at `emPx` (the footer's em is capPx), measured by the same
// DrawTextW that will draw it. The model composes the footer against it
// (menu_keys.h, menuComposeFooter) so what is drawn is what fits, rather
// than what an ellipsis leaves; MEASURED 2026-09-11: the old 115-character
// footer was 1520 px in a 770 px line at the default size. GDI only; no
// device, callable from a test.
int menuPanelMeasureLine(const char* utf8, int emPx);

// For the test: lay `c` out without rasterising. Returns the bitmap's
// height; `footTop`/`footBottom` receive the footer box's edges in pixels
// (-1 when no footer op was made); `rowEdges` receives up to `maxRows`
// row rectangles as y0,y1 pairs, fractions of the height as menuPanelLineAt
// reads them (-1 past the rows present); `footLines` receives the number
// of single-line ops the footer was split into (one per '\n'-separated
// line, so a second line is drawn as its own line and not appended to the
// first).
int menuPanelLayoutHeightForTest(const MenuContent& c, int* footTop, int* footBottom,
                                 float* rowEdges, int maxRows, int* footLines = nullptr);

void menuPanelShutdown();

}  // namespace edvr

extern "C" {
// The door's call (the sharpen export's contract): srcTex is an
// ID3D11Texture2D* the openvr half is about to forward, eye 0 or 1, bounds
// the Submit's uMin, vMin, uMax, vMax or null, xf the 12 floats the theater
// uses (a row-major 3x3 taking current-head vectors into anchor space, then
// this eye's ray origin in anchor space). Returns the composited texture
// (EDVR-owned, region-sized, full-span content) or null: nothing to draw,
// or a refusal said once in the log; the caller forwards what it had.
__declspec(dllexport) void* edvrMenuPanel(void* srcTex, int eye, const float* bounds,
                                          const float* xf);
}
