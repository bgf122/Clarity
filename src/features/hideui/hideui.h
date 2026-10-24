#pragma once

#include <windows.h>

// A key ([Interface] HideUiKey) that hides the game's interface - windows, chat, bars - and shows it
// again, keeping the nameplates and floating text (they are 3D, the interface is 2D).
namespace prk::hideui {

bool Install();

// From the game window's procedure (the game thread). True if the message was the key.
bool HandleMessage(UINT msg, WPARAM wp, LPARAM lp);

} // namespace prk::hideui
