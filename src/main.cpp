// Clarity entry point. Loaded as dinput.dll (see proxy/), so DllMain only spawns a thread;
// hooks go in once the game modules are loaded.
#include "core/config.h"
#include "core/game.h"
#include "core/hooks.h"
#include "core/log.h"
#include "features/fct/fct.h"
#include "features/hideui/hideui.h"
#include "features/nameplates/nameplates.h"
#include "features/settings/settings.h"
#include "version.h"

#include <windows.h>

namespace {

bool IsGameProcess()
{
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    const wchar_t* name = wcsrchr(path, L'\\');
    name = name ? name + 1 : path;
    return _wcsicmp(name, L"AnarchyOnline.exe") == 0;
}

bool WaitForModules(DWORD timeoutMs)
{
    const char* modules[] = {"Gamecode.dll", "DisplaySystem.dll", "randy31.dll"};
    const DWORD start = GetTickCount();
    for (;;) {
        bool all = true;
        for (const char* m : modules)
            all = all && GetModuleHandleA(m) != nullptr;
        if (all)
            return true;
        if (GetTickCount() - start > timeoutMs)
            return false;
        Sleep(100);
    }
}

DWORD WINAPI InitThread(void*)
{
    prk::LogInit();
    prk::Log("Clarity " CLARITY_VERSION " starting (built " __DATE__ " " __TIME__ ")");
    prk::LoadConfig();

    if (!WaitForModules(5 * 60 * 1000)) {
        prk::Log("game modules never loaded - giving up");
        return 0;
    }
    // Let other mods (AOReloaded waits for Utils.dll + game world) install their hooks first;
    // none of them touch our targets, this just keeps the logs easy to read.
    Sleep(2000);

    if (prk::HooksInit() && prk::game::Init()) {
        prk::fct::Install();
        prk::nameplates::Install();
        prk::hideui::Install();
        prk::settings::Install();
    } else {
        prk::Log("init failed - no features enabled");
    }

    for (;;) {
        Sleep(500);
        if (prk::ReloadConfigIfChanged()) {
            prk::fct::OnConfigChanged();
            prk::nameplates::OnConfigChanged();
        }
        prk::game::UpdateViewportSize();
        prk::nameplates::Tick();
        prk::settings::Flush();
    }
}

} // namespace

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, void*)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hinst);
        if (IsGameProcess()) {
            if (HANDLE h = CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr))
                CloseHandle(h);
        }
    }
    return TRUE;
}
