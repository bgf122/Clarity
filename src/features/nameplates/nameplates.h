#pragma once

// Modern nameplates on top of the game's own nametag Indicators (GUI TargetingModule_t):
// the game keeps deciding who gets a tag and where it is; we replace the 128/256x32 bitmap-font
// texture with a high-res one (level + name, org/title line, health bar), size it by distance
// within MinPx..MaxPx, and hide tags per type.
namespace prk::nameplates {

bool Install();

// Re-render all plates after settings changed (called after Clarity.ini reloads).
void OnConfigChanged();

// Called periodically from the init thread: hooks the game window for click-to-target.
void Tick();

} // namespace prk::nameplates
