// dinput.dll proxy: forwards the 7 exports of the system dinput.dll.
// DisplaySystem.dll imports DirectInputCreateA from dinput.dll, so placing this
// DLL next to AnarchyOnline.exe loads the mod into the game process only.
#include <windows.h>

#include <unknwn.h>

namespace {

HMODULE g_real = nullptr;

FARPROC Real(const char* name)
{
    if (!g_real) {
        wchar_t path[MAX_PATH];
        UINT n = GetSystemDirectoryW(path, MAX_PATH); // SysWOW64 for a 32-bit process
        if (n == 0 || n >= MAX_PATH - 12)
            return nullptr;
        lstrcatW(path, L"\\dinput.dll");
        g_real = LoadLibraryW(path);
        if (!g_real)
            return nullptr;
    }
    return GetProcAddress(g_real, name);
}

template <typename Fn>
Fn RealFn(const char* name)
{
    return reinterpret_cast<Fn>(Real(name));
}

} // namespace

extern "C" {

HRESULT WINAPI Proxy_DirectInputCreateA(HINSTANCE hinst, DWORD ver, void** out, IUnknown* outer)
{
    auto fn = RealFn<HRESULT(WINAPI*)(HINSTANCE, DWORD, void**, IUnknown*)>("DirectInputCreateA");
    return fn ? fn(hinst, ver, out, outer) : E_FAIL;
}

HRESULT WINAPI Proxy_DirectInputCreateW(HINSTANCE hinst, DWORD ver, void** out, IUnknown* outer)
{
    auto fn = RealFn<HRESULT(WINAPI*)(HINSTANCE, DWORD, void**, IUnknown*)>("DirectInputCreateW");
    return fn ? fn(hinst, ver, out, outer) : E_FAIL;
}

HRESULT WINAPI Proxy_DirectInputCreateEx(HINSTANCE hinst, DWORD ver, REFIID riid, void** out, IUnknown* outer)
{
    auto fn = RealFn<HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, void**, IUnknown*)>("DirectInputCreateEx");
    return fn ? fn(hinst, ver, riid, out, outer) : E_FAIL;
}

HRESULT WINAPI Proxy_DllCanUnloadNow()
{
    auto fn = RealFn<HRESULT(WINAPI*)()>("DllCanUnloadNow");
    return fn ? fn() : S_FALSE;
}

HRESULT WINAPI Proxy_DllGetClassObject(REFCLSID clsid, REFIID riid, void** out)
{
    auto fn = RealFn<HRESULT(WINAPI*)(REFCLSID, REFIID, void**)>("DllGetClassObject");
    return fn ? fn(clsid, riid, out) : E_FAIL;
}

HRESULT WINAPI Proxy_DllRegisterServer()
{
    auto fn = RealFn<HRESULT(WINAPI*)()>("DllRegisterServer");
    return fn ? fn() : E_FAIL;
}

HRESULT WINAPI Proxy_DllUnregisterServer()
{
    auto fn = RealFn<HRESULT(WINAPI*)()>("DllUnregisterServer");
    return fn ? fn() : E_FAIL;
}

} // extern "C"
