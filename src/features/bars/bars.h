#pragma once

// Health / nano and target panels in place of the game's bar windows ([Bars]). Two windows of the
// game's own GUI: yours (health over nano, percentages) and the target panel (the fight target, and
// the nano target - smaller below it when they differ, in its place when there's no fight target).
// Drawn by Clarity into GUI surfaces (SpriteInfo_t -> ViewSurface_c), dragged with the mouse, positions
// saved to Clarity.ini. The game's health, nano and target bar windows are hidden meanwhile.
namespace prk::bars {

// Resolves the GUI exports and adds the panels' handler (timer, dragging) to the game window.
bool Install();

} // namespace prk::bars
