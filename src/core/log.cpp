#include "log.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>

extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace prk {

namespace {
CRITICAL_SECTION g_lock;
FILE* g_file = nullptr;
} // namespace

const std::wstring& ModuleDir()
{
    static std::wstring dir = [] {
        wchar_t path[MAX_PATH];
        GetModuleFileNameW(reinterpret_cast<HMODULE>(&__ImageBase), path, MAX_PATH);
        std::wstring s(path);
        return s.substr(0, s.find_last_of(L"\\/") + 1);
    }();
    return dir;
}

void LogInit()
{
    InitializeCriticalSection(&g_lock);
    std::wstring path = ModuleDir() + L"Clarity.log";
    g_file = _wfopen(path.c_str(), L"w");
}

void Log(const char* fmt, ...)
{
    if (!g_file)
        return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    EnterCriticalSection(&g_lock);
    fprintf(g_file, "%02d:%02d:%02d.%03d ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list args;
    va_start(args, fmt);
    vfprintf(g_file, fmt, args);
    va_end(args);
    fputc('\n', g_file);
    fflush(g_file);
    LeaveCriticalSection(&g_lock);
}

} // namespace prk
