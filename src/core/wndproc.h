#pragma once

#include <windows.h>

// The game window's procedure, chained onto whatever is there (the game's, or another mod's). Features
// add handlers that see each message first, on the game thread.
namespace prk::wndproc {

// Returns true to keep the message from the game, with *result as the procedure's answer.
using Handler = bool (*)(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, LRESULT* result);

// Init thread, before Tick hooks the window. Handlers run in the order added.
void Add(Handler handler);

// Called periodically from the init thread: hooks the game window once it exists.
void Tick();

// The game thread's id (0 until the window's first message).
DWORD GameThread();

} // namespace prk::wndproc
