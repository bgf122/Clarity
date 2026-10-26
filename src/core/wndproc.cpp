#include "wndproc.h"

#include "game.h"
#include "log.h"

#include <atomic>

namespace prk::wndproc {

namespace {

constexpr int kMaxHandlers = 8;
Handler g_handlers[kMaxHandlers];
std::atomic<int> g_count{0};

WNDPROC g_prev = nullptr;
HWND g_hooked = nullptr;
bool g_unicode = false;
std::atomic<DWORD> g_gameThread{0};

LRESULT CALLBACK Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    g_gameThread.store(GetCurrentThreadId(), std::memory_order_relaxed);
    const int n = g_count.load(std::memory_order_acquire);
    for (int i = 0; i < n; ++i) {
        LRESULT result = 0;
        if (g_handlers[i](hwnd, msg, wp, lp, &result))
            return result;
    }
    return g_unicode ? CallWindowProcW(g_prev, hwnd, msg, wp, lp) : CallWindowProcA(g_prev, hwnd, msg, wp, lp);
}

} // namespace

void Add(Handler handler)
{
    const int n = g_count.load();
    if (n >= kMaxHandlers)
        return;
    g_handlers[n] = handler;
    g_count.store(n + 1, std::memory_order_release);
}

void Tick()
{
    HWND hwnd = game::GameWindow();
    if (!hwnd || hwnd == g_hooked || g_count.load() == 0)
        return;
    g_unicode = IsWindowUnicode(hwnd) != FALSE;
    g_hooked = hwnd;
    const LONG_PTR proc = reinterpret_cast<LONG_PTR>(&Proc);
    g_prev = reinterpret_cast<WNDPROC>(g_unicode ? SetWindowLongPtrW(hwnd, GWLP_WNDPROC, proc)
                                                 : SetWindowLongPtrA(hwnd, GWLP_WNDPROC, proc));
    Log("[wnd] game window %p hooked (previous procedure %p, %d handlers)", hwnd, g_prev, g_count.load());
}

DWORD GameThread()
{
    return g_gameThread.load(std::memory_order_relaxed);
}

} // namespace prk::wndproc
