#pragma once

#include <windows.h>

// A key ([Interface] HideUiKey) that hides the game's interface - windows, chat, bars - and shows it
// again, keeping the nameplates and floating text (they are 3D, the interface is 2D).
namespace prk::hideui {

// Resolves the exports and adds the key's handler to the game window's procedure (core/wndproc).
bool Install();

} // namespace prk::hideui
