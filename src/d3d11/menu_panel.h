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
    // The tooltip beside the highlighted row: a title line and a wrapped
    // body, drawn over the rows below (or above) line `popupLine`; -1 for
    // none. Sized to its text, up to nine lines of it.
    char     popupTitle[96];
    char     popup[720];
    int      popupLine = -1;
    bool     toast = false; // a one-line panel instead of the menu
    bool     compact = false;   // info pages: a tighter row pitch
    // Sizing, decided by the model from the channel: bitmap width in
    // pixels, and the cap height of row text in pixels.
    int      widthPx = 0;
    int      capPx = 0;
    // The Monitor page's frame-time strip: the last graphCount frame
    // intervals in ms, oldest first, drawn as bars against the display's
    // budget; graphCount 0 draws nothing.
    float    graph[120];
    int      graphCount = 0;
    float    graphBudgetMs = 11.1f;
    char     graphLabel[48];
};

// Where the panel sits, in the anchor's frame: metres to it, how much it
// wraps (0 flat .. 0.9), its half-width along the surface, and the fade.
struct MenuGeometry {
    float dist = 1.4f;
    float curve = 0.0f;
    float halfW = 0.3f;
    float alpha = 0.0f;
};

// Hand the raster a new content (copied; the worker wakes). Frame thread.
void menuPanelSubmit(const MenuContent& c);

// Once per frame: upload a finished raster, create the texture as needed.
void menuPanelTick(ID3D11Device* dev);

void menuPanelSetGeometry(const MenuGeometry& g);

// The last built raster's height over its width (0 until one exists).
float menuPanelAspect();

// Which line a panel-relative point lands on: u 0..1 left to right, v 0..1
// TOP to bottom. -1 for none, a heading, or no raster yet.
int menuPanelLineAt(float u, float v);

// The panel's ray intersection, pure, shared by the CPU aim and (by
// transcription) the shader: `org` and `dir` in the anchor's frame,
// returns whether the ray hits the panel and where, su 0..1 left to right,
// sv 0..1 BOTTOM to top.
bool menuPanelHit(const float org[3], const float dir[3], float dist, float curve,
                  float halfW, float halfH, float* su, float* sv);

// For the Status page: the raster's size, how long the last one took on the
// CPU, and the composite's measured GPU price per eye (0 until measured).
bool menuPanelStats(int* w, int* h, double* lastMs, float* gpuMs);

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
