#pragma once

// Floating combat text: constant on-screen size, outline/shadow, pop-in, eased rise,
// fade-out, stacking and crit emphasis, on top of the game's own _GfxControlFont_t effect.
namespace prk::fct {

bool Install();

// Applies settings that patch game code (called after Clarity.ini reloads).
void OnConfigChanged();

} // namespace prk::fct
