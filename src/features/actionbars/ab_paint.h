#pragma once

// The action bars' look, painted in software (no game calls, so tools/preview can draw it too):
//   - the dock: Clarity's frame round the first shortcut bars, side by side (two to a line, the first two
//     on the bottom line), each with a page control on its outer end (the row the bar shows, arrows to step
//     it), and the XP and alien XP bars across the top;
//   - the slots' bezels, under the game's icons (the dock paints its own; a floating bar has a backing
//     window with just these);
//   - the key labels, over the icons (a window of their own above each bar).
// Sizes are pixels; `scale` is 1 when a slot is 36 pixels apart, as the game's are at 1 GUI unit a pixel.
#include "../../core/canvas.h"

#include <cstdint>
#include <string>
#include <vector>

namespace prk::actionbars::paint {

constexpr int kSlots = 10;

struct Metrics {
    float scale = 1.0f;
    float cellF = 36.0f; // from one slot to the next (fractional when the GUI is scaled)
    int cell = 36;       // its size rounded: a cell's box
    int icon = 32;       // the icon's size inside it
    int margin = 2;      // the cell's edge -> the icon: the bezel's room

    int inset;      // the dock's edge -> the slots' cells
    int rowGap;     // between the dock's rows of cells
    int pageGap;    // the last cell -> the page control
    int pageW;      // the page control's width
    int xpTopH;     // the XP bar, flush against the frame's top (the frame's top step overlaps it)
    int xpH;        // the alien XP bar under it
    int xpBetween;  // the XP bar -> the alien XP bar
    int xpGap;      // the last XP bar -> the cells
};

// The metrics for slots `cell` pixels apart with `icon`-pixel icons `margin` pixels into their cells
// (< 0: centred).
Metrics MetricsFor(float cell, float icon, float margin = -1.0f);

// The top-left of cell i of a row whose first cell is at (x, y).
void CellAt(const Metrics& m, bool vertical, int x, int y, int i, int* cx, int* cy);

struct Slot {
    bool filled = false;  // has a shortcut
    std::wstring key;     // the key that uses it ("1", "s2", "F3"), empty = none
};

struct Row {
    Slot slot[kSlots];
    int page = 0;         // the row of the bar shown, 0-based (labelled 1..9, 0 for the tenth)
    bool active = false;  // the bar the number keys use
    bool vertical = false;
};

struct Xp {
    bool shown = false;
    float fraction = -1.0f;  // < 0 unknown
    std::wstring text;       // drawn in the bar ("XP 61.7%  ·  1.2M / 2.0M")
    uint32_t rgb = 0xCBA60E;
    std::vector<uint32_t> shading; // top to bottom, empty = gloss
};

// The game's XP and alien XP bar fills (GFX_GUI_ACTIONVIEW_XPBAR / _ALIENXPBAR in the ndGUI and default
// skins, 9 pixels across): their colours across, the fallback when the skin can't be read.
extern const std::vector<uint32_t> kGameXpShading, kGameAlienShading;
constexpr uint32_t kGameXpRgb = 0xCBA60E, kGameAlienRgb = 0x27C005;

struct Dock {
    int rows = 2;            // the bars in it
    Row row[4];              // [0] = the first bar (bottom left)
    Xp xp, alien;
    bool hideEmpty = false;  // empty slots: no bezel (and no key label)
};

struct DockLayout {
    int w = 0, h = 0;
    int cellX[4] = {};       // each bar's first cell: left
    int rowY[4] = {};        // and top
    RECT page[4] = {};       // each row's page control: the upper half (up arrow) steps to the next row, the lower to the previous
    RECT xp = {}, alien = {}; // the XP bars (empty when not shown); the XP bar is flush against the frame's top
};

DockLayout LayoutDock(const Metrics& m, int rows, bool xpShown, bool alienShown);

bool EnsureFonts(float scale);

// The dock: frame, the rows' slot bezels, page controls and the XP bars.
void PaintDock(gfx::Canvas& c, const Metrics& m, const DockLayout& l, const Dock& d);

// Just the XP bars' area (for repainting it alone when only XP changed): what PaintDock paints there.
RECT XpArea(const Metrics& m, const DockLayout& l);

// One bar's slot bezels with its first cell's top-left at (x, y).
void PaintSlots(gfx::Canvas& c, const Metrics& m, int x, int y, const Row& r, bool hideEmpty);

// One bar's key labels (top right of each icon), its first cell's top-left at (x, y).
void PaintKeys(gfx::Canvas& c, const Metrics& m, int x, int y, const Row& r, bool hideEmpty);

// The size of one bar's cells (vertical bars stand up).
void RowSize(const Metrics& m, const Row& r, int* w, int* h);

// "1".."9", "0" for the tenth row.
std::wstring PageLabel(int page);

} // namespace prk::actionbars::paint
