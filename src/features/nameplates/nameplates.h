#pragma once

#include "../../core/game.h"

#include <cstdint>

// Nameplates on top of the game's own nametag Indicators (GUI TargetingModule_t): the game keeps
// deciding who gets a tag and where it is; we replace the 128/256x32 bitmap-font texture with a
// high-res framed plate (title and name, org line, a health bar on target plates), size it by
// distance within MinPx..MaxPx, and hide tags per type.
namespace prk::nameplates {

bool Install();

// Re-render all plates after settings changed (called after Clarity.ini reloads).
void OnConfigChanged();

// The game's consider colour for a character's level (green, yellow, red by level difference, grey
// below the range) for a health bar, softened as the plates' bars; grey for those it can't rate.
uint32_t ConsiderBarRgb(const game::Identity& id);

// Top edge (window pixels, y down) of the character's visible plate, including room for its
// brackets; false if it has none on screen. Game thread only (floating text starts above it).
bool PlateTop(uint32_t type, uint32_t instance, float* top);

} // namespace prk::nameplates
