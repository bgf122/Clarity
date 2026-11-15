#pragma once

// The buff frame's look, painted in software (no game calls: tools/preview draws it too). The game's own
// icon textures go over it, one per cell; this paints round them: each icon's border (a thin ring in the
// colour the game gives the nano's kind: self, team, friendly, hostile), the time left under it, and - unlocked - a panel showing the frame's area with a resize grip.
// Sizes are pixels.
#include "../../core/canvas.h"

#include <string>
#include <vector>

namespace prk::buffs::paint {

struct Buff {
    bool hostile = false;   // a debuff
    uint32_t rgb = 0xC8CDD2; // the border: the nano kind's colour
    std::wstring time;      // "45m", "30s", "" = no end
    bool expiring = false;  // under 10 seconds left
};

struct Metrics {
    int iconW = 40;  // an icon's size: the game's nano icons are 40 x 24
    int iconH = 24;
    int border = 1;  // round each icon
    int gap = 4;     // between cells
    int timeH = 13;  // the line under each icon
    int pad = 6;     // the frame's edge -> the cells
    int rowGap = 6;  // the buffs -> the debuffs
    int grip = 12;   // the resize grip (unlocked)
    int CellW() const { return iconW + 2 * border + gap; }
    int CellH() const { return iconH + 2 * border + timeH + gap; }
};

Metrics MetricsFor(int iconW, int iconH);

// Where each icon goes (its top-left, inside its border) in a w x h frame: the buffs in rows from the top
// left (rightToLeft: from the top right, as WoW's), then the debuffs from a new row. Only what fits is placed.
struct Layout {
    int w = 0, h = 0;
    std::vector<POINT> buffAt, debuffAt;
};
Layout Arrange(const Metrics& m, int w, int h, int buffs, int debuffs, bool rightToLeft = false);

// The smallest frame: one cell.
void MinSize(const Metrics& m, int* w, int* h);

bool EnsureFonts(float scale);

// The frame: (unlocked) its panel and grip, then each placed icon's border and time.
void Paint(gfx::Canvas& c, const Metrics& m, const Layout& l, const std::vector<Buff>& buffs, const std::vector<Buff>& debuffs,
           bool unlocked);

// "2h", "45m", "30s" (WoW's short form); "" for a nano with no end.
std::wstring TimeLeft(int centiseconds);

} // namespace prk::buffs::paint
