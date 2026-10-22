#pragma once

#include <string>

namespace prk {

// Directory containing our DLL (the client folder), with trailing backslash.
const std::wstring& ModuleDir();

void LogInit();
void Log(const char* fmt, ...);

} // namespace prk
