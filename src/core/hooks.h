#pragma once

#include <cstdint>

namespace prk {

// Scan a loaded module's image for a "55 8B ?? ..." pattern.
// Returns the address only if it matches exactly once, otherwise nullptr (and logs).
uint8_t* FindUnique(const char* module, const char* pattern, const char* what);

bool HooksInit();

// Create + enable an inline hook. Logs and returns false on failure (target left untouched).
bool InstallHook(const char* what, void* target, void* detour, void** original);

// Detour and original usually differ in calling convention (__fastcall detour standing in
// for a __thiscall original), so they are separate template types.
template <typename Detour, typename Original>
bool InstallHook(const char* what, void* target, Detour detour, Original* original)
{
    return InstallHook(what, target, reinterpret_cast<void*>(detour), reinterpret_cast<void**>(original));
}

} // namespace prk
