// The game's timer bars, movable.
//
// GUI.dll's TimerSystemModule_t makes a bar for each running action: CreateTimer(int, Identity const&,
// char const*, unsigned) (a TimerBar_c) and the private CreateTimer(weak_ptr<TimerEntry_t const>, Identity
// const&, char const*, unsigned) (a GameTimerBar_c, from StartTimerBar). Both take the first free of 20
// slots (FindNextFreePos) and keep the bar in a std::list at +0x28 (FrameProcess updates them and deletes
// the finished). A bar (TimerBarBase_c, GUI 0x100512AE): +0x04 its RenderWindow_t (128x16, the PowerBar_t
// and the name drawn in it), +0x18 its slot. The window is placed once, at (40, (slot + 2) * 20) display
// pixels (RenderSprite_t vertices are pixels), and never moved again by the game. So Clarity hooks the
// action timers' CreateTimer (GameTimerBar_c) and moves each new bar's window (RenderWindow_t::Reposition)
// to the stack's place: [Bars] TimersX / TimersY (slot 0's top-left, display pixels; -1 = the game's 40,
// 40), slot n 20 lower. The public CreateTimer's TimerBar_c is the logout countdown (FlowControlModule_t::
// CampStartedMessage), which the game centres on the screen; it's left where it is.
// Unlocked ([Bars] TimersLocked=0), the stack shows a bar of its own to drag even with no action running:
// a TimerBar_c made with the public CreateTimer (as the logout one: layer 7000 as the action bars, never
// finishing by itself) and removed with DeleteTimer when locked. A left press on any bar of the stack drags
// it; the place is saved on release.
#include "timers.h"

#include "../../core/config.h"
#include "../../core/game.h"
#include "../../core/hooks.h"
#include "../../core/log.h"
#include "../../core/wndproc.h"

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>

namespace prk::timers {
namespace {

struct IPoint {
    int x, y;
};
struct WeakPtr { // boost::weak_ptr by value: handed on as it is (the callee releases it)
    void* px;
    void* pn;
};

using InstanceFn = void*(__cdecl*)();
using CreateFn = void*(__thiscall*)(void* self, int layer, const game::Identity* id, const char* name, unsigned color);
using DeleteFn = void(__thiscall*)(void* self, void* bar);
using CreateGameFn = void*(__thiscall*)(void* self, WeakPtr entry, const game::Identity* id, const char* name,
                                        unsigned color);
using RepositionFn = void(__thiscall*)(void* window, const IPoint* pos);
using IntFn = int(__thiscall*)(const void* self);

InstanceFn TimerSystem;  // TimerSystemModule_t::GetInstanceIfAny
CreateFn CreateTimer;    // TimerSystemModule_t::CreateTimer(int layer, Identity const&, char const*, unsigned)
DeleteFn DeleteTimer;    // TimerSystemModule_t::DeleteTimer(TimerBarBase_c*)
RepositionFn Reposition; // RenderWindow_t::Reposition
IntFn WindowX, WindowY, WindowW, WindowH;
InstanceFn DisplayInstance; // DisplaySystem_t::GetInstance
IntFn DisplayW, DisplayH;
CreateGameFn g_origCreateGame = nullptr;

constexpr int kListHead = 0x28;   // TimerSystemModule_t: std::list<TimerBarBase_c*>
constexpr int kBarWindow = 0x04;  // TimerBarBase_c: RenderWindow_t*
constexpr int kBarSlot = 0x18;    // TimerBarBase_c: its slot (0-19)
constexpr int kSlots = 20;
constexpr int kDefaultX = 40, kDefaultY = 40, kSlotPx = 20; // as the game (GUI 0x100512AE)
constexpr int kBarLayer = 7000;                              // the action bars' (CreateTimer, GUI 0x1005195B)
constexpr const char* kPlaceholderName = "Timer bars - drag";
constexpr unsigned kPlaceholderRgb = 0xFFC83C;

template <typename T>
bool Resolve(T& fn, const char* module, const char* name)
{
    HMODULE m = GetModuleHandleA(module);
    fn = m ? reinterpret_cast<T>(GetProcAddress(m, name)) : nullptr;
    if (!fn)
        Log("[timers] missing export %s!%s", module, name);
    return fn != nullptr;
}

template <typename T>
T At(const void* p, int offset)
{
    T v;
    memcpy(&v, static_cast<const uint8_t*>(p) + offset, sizeof(T));
    return v;
}

// Slot 0's top-left (display pixels).
IPoint g_anchor{kDefaultX, kDefaultY};
float g_xSeen = -2.0f, g_ySeen = -2.0f; // [Bars] TimersX / TimersY as last taken

IPoint ConfigAnchor()
{
    const BarsConfig& b = Cfg().bars;
    return IPoint{b.timersX >= 0.0f ? static_cast<int>(b.timersX) : kDefaultX,
                  b.timersY >= 0.0f ? static_cast<int>(b.timersY) : kDefaultY};
}

const void* g_gameBarVtable = nullptr; // GameTimerBar_c's, from the first one made
void* g_placeholder = nullptr;         // our TimerBar_c while unlocked

// Bars of the stack: the action bars and our placeholder (not the logout countdown).
bool InStack(const void* bar)
{
    return bar && (bar == g_placeholder || (g_gameBarVtable && *static_cast<const void* const*>(bar) == g_gameBarVtable));
}

void Place(void* bar)
{
    if (!InStack(bar))
        return;
    void* window = At<void*>(bar, kBarWindow);
    const int slot = At<int>(bar, kBarSlot);
    if (!window || slot < 0 || slot >= kSlots)
        return;
    const IPoint pos{g_anchor.x, g_anchor.y + slot * kSlotPx};
    if (WindowX(window) != pos.x || WindowY(window) != pos.y)
        Reposition(window, &pos);
}

constexpr UINT_PTR kTimerId = 0xC1A9;
constexpr UINT kTickMs = 100;
bool g_timerSet = false;
bool g_dragging = false;
IPoint g_dragMouse{}, g_dragAnchor{};
bool g_logged = false;
void EndDrag();

template <typename F>
void ForEachBar(F f)
{
    void* tsm = TimerSystem();
    if (!tsm)
        return;
    struct Node {
        Node* next;
        Node* prev;
        void* value;
    };
    Node* head = At<Node*>(tsm, kListHead);
    int guard = 0;
    for (Node* n = head ? head->next : nullptr; n && n != head && guard < 256; n = n->next, ++guard)
        f(n->value);
}

void PlaceAll()
{
    ForEachBar([](void* bar) { Place(bar); });
}

void* __fastcall Hook_CreateGame(void* self, void* /*edx*/, WeakPtr entry, const game::Identity* id, const char* name,
                                 unsigned color)
{
    void* bar = g_origCreateGame(self, entry, id, name, color);
    if (bar && !g_gameBarVtable)
        g_gameBarVtable = *static_cast<void**>(bar);
    Place(bar);
    return bar;
}

bool Listed(const void* bar)
{
    bool found = false;
    ForEachBar([&](void* b) { found = found || b == bar; });
    return found;
}

// Unlocked: our bar to drag, made when there's none (or the game dropped it, e.g. on a zone change);
// locked: removed.
void UpdatePlaceholder()
{
    void* tsm = TimerSystem();
    if (g_placeholder && (!tsm || !Listed(g_placeholder)))
        g_placeholder = nullptr; // the game deleted it
    const bool want = tsm && !Cfg().bars.timersLocked && game::ClientInstance() != 0;
    if (want && !g_placeholder) {
        const game::Identity none{0, 0};
        g_placeholder = CreateTimer(tsm, kBarLayer, &none, kPlaceholderName, kPlaceholderRgb);
        Place(g_placeholder);
    } else if (!want && g_placeholder) {
        if (g_dragging)
            EndDrag();
        DeleteTimer(tsm, g_placeholder);
        g_placeholder = nullptr;
    }
}

// Client-area pixels to display pixels (the back buffer the bars are drawn in).
void ToDisplay(HWND hwnd, LPARAM lp, int* x, int* y)
{
    RECT rc{};
    GetClientRect(hwnd, &rc);
    const int cw = rc.right - rc.left, ch = rc.bottom - rc.top;
    const void* ds = DisplayInstance();
    const int dw = ds ? DisplayW(ds) : cw, dh = ds ? DisplayH(ds) : ch;
    const int mx = static_cast<short>(LOWORD(lp)), my = static_cast<short>(HIWORD(lp));
    *x = cw > 0 ? mx * dw / cw : mx;
    *y = ch > 0 ? my * dh / ch : my;
}

bool OverBar(int x, int y)
{
    bool over = false;
    ForEachBar([&](void* bar) {
        void* w = InStack(bar) ? At<void*>(bar, kBarWindow) : nullptr;
        if (!w)
            return;
        const int bx = WindowX(w), by = WindowY(w);
        if (x >= bx && y >= by && x < bx + WindowW(w) && y < by + WindowH(w))
            over = true;
    });
    return over;
}

void EndDrag()
{
    if (!g_dragging)
        return;
    g_dragging = false;
    if (GetCapture())
        ReleaseCapture();
    const std::wstring& path = ConfigPath();
    wchar_t v[32];
    swprintf_s(v, L"%d", g_anchor.x);
    WritePrivateProfileStringW(L"Bars", L"TimersX", v, path.c_str());
    swprintf_s(v, L"%d", g_anchor.y);
    WritePrivateProfileStringW(L"Bars", L"TimersY", v, path.c_str());
    // Until Clarity.ini is read again its old place isn't a change to take.
    g_xSeen = Cfg().bars.timersX;
    g_ySeen = Cfg().bars.timersY;
}

bool OnMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, LRESULT* result)
{
    if (!g_timerSet)
        g_timerSet = SetTimer(hwnd, kTimerId, kTickMs, nullptr) != 0;
    switch (msg) {
    case WM_TIMER: {
        if (wp != kTimerId)
            return false;
        UpdatePlaceholder();
        const BarsConfig& b = Cfg().bars;
        if (!g_dragging && (b.timersX != g_xSeen || b.timersY != g_ySeen)) { // a new place in Clarity.ini
            g_xSeen = b.timersX;
            g_ySeen = b.timersY;
            g_anchor = ConfigAnchor();
            PlaceAll();
        }
        *result = 0;
        return true;
    }
    case WM_LBUTTONDOWN: {
        if (g_dragging || Cfg().bars.timersLocked || (wp & (MK_RBUTTON | MK_SHIFT | MK_CONTROL)))
            return false;
        int x, y;
        ToDisplay(hwnd, lp, &x, &y);
        if (!OverBar(x, y))
            return false;
        g_dragging = true;
        g_dragMouse = {x, y};
        g_dragAnchor = g_anchor;
        SetCapture(hwnd);
        if (!g_logged) {
            g_logged = true;
            Log("[timers] dragging the timer bars (at %d,%d)", g_anchor.x, g_anchor.y);
        }
        *result = 0;
        return true;
    }
    case WM_MOUSEMOVE: {
        if (!g_dragging)
            return false;
        int x, y;
        ToDisplay(hwnd, lp, &x, &y);
        const void* ds = DisplayInstance();
        const int dw = ds ? DisplayW(ds) : 1024, dh = ds ? DisplayH(ds) : 768;
        g_anchor.x = std::clamp(g_dragAnchor.x + x - g_dragMouse.x, 0, std::max(0, dw - 128));
        g_anchor.y = std::clamp(g_dragAnchor.y + y - g_dragMouse.y, 0, std::max(0, dh - kSlotPx));
        PlaceAll();
        *result = 0;
        return true;
    }
    case WM_LBUTTONUP:
        if (!g_dragging)
            return false;
        EndDrag();
        *result = 0;
        return true;
    case WM_CAPTURECHANGED:
    case WM_KILLFOCUS:
        EndDrag();
        return false;
    default:
        return false;
    }
}

} // namespace

bool Install()
{
    const char* gui = "GUI.dll";
    bool ok = true;
    ok &= Resolve(TimerSystem, gui, "?GetInstanceIfAny@TimerSystemModule_t@@SAPAV1@XZ");
    ok &= Resolve(CreateTimer, gui, "?CreateTimer@TimerSystemModule_t@@QAEPAVTimerBar_c@@HABVIdentity_t@@PBDI@Z");
    ok &= Resolve(DeleteTimer, gui, "?DeleteTimer@TimerSystemModule_t@@QAEXPAVTimerBarBase_c@@@Z");
    ok &= Resolve(Reposition, gui, "?Reposition@RenderWindow_t@@QAEXABVIPoint@@@Z");
    ok &= Resolve(WindowX, gui, "?GetXPos@RenderWindow_t@@QBEHXZ");
    ok &= Resolve(WindowY, gui, "?GetYPos@RenderWindow_t@@QBEHXZ");
    ok &= Resolve(WindowW, gui, "?GetSizeX@RenderWindow_t@@QBEHXZ");
    ok &= Resolve(WindowH, gui, "?GetSizeY@RenderWindow_t@@QBEHXZ");
    ok &= Resolve(DisplayInstance, "DisplaySystem.dll", "?GetInstance@DisplaySystem_t@@SAPAV1@XZ");
    ok &= Resolve(DisplayW, "DisplaySystem.dll", "?GetDisplayWidth@DisplaySystem_t@@QBEHXZ");
    ok &= Resolve(DisplayH, "DisplaySystem.dll", "?GetDisplayHeight@DisplaySystem_t@@QBEHXZ");
    void* createGame = nullptr;
    ok &= Resolve(createGame, gui,
                  "?CreateTimer@TimerSystemModule_t@@AAEPAVGameTimerBar_c@@V?$weak_ptr@$$CBVTimerEntry_t@@@boost@@ABVIdentity_t@@PBDI@Z");
    if (!ok) {
        Log("[timers] disabled");
        return false;
    }
    if (!InstallHook("TimerSystemModule_t::CreateTimer (GameTimerBar_c)", createGame, &Hook_CreateGame, &g_origCreateGame))
        return false;
    g_anchor = ConfigAnchor();
    wndproc::Add(&OnMessage);
    Log("[timers] ready");
    return true;
}

} // namespace prk::timers
