#pragma once

#include "../../core/game.h"

#include <cstdint>

// Modern nameplates on top of the game's own nametag Indicators (GUI TargetingModule_t):
// the game keeps deciding who gets a tag and where it is; we replace the 128/256x32 bitmap-font
// texture with a high-res one (level + name, org/title line, health bar), size it by distance
// within MinPx..MaxPx, and hide tags per type.
namespace prk::nameplates {

bool Install();

// Re-render all plates after settings changed (called after Clarity.ini reloads).
void OnConfigChanged();

// The health bar colour of a character's plate (tenths: health in 0.1 %, -1 = look it up), as set on
// the Nameplates page: the game's consider colours, one colour, or by health.
uint32_t HealthBarRgb(const game::Identity& id, int tenths);

// The game's consider colour for a character's level (green, yellow, red by level difference, grey
// below the range), for text: full brightness, white for those it can't rate.
uint32_t ConsiderTextRgb(const game::Identity& id);

// Top edge (window pixels, y down) of the character's visible plate, including room for its
// brackets; false if it has none on screen. Game thread only (floating text starts above it).
bool PlateTop(uint32_t type, uint32_t instance, float* top);

} // namespace prk::nameplates
